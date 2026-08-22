#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""HIL: slots become files - the path-based active context.

Design: .superpowers/sdd/projects-wave-2-bench-notes/design-slots.md
(§0 the model, §2 persistence/boot, §3 auto-save + the retired trap, §5 UI).

What this proves on real hardware:

  1. ADOPT + NO-CLOBBER - loading an arbitrary slot YAML makes THAT FILE the
     active context (ACTIVE_SLOT:-1 + ACTIVE_PATH:<it>), later edits auto-save
     back into it, and no numbered slot file is touched. Before this wave the
     context stayed on the previous NUMBER and the idle auto-save wrote the
     loaded file's content into that slot.
  2. TRAP REGRESSION - /slots/slot_555.yaml. The old basename matcher
     toInt()'d "_555" to 0 and adopted slot 0, so the auto-save clobbered
     /slots/slot0.yaml. The strict full-path matcher makes it a FILE context.
  3. BOOT PERSISTENCE - the active file survives a reboot (boot_mode 1, the
     default), and boot_mode 0 pins boot_slot instead.
  4. LEAVE AND RETURN - '<n' out of a file context lands on slot n and
     last_active.txt follows.
  5. MSC ROUND-TRIP - skipped headless (needs a host mount); see the note.
  6. TEMP-8 NESTING - a calibration-style temp slot entered from a file
     context returns to that file.
  7. jl_switch_slot FROM a file context - loads slot n's real content instead
     of flipping netSlot with the file's bridges still live (the clobber
     vector that had to die in the same commit as the sentinel).

NOT here: the run-file allocator (<dir>_<N>.yaml numbering, open-most-recent,
no-reuse) and the provisioning-with-run-files-present assertions. Those belong
to the LAUNCHER task's suite (task 5) because projectBeginRun is what creates
run files, and it does not exist yet.

Port 1 must be free (close the jumperless client) - this suite drives it.
"""

import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jl import (jl_exec, parse_kv, port1_command, port1_path, check, finish,
                board_state_capture, board_state_restore,
                active_context, restore_context, reboot_board)

_csi = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")

TMP_PROJ = "/projects/slotfiles"
RUN_FILE = "/projects/slotfiles/slotfiles_1.yaml"
TEMPLATE_FILE = "/projects/slotfiles/wiring.yaml"
BAD_FILE = "/projects/slotfiles/slotfiles_bad.yaml"
TRAP_FILE = "/slots/slot_555.yaml"
LAST_ACTIVE = "/slots/last_active.txt"

# A minimal but REAL slot YAML: two bridges plus a power section, so the
# adopt/reload assertions have something observable on the hardware.
RUN_YAML = """version: 2
sourceOfTruth: bridges

bridges:
  - {n1: 11, n2: 12, dup: 1}
  - {n1: 13, n2: 14, dup: 1}

power:
  topRail: 0.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00
"""

# A TEMPLATE, carrying a guide: section. guide:/meta: are swallowed on parse
# and never re-emitted by toYAML, so an auto-save over a template does not
# merely overwrite it - it DESTROYS those sections. That is what the
# read-only guard exists to prevent, and what this fixture detects.
TEMPLATE_YAML = """version: 2
sourceOfTruth: bridges

bridges:
  - {n1: 31, n2: 32, dup: 1}

power:
  topRail: 0.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00

guide:
  - text: "step one"
  - text: "step two"
"""

# A file that PARSES but fails validation: topRail 12.0 is outside the +/-8 V
# PowerState::validate band, so fromYAML returns false AFTER it has already
# clear()ed and repopulated globalState. That is the second failure mode the
# original atomicity contract wrongly assumed could not happen.
BAD_YAML = """version: 2
sourceOfTruth: bridges

bridges:
  - {n1: 51, n2: 52, dup: 1}

power:
  topRail: 12.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00
"""

# The trap file's content is deliberately DIFFERENT so "which file loaded"
# is answerable from the bridges alone.
TRAP_YAML = """version: 2
sourceOfTruth: bridges

bridges:
  - {n1: 21, n2: 22, dup: 1}

power:
  topRail: 0.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00
"""


def read_config():
    """The board's /config.txt, verbatim."""
    return jl_exec("""
f = jfs.open("/config.txt", "r")
data = ""
while True:
    chunk = jfs.read(f, 512)
    if not chunk:
        break
    data += chunk
jfs.close(f)
print(data)
""", timeout=20)


def read_device_file(path):
    """(exists, contents) for a file on the board."""
    out = jl_exec(f"""
p = {path!r}
if fs_exists(p):
    print("EXISTS=1")
    print("BEGIN")
    print(fs_read(p))
    print("END")
else:
    print("EXISTS=0")
""", timeout=25)
    if "EXISTS=1" not in out:
        return False, None
    try:
        body = out.split("BEGIN", 1)[1].rsplit("END", 1)[0]
    except IndexError:
        return True, None
    return True, body.strip()


# --- File-manager driving (phase 6c) -----------------------------------------
# The delete-the-active-file crash was reported through the Files browser, so
# one leg of that needle has to actually BE the Files browser. These helpers
# drive it over port 1 by reading the rendered screen ("Current Path: X |
# Files: n | Selected: i/n") rather than counting blind keystrokes - a missed
# keypress then fails loudly instead of silently opening some other file (ask
# how we know).
_FM_CSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78=]")
_FM_STATUS = re.compile(
    r"Current Path:\s*(\S+)\s*\|\s*Files:\s*(\d+)\s*\|\s*Selected:\s*(\d+)/(\d+)")
_FM_DOWN = b"\x1b[B"
_FM_UP = b"\x1b[A"


def _fm_vis(b):
    return _FM_CSI.sub("", b.decode(errors="replace"))


def _fm_drain(ser, secs):
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        c = ser.read(4096)
        if c:
            buf += c
            end = max(end, time.time() + 0.25)   # keep reading while it talks
    return buf


def _fm_send(ser, data, wait=0.9):
    ser.write(data)
    ser.flush()
    return _fm_drain(ser, wait)


def _fm_state(ser):
    """Force a redraw with 'r' and return (path, selected, count) or None."""
    text = _fm_vis(_fm_send(ser, b"r", 1.2))
    m = None
    for m in _FM_STATUS.finditer(text):
        pass
    return (m.group(1), int(m.group(3)), int(m.group(4))) if m else None


def _fm_order(listing, path):
    """FileManager::refreshListing's ordering: '..' first, then directories,
    then files, each alphabetical; dotfiles skipped."""
    dirs = sorted(n.rstrip("/") for n in listing if n.endswith("/"))
    files = sorted(n for n in listing
                   if not n.endswith("/") and not n.startswith("."))
    return ([".."] if path != "/" else []) + dirs + files


def _fm_goto(ser, target):
    st = _fm_state(ser)
    for _ in range(60):
        if st is None or st[1] == target:
            return st
        _fm_send(ser, _FM_DOWN if target > st[1] else _FM_UP, 0.5)
        st = _fm_state(ser)
    return None


print("=== test_slot_files: the active context is a path ===")

# --- 0. Snapshot the bench ---------------------------------------------------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

orig_slot, orig_path = active_context(1.5)
check(orig_slot is not None, "queried active context ('Q')")
check(orig_path is not None, "'Q' always emits ACTIVE_PATH")
print(f"  info: active context slot={orig_slot} path={orig_path}")

# Everything below MUTATES the board. The finally puts the bench back: the
# test files off the flash, the original context re-loaded, boot_mode restored,
# and the state snapshot re-applied. Phase 0 stays outside so the finally has
# orig_slot/orig_path/snapshot bound before it can restore anything.
slot0_existed, slot0_before = read_device_file("/slots/slot0.yaml")
slot2_existed, slot2_before = read_device_file("/slots/slot2.yaml")
boot_mode_before = None
boot_slot_before = None

try:
    # Start from a known NUMBERED context so "the context moved" is
    # unambiguous. Slot 2 is the workspace; slot 0 is the trap's victim and is
    # only ever read.
    port1_command("<2", 4.0)
    time.sleep(1.5)
    slot_now, path_now = active_context(1.5)
    check(slot_now == 2, f"bounced to slot 2 (got {slot_now})")
    check(path_now == "/slots/slot2.yaml",
          f"a NUMBERED slot also reports its canonical path (got {path_now!r})")

    # --- 1. Adopt: an arbitrary slot YAML becomes the context -------------
    out = jl_exec(f"""
for d in ("/projects", {TMP_PROJ!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({TMP_PROJ!r}) else 0)
print("wrote=", 1 if fs_write({RUN_FILE!r}, {RUN_YAML!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("projdir") == 1, f"created {TMP_PROJ} on the board")
    check(vals.get("wrote") == 1, f"pushed {RUN_FILE}")

    # Re-read slot 2 right before the load: it is the context we are leaving,
    # and therefore the file the OLD behavior would have written the run
    # file's content into.
    slot2_pre_existed, slot2_pre = read_device_file("/slots/slot2.yaml")

    out = jl_exec(f"print('loaded=', 1 if load_project({RUN_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, f"loaded {RUN_FILE} by literal path")
    time.sleep(1.5)

    slot_a, path_a = active_context(1.5)
    check(slot_a == -1,
          f"'Q' reports ACTIVE_SLOT:-1 for a file context (got {slot_a})")
    check(path_a == RUN_FILE,
          f"'Q' reports ACTIVE_PATH:{RUN_FILE} (got {path_a!r})")

    out = jl_exec("""
print("b1=", 1 if is_connected(11, 12) else 0)
print("b2=", 1 if is_connected(13, 14) else 0)
""")
    vals = parse_kv(out)
    check(vals.get("b1") == 1 and vals.get("b2") == 1,
          "the run file's bridges (11-12, 13-14) are live on the hardware")

    # --- 2. The auto-save writes back to the FILE, not to a slot ----------
    # Mutate, wait past the idle flush gate, then read both the run file and
    # the slot we came from.
    jl_exec("connect(41, 42)", timeout=20)
    time.sleep(4.0)   # idle flush gate is ~750 ms of quiet; 4 s is generous

    run_exists, run_after = read_device_file(RUN_FILE)
    check(run_exists and run_after is not None and "41" in run_after,
          "the idle auto-save wrote the new bridge back INTO the run file")

    slot2_post_existed, slot2_post = read_device_file("/slots/slot2.yaml")
    check(slot2_post_existed == slot2_pre_existed and slot2_post == slot2_pre,
          "NO-CLOBBER: /slots/slot2.yaml (the context we left) is byte-identical")

    slot0_mid_existed, slot0_mid = read_device_file("/slots/slot0.yaml")
    check(slot0_mid_existed == slot0_existed and slot0_mid == slot0_before,
          "NO-CLOBBER: /slots/slot0.yaml is byte-identical")

    # --- 3. Leave and return ----------------------------------------------
    q = port1_command("<3", 4.0)
    time.sleep(1.5)
    check("SLOT_CHANGED:3" in q, f"'<3' announced SLOT_CHANGED:3 ({q.strip()[:80]!r})")
    slot_b, path_b = active_context(1.5)
    check(slot_b == 3, f"left the file context for slot 3 (got {slot_b})")
    check(path_b == "/slots/slot3.yaml",
          f"the path followed to slot 3's canonical file (got {path_b!r})")

    la_exists, la = read_device_file(LAST_ACTIVE)
    check(la_exists and la is not None and la.strip() == "/slots/slot3.yaml",
          f"last_active.txt tracked the switch (got {la!r})")

    # --- 4. The slot_555 trap regression ----------------------------------
    # "slot_555.yaml".toInt() on the old basename matcher -> 0 -> slot 0
    # adopted -> auto-save clobbers /slots/slot0.yaml. It must be a FILE.
    out = jl_exec(f"print('wrote=', 1 if fs_write({TRAP_FILE!r}, {TRAP_YAML!r}) else 0)",
                  timeout=25)
    check(parse_kv(out).get("wrote") == 1, f"pushed {TRAP_FILE}")

    slot0_trap_existed, slot0_trap_before = read_device_file("/slots/slot0.yaml")

    out = jl_exec(f"print('loaded=', 1 if load_project({TRAP_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, f"loaded {TRAP_FILE}")
    time.sleep(1.5)

    slot_t, path_t = active_context(1.5)
    check(slot_t == -1,
          f"TRAP: slot_555.yaml is a FILE context, not slot 0 (ACTIVE_SLOT={slot_t})")
    check(path_t == TRAP_FILE,
          f"TRAP: ACTIVE_PATH is {TRAP_FILE} (got {path_t!r})")

    out = jl_exec("print('t=', 1 if is_connected(21, 22) else 0)")
    check(parse_kv(out).get("t") == 1, "the trap file's own bridge (21-22) is live")

    jl_exec("connect(43, 44)", timeout=20)
    time.sleep(4.0)
    slot0_trap_after_exists, slot0_trap_after = read_device_file("/slots/slot0.yaml")
    check(slot0_trap_after_exists == slot0_trap_existed and
          slot0_trap_after == slot0_trap_before,
          "TRAP: /slots/slot0.yaml is byte-identical after loading slot_555.yaml "
          "and dirtying the state")
    trap_exists, trap_after = read_device_file(TRAP_FILE)
    check(trap_exists and trap_after is not None and "43" in trap_after,
          "TRAP: the edit landed in slot_555.yaml itself")

    # --- 5. jl_switch_slot FROM a file context ----------------------------
    # Two separate regressions here:
    #   (a) it used to flip netSlot and refresh WITHOUT loading, leaving the
    #       file's bridges live under slot n's number for the auto-save to
    #       write there;
    #   (b) it had no dirty pre-save, so the outgoing file context's unsaved
    #       edits were silently DISCARDED (loadSlot's internal cache flush
    #       drains cache entries, never a dirty in-RAM state).
    #
    # The fresh edit and the switch MUST be in ONE jl_exec snippet. Split
    # across two calls, the multi-second REPL gap lets the idle auto-save
    # write the edit on its own and the assertion below becomes vacuous -
    # which is exactly how the previous version of this check passed while
    # the bug was live.
    out = jl_exec("""
connect(47, 48)
print("prev=", switch_slot(2))
""", timeout=30)
    time.sleep(1.5)
    slot_s, path_s = active_context(1.5)
    check(slot_s == 2, f"switch_slot(2) landed on slot 2 (got {slot_s})")
    check(path_s == "/slots/slot2.yaml",
          f"switch_slot moved the path too (got {path_s!r})")
    out = jl_exec("""
print("trap_live=", 1 if is_connected(21, 22) else 0)
print("edit_live=", 1 if is_connected(43, 44) else 0)
""")
    vals = parse_kv(out)
    check(vals.get("trap_live") == 0 and vals.get("edit_live") == 0,
          "switch_slot ACTUALLY LOADED slot 2 - the file context's bridges "
          "(21-22, 43-44) are gone, not carried over under slot 2's number")

    # The dirty pre-save witness: 47-48 was connected in the SAME snippet as
    # the switch, with no idle window in between, so the only thing that can
    # have put it in the file is switch_slot's own call-site save.
    trap_exists2, trap_after2 = read_device_file(TRAP_FILE)
    check(trap_exists2 and trap_after2 is not None and "47" in trap_after2,
          "switch_slot saved the outgoing file context's UNSAVED edit (47-48) "
          "to its own file before switching")
    check(trap_after2 is not None and "43" in trap_after2,
          "...and the previously-flushed edit (43-44) is still there")

    # --- 6. Temp slot 8 nesting from a file context ------------------------
    # enterTemporarySlot captures the ORIGINAL as a (number, path) pair, and
    # exitTemporarySlot reloads from that PATH. Before this wave it reloaded
    # getSlotFilename(temporarySlotOriginal) - which for a file context would
    # be "/slots/slot-2.yaml", i.e. nothing - so an app launched from a run
    # file came back to an empty board with the run file's number lost.
    #
    # "Switch Calib" (calibrateProbeSwitchThresholds) is the drivable one: it
    # calls enterTemporarySlot(8) immediately and aborts on ANY serial key,
    # taking leaveApp() -> exitTemporarySlot(). Same worker+cancel-byte dance
    # test_projects.py uses for the launcher.
    out = jl_exec(f"print('loaded=', 1 if load_project({RUN_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, "re-entered the run-file context")
    time.sleep(1.5)
    slot_c, path_c = active_context(1.5)
    check(slot_c == -1 and path_c == RUN_FILE,
          f"back in the run-file context ({slot_c}, {path_c!r})")

    app = {}

    def _launch_switch_calib():
        try:
            app["out"] = jl_exec("run_app('Switch Calib')\nprint('returned=', 1)",
                                 timeout=45)
        except SystemExit as e:
            app["err"] = str(e)
        except Exception as e:                                   # pragma: no cover
            app["err"] = repr(e)

    import serial  # pyserial
    buf = ""
    worker = None
    ser = serial.Serial(port1_path(), 115200, timeout=0.05)
    try:
        # Prime the connection BEFORE the app opens - the firmware's
        # connection-init eats the first byte, and that byte would otherwise
        # be the abort key.
        ser.write(b"\r\n")
        ser.flush()
        quiet, overall = time.time(), time.time()
        while time.time() - overall < 4.0:
            if ser.read(4096):
                quiet = time.time()
            elif time.time() - quiet > 0.6:
                break
        ser.reset_input_buffer()

        worker = threading.Thread(target=_launch_switch_calib, daemon=True)
        worker.start()
        overall = time.time()
        last_send = 0.0
        deadline = time.time() + 35
        while time.time() < deadline and worker.is_alive():
            chunk = ser.read(4096)
            if chunk:
                buf += _csi.sub("", chunk.decode(errors="replace"))
            # Abort once the app has announced itself (it prints its banner as
            # soon as it starts), blind-aborting after 8 s so a missed banner
            # cannot wedge the board inside the app. Re-sent every 0.5 s.
            if ("Threshold Calibration" in buf or time.time() - overall > 8.0) \
                    and time.time() - last_send > 0.5:
                ser.write(b"\r")
                ser.flush()
                last_send = time.time()
        if worker.is_alive():
            worker.join(timeout=15)
    finally:
        ser.close()

    check(worker is not None and not worker.is_alive(),
          "run_app('Switch Calib') returned after the serial abort")
    if "err" in app:
        print(f"  info: app worker error: {app['err'][:300]}")
    time.sleep(2.0)

    slot_t8, path_t8 = active_context(2.0)
    check(slot_t8 == -1,
          f"TEMP-8: exiting the app returned to a FILE context (got {slot_t8}, "
          f"not 8 and not a number)")
    check(path_t8 == RUN_FILE,
          f"TEMP-8: ...to the SAME run file (got {path_t8!r})")
    out = jl_exec("""
print("b1=", 1 if is_connected(11, 12) else 0)
print("e=", 1 if is_connected(41, 42) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("b1") == 1 and vals.get("e") == 1,
          "TEMP-8: the run file's wiring was reloaded from its own path")

    # --- 6a. Atomic on PARSE failure, not just open failure ----------------
    # fromYAML ends in `return validate()`, and PowerState::validate rejects
    # any rail/DAC outside +/-8 V - so a file can parse far enough to REPLACE
    # globalState and still fail. The original contract assumed this was
    # unreachable; it is reached by exactly the flows this design endorses
    # (host-editing a run file over MSC, clicking a hand-written YAML).
    #
    # Without the fix, tracking stays on the prior NUMBERED slot while RAM
    # holds the foreign file's remnants, and the next dirty auto-save writes
    # them into that slot - with no -2 anywhere, so the loud saveSlot guard
    # never fires.
    port1_command("<2", 4.0)
    time.sleep(1.5)
    slot_pre, path_pre = active_context(1.5)
    check(slot_pre == 2, f"on slot 2 before the bad-parse needle (got {slot_pre})")
    slot2_bad_existed, slot2_bad_before = read_device_file("/slots/slot2.yaml")

    out = jl_exec(f"print('wrote=', 1 if fs_write({BAD_FILE!r}, {BAD_YAML!r}) else 0)",
                  timeout=25)
    check(parse_kv(out).get("wrote") == 1, f"pushed {BAD_FILE} (topRail 12.0)")

    out = jl_exec(f"print('loaded=', 1 if load_project({BAD_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 0,
          "loading an out-of-range-power YAML FAILS (validate rejects topRail 12.0)")
    time.sleep(1.5)

    slot_post, path_post = active_context(1.5)
    check(slot_post == 2 and path_post == "/slots/slot2.yaml",
          f"ATOMIC-ON-PARSE: the prior context is still fully active "
          f"(got {slot_post}, {path_post!r} - expected 2, /slots/slot2.yaml)")
    out = jl_exec("print('foreign=', 1 if is_connected(51, 52) else 0)")
    check(parse_kv(out).get("foreign") == 0,
          "ATOMIC-ON-PARSE: the failed file's bridge (51-52) is not live")

    # Idle window with NO mutation first: a failed load must not cause a write
    # at all. (fromYAML's own bridge-dropping sanitizer can call markDirty(),
    # so "nothing was dirtied" is a real thing to check, not a tautology.)
    time.sleep(4.0)
    slot2_idle_exists, slot2_idle = read_device_file("/slots/slot2.yaml")
    check(slot2_idle_exists == slot2_bad_existed and slot2_idle == slot2_bad_before,
          "ATOMIC-ON-PARSE: slot2.yaml is byte-identical after an idle window "
          "with no mutation - the failed load wrote nothing")

    # NOW dirty the restored context and let the idle auto-save run: it must
    # write slot 2's OWN content, not the failed file's remnants.
    jl_exec("connect(53, 54)", timeout=20)
    time.sleep(4.0)
    slot2_bad_after_exists, slot2_bad_after = read_device_file("/slots/slot2.yaml")
    check(slot2_bad_after_exists and slot2_bad_after is not None and
          "51" not in slot2_bad_after,
          "ATOMIC-ON-PARSE: slot2.yaml did NOT receive the failed file's content")
    check(slot2_bad_after is not None and "53" in slot2_bad_after,
          "ATOMIC-ON-PARSE: slot 2 auto-saved its own edit normally afterwards")

    # The OTHER restore branch: prior context is a FILE, not a numbered slot.
    # savedSlot == SLOT_FILE_CONTEXT takes the recursive loadSlotFromPath arm
    # instead of loadSlot, so it needs its own coverage - the two arms share
    # nothing but the capture.
    out = jl_exec(f"print('loaded=', 1 if load_project({RUN_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, "back in the run-file context for the path-prior branch")
    time.sleep(1.5)
    run_pre_exists, run_pre = read_device_file(RUN_FILE)

    out = jl_exec(f"print('loaded=', 1 if load_project({BAD_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 0, "the bad file fails from a FILE context too")
    time.sleep(1.5)

    slot_fp, path_fp = active_context(1.5)
    check(slot_fp == -1 and path_fp == RUN_FILE,
          f"ATOMIC-ON-PARSE (path prior): the run-file context is still active "
          f"(got {slot_fp}, {path_fp!r})")
    out = jl_exec("""
print("foreign=", 1 if is_connected(51, 52) else 0)
print("own=", 1 if is_connected(11, 12) else 0)
""")
    vals = parse_kv(out)
    check(vals.get("foreign") == 0 and vals.get("own") == 1,
          "ATOMIC-ON-PARSE (path prior): the run file's own wiring is live and "
          "the failed file's bridge is not - the prior context was re-loaded")

    jl_exec("connect(55, 56)", timeout=20)
    time.sleep(4.0)
    run_post_exists, run_post = read_device_file(RUN_FILE)
    check(run_post_exists and run_post is not None and "51" not in run_post,
          "ATOMIC-ON-PARSE (path prior): the run file did NOT receive the "
          "failed file's content")
    check(run_post is not None and "55" in run_post,
          "ATOMIC-ON-PARSE (path prior): the run file auto-saved its own edit "
          "normally afterwards")

    # --- 6b. Project TEMPLATES are read-only -------------------------------
    # Adoption made this reachable and the bench proved it destructive:
    # load_project("eeprom") pointed the context at the shipped template, the
    # state went dirty, and the idle auto-save rewrote it - losing the whole
    # guide: section, because toYAML is a wholesale rewrite that never
    # re-emits guide:/meta:. saveActiveSlot now REFUSES to write a
    # /projects/<dir>/wiring*.yaml. Run files (<dir>_<N>.yaml) do not match
    # the predicate and are unaffected - phases 1-2 above already proved that.
    out = jl_exec(f"print('wrote=', 1 if fs_write({TEMPLATE_FILE!r}, {TEMPLATE_YAML!r}) else 0)",
                  timeout=25)
    check(parse_kv(out).get("wrote") == 1, f"pushed {TEMPLATE_FILE}")
    tpl_existed, tpl_before = read_device_file(TEMPLATE_FILE)
    check(tpl_existed and tpl_before is not None and "guide:" in tpl_before,
          "the template fixture has a guide: section to lose")

    out = jl_exec(f"print('loaded=', 1 if load_project({TEMPLATE_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, "loaded the template (it still LOADS - only writes are refused)")
    time.sleep(1.5)
    slot_tp, path_tp = active_context(1.5)
    check(slot_tp == -1 and path_tp == TEMPLATE_FILE,
          f"the template is the active context ({slot_tp}, {path_tp!r})")

    jl_exec("connect(45, 46)", timeout=20)
    time.sleep(5.0)   # well past the idle flush gate

    tpl_after_exists, tpl_after = read_device_file(TEMPLATE_FILE)
    check(tpl_after_exists and tpl_after == tpl_before,
          "TEMPLATE PROTECTION: the template is byte-identical after dirtying "
          "the state - the auto-save was refused")
    check(tpl_after is not None and "guide:" in tpl_after,
          "TEMPLATE PROTECTION: the guide: section survived")

    la_exists, la = read_device_file(LAST_ACTIVE)
    check(la_exists and la is not None and la.strip() != TEMPLATE_FILE,
          f"TEMPLATE PROTECTION: a template never becomes the boot context "
          f"(last_active.txt = {la!r})")

    # --- 6c. DELETE THE ACTIVE RUN FILE OUT FROM UNDER THE CONTEXT ---------
    # THE BENCH CRASH (wave-2 §1.2, Kevin 2026-08-20): deleting the file that
    # IS the active context from the Files browser, then leaving the browser,
    # HardFaulted core 0 and rebooted. The crashlog decoded to _exit / abort
    # with CFSR=0 and HFSR.DEBUGEVT - a DELIBERATE abort(), not a wild
    # pointer. Nothing asserted this flow before, in either direction.
    #
    # The wave-2 contract is that it is HARMLESS: a save to a path IS a
    # create, so the next dirty auto-save re-creates the file. Both legs
    # below assert that contract AND that the board is still standing:
    #   (a) the plain path - remove the file, dirty the state, let the idle
    #       flush run: the file comes back, carrying the edit;
    #   (b) the FILES BROWSER path - delete it with the browser's own x/y and
    #       leave with CTRL+Q, which is exactly what crashed.
    # A reboot is caught three ways: the CDC node disappears, a fresh
    # [crashlog] line appears in the captured output (a clean run has none),
    # and 'Q' stops answering with the expected context.
    out = jl_exec(f"print('loaded=', 1 if load_project({RUN_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, "6c: on the run file to delete it")
    time.sleep(1.5)

    # (a) delete it from underneath, then dirty + idle-flush.
    out = jl_exec(f"""
jfs.remove({RUN_FILE!r})
print("gone=", 0 if fs_exists({RUN_FILE!r}) else 1)
connect(57, 58)
""", timeout=30)
    check(parse_kv(out).get("gone") == 1,
          "6c: the ACTIVE run file is deleted while it is the context")
    time.sleep(4.0)   # idle flush gate is ~750 ms; 4 s is generous
    out = jl_exec(f"print('back=', 1 if fs_exists({RUN_FILE!r}) else 0)", timeout=25)
    check(parse_kv(out).get("back") == 1,
          "6c: the auto-save RE-CREATED the deleted active file (a save is a create)")
    slot_d, path_d = active_context(2.5)
    check(slot_d == -1 and path_d == RUN_FILE,
          f"6c: the context survived the delete (slot={slot_d}, path={path_d!r})")
    _exists, body = read_device_file(RUN_FILE)
    check(body is not None and "n1: 57" in body and "n2: 58" in body,
          "6c: the re-created file carries the edit made after the delete")

    # (b) the same thing through the Files browser, ending in CTRL+Q.
    import serial  # pyserial (jl.py imports it the same way)

    def _listing(out_text):
        m = re.search(r"L=\s*\[(.*?)\]", out_text, re.S)
        return re.findall(r"'([^']*)'", m.group(1)) if m else []

    # EVERY directory listing has to be fetched BEFORE the browser opens: the
    # file manager owns Core 0 for its whole session, so the MicroPython REPL
    # on port 5 cannot answer while it is up (it times out waiting for the raw
    # REPL, which fails the suite mid-phase with the browser still on screen).
    root_order = _fm_order(_listing(jl_exec("print('L=', fs_listdir('/'))", timeout=25)), "/")
    proj_order = _fm_order(
        _listing(jl_exec("print('L=', fs_listdir('/projects'))", timeout=25)), "/projects")
    fixture = _fm_order(
        _listing(jl_exec(f"print('L=', fs_listdir({TMP_PROJ!r}))", timeout=25)), TMP_PROJ)

    hops = [("/", root_order, "projects"),
            ("/projects", proj_order, TMP_PROJ.rsplit("/", 1)[1])]

    fm_captured = ""
    fm_ok = True
    ser = serial.Serial(port1_path(), 115200, timeout=0.1)
    try:
        _fm_send(ser, b"\r\n", 2.0)
        opened = _fm_vis(_fm_send(ser, b"/\r\n", 4.0))
        if "Press Enter" in opened:
            opened += _fm_vis(_fm_send(ser, b"\r", 3.0))
        fm_captured += opened
        check("JUMPERLESS FILE MANAGER" in opened, "6c: the Files browser opened")

        st = _fm_state(ser)
        for _ in range(6):                        # climb to root
            if st is None or st[0] == "/":
                break
            _fm_send(ser, b".", 1.2)
            st = _fm_state(ser)
        for want_path, order, name in hops:
            st = _fm_state(ser)
            if st is None or st[0] != want_path or name not in order:
                fm_ok = False
                break
            if _fm_goto(ser, order.index(name) + 1) is None:
                fm_ok = False
                break
            _fm_send(ser, b"\r", 1.6)
        st = _fm_state(ser)
        check(fm_ok and st is not None and st[0] == TMP_PROJ,
              f"6c: navigated the browser to {TMP_PROJ} (at {st[0] if st else None})")

        if st is not None and st[0] == TMP_PROJ:
            target = RUN_FILE.rsplit("/", 1)[1]
            check(target in fixture, f"6c: {target} is listed in the browser")
            if target in fixture and _fm_goto(ser, fixture.index(target) + 1):
                prompt = _fm_vis(_fm_send(ser, b"x", 1.6))
                fm_captured += prompt
                check("Delete file" in prompt,
                      "6c: the browser asked to confirm the delete")
                done = _fm_vis(_fm_send(ser, b"y", 2.0))
                fm_captured += done
                check("Deleted:" in done,
                      "6c: the browser deleted the ACTIVE run file")

        # THE CRASHING KEYSTROKE.
        out_q = _fm_vis(_fm_send(ser, b"\x11", 6.0))
        out_q += _fm_vis(_fm_drain(ser, 6.0))
        fm_captured += out_q
        check("Exiting File Manager" in out_q,
              "6c: the Files browser exited with the active file gone")
    finally:
        # Leave the browser NO MATTER WHAT. A phase that dies with the file
        # manager still up strands Core 0 inside it: every later jl_exec times
        # out waiting for a raw REPL that cannot answer, so the suite's own
        # teardown fails too and the fixture/slot restores never run.
        try:
            if "Exiting File Manager" not in fm_captured:
                _fm_send(ser, b"\x11", 3.0)
        except Exception:
            pass
        try:
            ser.close()
        except Exception:
            pass

    node_alive = all(os.path.exists(port1_path()) for _ in range(3))
    check(node_alive, "6c: the CDC node never disappeared (no reboot)")
    check("[crashlog]" not in fm_captured,
          "6c: NO crashlog banner - the board did not HardFault on the way out")
    slot_f, path_f = active_context(3.0)
    check(slot_f == -1 and path_f == RUN_FILE,
          f"6c: the board still answers, still on {RUN_FILE} "
          f"(slot={slot_f}, path={path_f!r})")

    # Put the fixture back the way the design says it comes back - by dirtying
    # the context, not by re-writing RUN_YAML over it. The live state carries
    # phase 2's 41-42 edit, which phase 7's boot-persistence check reads back
    # out of this file; a fresh RUN_YAML would silently delete that witness.
    jl_exec("connect(59, 60)", timeout=25)
    time.sleep(4.0)
    out = jl_exec(f"print('restored=', 1 if fs_exists({RUN_FILE!r}) else 0)", timeout=25)
    check(parse_kv(out).get("restored") == 1,
          "6c: the browser's delete also heals on the next dirty auto-save")

    # --- 7. Boot persistence ----------------------------------------------
    # Back to the run file: phase 6b left the context on the read-only
    # template, which by design can never be the boot context.
    out = jl_exec(f"print('loaded=', 1 if load_project({RUN_FILE!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("loaded") == 1, "re-entered the run-file context for the reboot")
    time.sleep(2.0)

    la_exists, la = read_device_file(LAST_ACTIVE)
    check(la_exists and la is not None and la.strip() == RUN_FILE,
          f"last_active.txt holds the run file before the reboot (got {la!r})")

    cfg = read_config()
    check("[slots]" in cfg, "/config.txt has the new [slots] section")
    m_mode = re.search(r"boot_mode\s*=\s*(\d+)", cfg)
    m_slot = re.search(r"boot_slot\s*=\s*(\d+)", cfg)
    check(m_mode is not None, "[slots] boot_mode is written to /config.txt")
    check(m_slot is not None, "[slots] boot_slot is written to /config.txt")
    boot_mode_before = int(m_mode.group(1)) if m_mode else 1
    boot_slot_before = int(m_slot.group(1)) if m_slot else 0
    check(boot_mode_before == 1,
          f"boot_mode defaults to 1 = boot the last-active FILE (got {boot_mode_before})")

    # Reboot and re-assert. The board re-enumerates USB, so port1_wait_ready
    # polls for the node and then for a firmware that answers - bounded, and
    # not a blind sleep.
    ready = reboot_board()
    check(ready, "board came back after the reboot")

    if ready:
        slot_r, path_r = active_context(2.5)
        check(slot_r == -1 and path_r == RUN_FILE,
              f"BOOT PERSISTENCE: came up in the run-file context "
              f"(slot={slot_r}, path={path_r!r})")
        out = jl_exec("""
print("b1=", 1 if is_connected(11, 12) else 0)
print("e=", 1 if is_connected(41, 42) else 0)
""", timeout=30)
        vals = parse_kv(out)
        check(vals.get("b1") == 1 and vals.get("e") == 1,
              "BOOT PERSISTENCE: the run file's wiring is actually live "
              "(11-12 from the file, 41-42 from the edit)")

        # boot_mode 0 pins boot_slot instead.
        port1_command("`[slots] boot_slot = 5", 2.0)
        port1_command("`[slots] boot_mode = 0", 2.0)
        time.sleep(2.5)
        cfg2 = read_config()
        m2 = re.search(r"boot_mode\s*=\s*(\d+)", cfg2)
        check(m2 is not None and int(m2.group(1)) == 0,
              f"boot_mode = 0 persisted to /config.txt (got {m2.group(1) if m2 else None})")
        ready2 = reboot_board()
        check(ready2, "board came back after the boot_mode 0 reboot")
        if ready2:
            slot_p, path_p = active_context(2.5)
            check(slot_p == 5,
                  f"boot_mode 0 booted the pinned boot_slot 5 (got {slot_p})")
            check(path_p == "/slots/slot5.yaml",
                  f"...with its canonical path (got {path_p!r})")

    # --- 8. MSC round-trip -------------------------------------------------
    # The §1 USBfs change (reload the ACTIVE CONTEXT by path on eject, instead
    # of the old `if (netSlot >= 0)` guard that silently skipped file
    # contexts) needs a HOST to mount the drive, edit the file and eject it.
    # Nothing in this harness can drive a host mount, so it is verified by
    # hand. Bench recipe: load a run file, mount MSC, edit the run file on the
    # host, eject, and check the edit is live with 'f'.
    print("  SKIP: MSC round-trip needs a host mount - verify by hand "
          "(load a run file, mount, host-edit it, eject, confirm it is live)")

finally:
    # --- 9. Teardown -------------------------------------------------------
    # ORDER MATTERS: leave any FILE context BEFORE deleting the files it
    # points at. A file context auto-saves back to ITSELF, so a switch after
    # the delete would re-create what we just removed.
    try:
        port1_command("<2", 4.0)
        time.sleep(1.5)
    except Exception as e:
        print(f"  info: bounce to slot 2 failed: {e}")

    if boot_mode_before is not None:
        port1_command(f"`[slots] boot_mode = {boot_mode_before}", 2.0)
    if boot_slot_before is not None:
        port1_command(f"`[slots] boot_slot = {boot_slot_before}", 2.0)
    time.sleep(1.5)

    # Sweep the WHOLE fixture directory, not just the four files this suite
    # names: the allocator is monotonic, so a re-run (or a phase that failed
    # after minting <dir>_2.yaml) leaves a run file behind that the named list
    # never mentions - and rmdir refuses a directory that still holds one, so
    # the leftover silently becomes permanent. Same sweep test_projects and
    # test_parts_roundtrip already do.
    out = jl_exec(f"""
for p in ({RUN_FILE!r}, {TEMPLATE_FILE!r}, {BAD_FILE!r}, {TRAP_FILE!r}):
    if fs_exists(p):
        jfs.remove(p)
if fs_exists({TMP_PROJ!r}):
    for nm in jfs.listdir({TMP_PROJ!r}):
        try:
            jfs.remove({TMP_PROJ!r} + "/" + nm)
        except Exception as e:
            print("rmerr=", e)
try:
    jfs.rmdir({TMP_PROJ!r})
except Exception as e:
    print("rmdirerr=", e)
print("gone=", 0 if (fs_exists({TMP_PROJ!r}) or fs_exists({TRAP_FILE!r})) else 1)
""", timeout=30)
    check(parse_kv(out).get("gone") == 1,
          f"removed {TMP_PROJ} and {TRAP_FILE}")

    # Restore the numbered slot files this suite may have disturbed.
    #
    # "Did not exist" is a state that has to be restored TOO, and it was not:
    # phase 6a switches to slot 2 and auto-saves the fixture's own bridges into
    # /slots/slot2.yaml, so a bench that starts with a slot file ABSENT comes
    # out of the run with that file present, and no later run ever removes it -
    # the snapshot/restore pair then perpetuates the residue instead of undoing
    # it. Put the absence back. (General hygiene; every slot file happened to
    # exist on the bench this was written on, so it changes nothing there.)
    for path, existed, before in (("/slots/slot0.yaml", slot0_existed, slot0_before),
                                  ("/slots/slot2.yaml", slot2_existed, slot2_before)):
        if existed and before is not None:
            jl_exec(f"fs_write({path!r}, {before!r})", timeout=25)
        elif not existed:
            jl_exec(f"""
if fs_exists({path!r}):
    jfs.remove({path!r})
""", timeout=25)

    # Path-aware: the bench may have been on a file context when we started.
    restore_context(orig_slot, orig_path)
    time.sleep(1.5)

    if snapshot is not None:
        check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

finish("test_slot_files")
