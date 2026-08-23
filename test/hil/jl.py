#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared transport + assertion helpers for the Jumperless HIL suite.

Two channels:
  - MicroPython raw REPL (CDC port 5) via the jumperless-v5 skill's
    jumperless.py (JUMPERLESS_PY env var overrides discovery).
  - Main terminal (CDC port 1) via pyserial, for single-char commands like
    'i!' / 'i?' whose output only exists there.

Per the project rule: no retry loops or connection troubleshooting here.
If the board isn't there, fail fast with one clear message.
"""

import glob
import os
import re
import subprocess
import sys
import tempfile
import time

# CSI sequences, plus the two-byte DECSC/DECRC (ESC 7 / ESC 8) that measure
# mode's status-area writer emits around every live readout - without the
# second alternative those bytes survive into captures and break parsing.
_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]")

_failures = []
_checks = 0


def check(cond, msg):
    global _checks
    _checks += 1
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        _failures.append(msg)


def skip(msg):
    print(f"SKIP: {msg}")
    sys.exit(0)


def finish(name):
    if _failures:
        print(f"{name}: FAIL ({len(_failures)}/{_checks} checks failed)")
        sys.exit(1)
    print(f"{name}: PASS ({_checks} checks)")
    sys.exit(0)


# ----------------------------------------------------------------------------
# Fault witness
# ----------------------------------------------------------------------------
# Every byte this module reads off port 1 passes through here. The firmware
# announces a fault two ways and both are unambiguous:
#
#   [crashlog] ...  printed once on the boot AFTER a HardFault. crashlogPending
#                   is set by isr_hardfault and by nothing else, so a clean
#                   reboot - machine.reset(), a UF2 load, reboot_board() - never
#                   produces one. Seeing it mid-suite means the board faulted.
#   [abort] ...     printed live, at the instant abort()/assert() is entered,
#                   before the deliberate bkpt.
#
# Before this, "no crashlog anywhere in the run" was a human grepping the log
# afterwards (task w3-1 report §7). Now it is enforced for everything that goes
# through this module - port1_command (including its connect banner) and
# port1_paste - which turns those suites into passive abort detectors for free.
#
# THE LIMIT, AND THE RULE THAT FOLLOWS FROM IT. This only sees bytes THIS MODULE
# read. Several suites open their own serial.Serial on port 1 because they have
# to drive an interactive app keystroke by keystroke, and those readers bypass
# this function entirely unless they call it themselves. So: ANY raw port-1
# reader must pass what it drains through fault_scan(). The ones that exist at
# the time of writing all do:
#
#   test_projects.py     the run_app probe, and its local GuideDriver
#   test_guide_flow.py   GuideDriver, and the teardown's unwedge drain
#   test_paste_state.py  _collect (every caller, banner included)
#   test_slot_files.py   the Switch Calib probe, and _fm_drain - the funnel for
#                        the whole Files-browser driver, which is the one that
#                        presses the delete-the-active-file key that w3-1 proved
#                        reaches abort()
#
# Treat that list as a snapshot, not a guarantee: it is only true until someone
# opens the next serial.Serial. `grep -n "serial.Serial(" test/hil/*.py` is the
# authoritative question, and every PORT-1 hit should have a fault_scan near it.
# (port7.py is the one legitimate exception in that grep: the banners are
# printed to port 1, so a port-7 reader has nothing to witness.)
#
# The connect banner is the part that must not be skipped, and it is also the
# part that cannot be shared: a post-fault [crashlog] is printed ONCE, to the
# FIRST terminal that attaches after the reboot. Whichever reader gets there
# first is the only one that can ever see it, so every reader scans its own
# banner and none of them can assume another will.
_FAULT_RE = re.compile(r"^.*\[(crashlog|abort)\].*$", re.M)

# Set once, and only by fault_scan. port1_wait_ready's blanket `except
# SystemExit` (which exists so a still-enumerating port does not kill the poll)
# would otherwise swallow the one exit that must never be swallowed.
_fault_message = ""


def fault_scan(text, where=""):
    """Fail fast if the board announced a fault. Returns text unchanged."""
    global _fault_message
    lines = [m.group(0).strip() for m in _FAULT_RE.finditer(text or "")]
    if not lines:
        return text
    _fault_message = (
        "FAIL: the board reported a FAULT" + (f" during {where}" if where else "")
        + " - the run is invalid from here:\n  "
        + "\n  ".join(lines[:12])
        + "\n(a [crashlog] banner means the PREVIOUS boot HardFaulted; an "
          "[abort] line means it just did. Symbolize the 'called from' address, "
          "not PC/LR.)"
    )
    sys.exit(_fault_message)


# ----------------------------------------------------------------------------
# REPL (port 5) via the skill's jumperless.py
# ----------------------------------------------------------------------------

def find_jumperless_py():
    env = os.environ.get("JUMPERLESS_PY")
    candidates = [env] if env else []
    home = os.path.expanduser("~")
    candidates += [
        os.path.join(home, ".cursor/skills/jumperless-v5/scripts/jumperless.py"),
        os.path.join(home, ".claude/skills/jumperless-v5/scripts/jumperless.py"),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    sys.exit(
        "FAIL: jumperless.py not found. Install the jumperless-v5 skill or "
        "set JUMPERLESS_PY to its scripts/jumperless.py path."
    )


# ----------------------------------------------------------------------------
# Device namespace hygiene  (THE INVARIANT: a snippet leaves no globals behind)
# ----------------------------------------------------------------------------
# jumperless.py runs `exec` with soft_reboot=False, so the device's __main__
# namespace SURVIVES every snippet, every suite, and the whole run - until a
# machine.reset(). Anything a snippet binds at top level stays live forever.
#
# This is not a style point. Measured across one full run_all: the harness
# accumulated ~55 distinct names, and the big ones were whole-file strings.
#
# WHAT THIS FIXES AND WHAT IT DOES NOT - both measured, because the difference
# decides where the real fix has to go:
#   * it DOES reclaim free bytes. In the bench experiment, deleting one
#     script's 82 globals returned 4.8 KB of free heap.
#   * it does NOT restore the largest CONTIGUOUS block. Same experiment:
#     the allocation cost 17.6 KB of maxblk and the delete gave back 1.9 KB
#     of it - 11%. MicroPython's GC never compacts, so freeing an object
#     leaves a hole exactly where it sat.
# So cleanup buys headroom and keeps growth bounded; only NOT ALLOCATING (see
# device_text) or a reset cures fragmentation. Both are worth having.
#
# Safety: an AST pass over every jl_exec snippet in test/hil confirmed that no
# snippet reads a global another snippet bound (55 names bound, zero
# cross-snippet reads), so deleting them between snippets cannot break one.
# Underscore-prefixed names are deliberately left alone - they are the escape
# hatch for anything that must deliberately persist (the heap probes use it).
#
# Set JL_KEEP_DEVICE_GLOBALS=1 to disable, e.g. when hand-debugging on the REPL
# and you want a snippet's variables to still be there afterwards.

_HYGIENE_PROLOGUE = (
    "import gc as _hilgc\n"
    "if '_hilkeep' not in globals():\n"
    "    _hilkeep = set(globals().keys())\n"
    "    _hilkeep.add('_hilkeep')\n"
)

_HYGIENE_EPILOGUE = (
    "\nfor _hilk in [_k for _k in globals().keys() "
    "if _k not in _hilkeep and not _k.startswith('_')]:\n"
    "    del globals()[_hilk]\n"
    "_hilgc.collect()\n"
)


def device_globals():
    """(count, summed len()) of globals the harness has left on the device.

    Observability for the invariant above: with the epilogue on this should sit
    at zero all run long, so a future suite that starts retaining something is
    VISIBLE rather than silent. run_all prints it between suites.
    """
    # Underscore names throughout: this snippet must not count ITSELF.
    out = jl_exec("""
_n = 0
_b = 0
for _k in globals().keys():
    if _k in _hilkeep or _k.startswith('_'):
        continue
    _n += 1
    try:
        _b += len(globals()[_k])
    except Exception:
        pass
print("retained_n=", _n)
print("retained_bytes=", _b)
""", timeout=20)
    v = parse_kv(out)
    return int(v.get("retained_n", -1)), int(v.get("retained_bytes", -1))


def device_heap():
    """(free, alloc, maxblk) for the device's MicroPython heap.

    maxblk - the largest CONTIGUOUS block, found by a binary-search ladder of
    bytearray() allocations - is the number that actually predicts a
    MemoryError. Free heap and contiguity are different diseases: across one
    full run_all, free fell ~12 KB while maxblk fell from 29 KB to 6 KB, and
    every reported MemoryError in this suite's history was a contiguity
    failure with plenty of free bytes left.
    """
    out = jl_exec("""
import gc
gc.collect()
_f = gc.mem_free()
_a = gc.mem_alloc()
_lo = 0
_hi = _f + 2048
while _lo + 64 < _hi:
    _md = (_lo + _hi) // 2
    try:
        _b = bytearray(_md)
        del _b
        _lo = _md
    except MemoryError:
        _hi = _md
gc.collect()
print("free=", _f)
print("alloc=", _a)
print("maxblk=", _lo)
""", timeout=30)
    v = parse_kv(out)
    return (int(v.get("free", -1)), int(v.get("alloc", -1)),
            int(v.get("maxblk", -1)))


def jl_exec(code, timeout=15):
    """Run a MicroPython snippet on the device, return its stdout.

    The snippet is wrapped so it leaves no globals behind - see the
    "Device namespace hygiene" note above.
    """
    if not os.environ.get("JL_KEEP_DEVICE_GLOBALS"):
        code = _HYGIENE_PROLOGUE + code + _HYGIENE_EPILOGUE
    jl = find_jumperless_py()
    with tempfile.NamedTemporaryFile(
        "w", suffix=".py", delete=False, prefix="hil_"
    ) as f:
        f.write(code)
        path = f.name
    try:
        proc = subprocess.run(
            [sys.executable, jl, "exec", "--file", path, "--timeout", str(timeout)],
            capture_output=True,
            text=True,
            timeout=timeout + 20,
            cwd=os.path.dirname(os.path.dirname(jl)),  # skill root (.jumperless_port cache)
        )
    finally:
        os.unlink(path)
    if proc.returncode != 0:
        sys.exit(
            f"FAIL: REPL exec failed (rc={proc.returncode}).\n"
            f"stdout: {proc.stdout.strip()}\nstderr: {proc.stderr.strip()}\n"
            "Check the board connection and try again."
        )
    return proc.stdout


def device_text(path, chunk=512):
    """Return a device file's contents as host text, WITHOUT ever building it
    as one contiguous MicroPython string.

    USE THIS instead of the `data = ""` / `data += chunk` idiom. That idiom is
    what produced this suite's recurring `MemoryError`, and the byte counts in
    the reports name it exactly: MicroPython asks the allocator for len+1 bytes
    for a str, so accumulating a 3114-byte /config.txt in 512-byte chunks walks
    it up through 513, 1025, 1537, 2049, 2561, 3073, 3115 - and W3-T2 reported
    "allocating 2049 bytes" while W3-T4 reported "allocating 3115 bytes". Two
    sightings, one loop, two different iterations of it.

    Worse than the transient: the loop leaves the finished string bound to a
    global (`data`) in the device's __main__, which persists for the WHOLE
    session because jumperless.py runs exec with soft_reboot=False. Measured on
    the bench: 3.1 KB retained that way cost 11 KB of largest contiguous block,
    and - because MicroPython's GC does not compact - deleting it afterwards
    does NOT give the contiguity back. Not allocating it is the only fix.

    Streaming to the host peaks at `chunk` bytes on the device instead.
    """
    return jl_exec(f"""
f = jfs.open({path!r}, "r")
while True:
    c = jfs.read(f, {int(chunk)})
    if not c:
        break
    print(c, end="")
jfs.close(f)
print()
""", timeout=30)


def parse_kv(out):
    """Parse 'key= value' prints from device output into a dict of floats/strs."""
    vals = {}
    for line in out.splitlines():
        m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$", line)
        if m:
            key, raw = m.group(1), m.group(2)
            try:
                vals[key] = float(raw)
            except ValueError:
                vals[key] = raw
    return vals


# ----------------------------------------------------------------------------
# Main terminal (port 1)
# ----------------------------------------------------------------------------

def port1_path():
    for pattern in ("/dev/cu.usbmodem*JLV5port1", "/dev/cu.*JLV5port1"):
        hits = [p for p in glob.glob(pattern) if p.endswith("port1")]
        if hits:
            return hits[0]
    sys.exit("FAIL: main terminal (JLV5port1) not found. Is the board connected?")


def port1_paste(cmd, payload, settle=3.5):
    """Send `cmd` on port 1, wait for its paste prompt, paste `payload` in one
    write, return (prompt, output). The S/L paste mechanics test_paste_state
    verifies - factored here so run_all's state restore can reuse them."""
    import serial  # pyserial

    _ansi = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

    def _collect(ser, secs):
        deadline = time.time() + secs
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
        return _ansi.sub("", buf.decode(errors="replace"))

    with serial.Serial(port1_path(), 115200, timeout=0.05) as ser:
        ser.write(b"\r\n")
        ser.flush()
        # The connection-init banner is where a post-fault [crashlog] appears -
        # it is printed once, to the first terminal that attaches after the
        # reboot - so it gets scanned even though nothing else reads it.
        fault_scan(_collect(ser, 1.5), f"the connect banner before '{cmd}'")
        ser.reset_input_buffer()
        ser.write(cmd.encode() + b"\r\n")
        ser.flush()
        prompt = _collect(ser, 1.0)
        ser.write(payload)
        ser.flush()
        out = _collect(ser, settle)
    fault_scan(prompt + out, f"the '{cmd}' paste")
    return prompt, out


def board_state_capture():
    """Snapshot the board's full state (bridges + power) as a pastable YAML,
    or None if the board didn't produce one. Forces line mode first (the
    suite's standing assumption)."""
    port1_command("B1", 1.5)
    y = port1_command("Y", 3.0)
    # An empty board prints "nets:" with no "bridges:" section - still a
    # valid, pastable snapshot (restoring "no user nets" is exactly right).
    if "version:" not in y or "power:" not in y:
        return None
    yaml = y[y.index("version:"):]
    return "\n".join(l.rstrip("\r") for l in yaml.split("\n")).rstrip() + "\n\n"


def board_state_restore(yaml):
    """Paste a board_state_capture() snapshot back. Returns True when the
    board confirmed it. The suite's cleanup (nodes_clear + zeroed rails) used
    to simply STAY on the board - twice now that read as a firmware bug on
    the bench ('rails aren't setting', 'current sensing isn't working')."""
    prompt, out = port1_paste("S", yaml.encode())
    ok = "State applied successfully" in out
    # Applying a pasted power section claims DAC0 as a user write, which
    # relocates the probe buffer feed; a DAC0 write inside the feed window
    # (2.80-3.90 V) hands it back. Re-issue the captured dac0 so the feed
    # returns whenever the bench value allows it.
    m = re.search(r"dac0:\s*(-?[0-9.]+)", yaml)
    if ok and m:
        v = float(m.group(1))
        if 2.80 <= v <= 3.90:
            jl_exec(f"dac_set(0, {v}, True)", timeout=15)
    return ok


def active_context(collect_seconds=1.5):
    """Query the active context with 'Q' and return (slot, path).

    Since "slots become files", 'Q' prints TWO lines:

        ACTIVE_SLOT:<n>      # 0-7 or 99 for a numbered slot, -1 for a file
        ACTIVE_PATH:<path>   # ALWAYS - numbered slots print their canonical path

    slot is an int (possibly -1) or None if the board didn't answer; path is a
    str or None. Note the '-?': the old suite-wide r"ACTIVE_SLOT:(\\d+)" does
    NOT match -1, so every suite aborted at its phase-0 snapshot whenever a
    file context happened to be active. That is exactly the trap this helper
    exists to close - use it instead of a local regex.
    """
    q = port1_command("Q", collect_seconds)
    ms = re.search(r"ACTIVE_SLOT:(-?\d+)", q)
    mp = re.search(r"ACTIVE_PATH:(\S+)", q)
    return (int(ms.group(1)) if ms else None,
            mp.group(1) if mp else None)


def restore_context(slot, path):
    """Return the board to a context captured with active_context().

    Numbered slots go back through '<n'. A file context has no single-char
    command, so it goes back through the MicroPython load_project() binding,
    which is loadSlotFromPath - the same adopting call the Files browser uses.
    """
    if slot is not None and slot >= 0:
        port1_command(f"<{slot}", 4.0)
        return True
    if path:
        jl_exec(f"load_project({path!r})", timeout=20)
        return True
    return False


# ----------------------------------------------------------------------------
# Run-file protection  (THE INVARIANT: never delete a run file you did not make)
# ----------------------------------------------------------------------------
# A project launch opens /projects/<dir>/<dir>_run.yaml as the persistent
# active context. It is USER DATA - the firmware never auto-deletes one, and
# it is where the user's circuit lives after they run a project.
#
# Under the wave-2 NUMBERED scheme a suite could mint <dir>_2.yaml and delete
# exactly that, leaving the user's <dir>_1.yaml alone. With ONE well-known
# filename that safety is gone: the suite's fixture run file IS the user's run
# file. So the rule, for every suite:
#
#   * fixture into a SCRATCH project directory (hiltest / hilguide / slotfiles
#     / tmptest) whenever the phase does not specifically need a shipped one;
#   * when a phase must touch a REAL project (555, i2cscrn, nand00, eeprom),
#     wrap it in run_file_capture()/run_file_restore() so the bytes - or the
#     ABSENCE - come back exactly;
#   * only ever purge with purge_numbered_runs(), which matches digits and
#     therefore cannot eat <dir>_run.yaml.
#
# The snapshot is a DEVICE-SIDE copy to "<path>.hilbak", not a host round-trip:
# byte-exact for any size, and it survives a host-side exception because the
# restore reads the board's own copy back.

PROJECT_RUN_SUFFIX = "_run"


def project_run_mode():
    """"single" or "history": which run-file scheme the FLASHED firmware uses.

    PROBED, never assumed - the suites run against whatever is on the board,
    and JL_PROJECT_RUN_HISTORY is a compile-time flag with no runtime read-out.
    `run=<N>` is the numbered scheme's grammar and a single-file build refuses
    it BY NAME, so one refused command is the whole discriminator. The project
    name is deliberately one that cannot exist: neither branch creates, opens
    or deletes anything."""
    out = port1_command("z hilmodeprobe run=1", 3.0)
    return "single" if "needs a JL_PROJECT_RUN_HISTORY build" in out else "history"


def project_run_path(pdir):
    """/projects/<dir> -> /projects/<dir>/<dir>_run.yaml (the single-file
    mode's one run file; JL_PROJECT_RUN_HISTORY builds use <dir>_<N>.yaml and
    this path simply never exists on them)."""
    name = pdir.rstrip("/").rsplit("/", 1)[-1]
    return f"{pdir.rstrip('/')}/{name}{PROJECT_RUN_SUFFIX}.yaml"


def run_file_capture(path):
    """Snapshot a run file (or its absence) to <path>.hilbak on the device.

    Returns a dict to hand back to run_file_restore(). Raises nothing on a
    missing file - "it wasn't there" is a state worth restoring too."""
    out = jl_exec(f"""
p = {path!r}
bak = p + ".hilbak"
if fs_exists(bak):
    jfs.remove(bak)
if fs_exists(p):
    s = jfs.open(p, "rb"); d = jfs.open(bak, "wb")
    n = 0
    while True:
        c = s.read(512)
        if not c:
            break
        d.write(c); n += len(c)
    s.close(); d.close()
    print("existed= 1")
    print("bytes=", n)
else:
    print("existed= 0")
""", timeout=45)
    vals = parse_kv(out)
    return {"path": path,
            "existed": vals.get("existed") == 1,
            "bytes": vals.get("bytes")}


def run_file_restore(snap):
    """Put a run_file_capture() snapshot back byte-exact, or restore its
    absence. The caller must have LEFT the run-file context first (a run file
    that is still active is re-created by the next switch's dirty pre-save).
    Returns True when the board agrees the file is back the way it was."""
    if not snap:
        return False
    path = snap["path"]
    want = 1 if snap["existed"] else 0
    out = jl_exec(f"""
p = {path!r}
bak = p + ".hilbak"
want = {want}
if want:
    if fs_exists(bak):
        s = jfs.open(bak, "rb"); d = jfs.open(p, "wb")
        while True:
            c = s.read(512)
            if not c:
                break
            d.write(c)
        s.close(); d.close()
        jfs.remove(bak)
else:
    if fs_exists(p):
        jfs.remove(p)
    if fs_exists(bak):
        jfs.remove(bak)
print("ok=", 1 if (1 if fs_exists(p) else 0) == want else 0)
print("bakgone=", 0 if fs_exists(bak) else 1)
""", timeout=45)
    vals = parse_kv(out)
    return vals.get("ok") == 1 and vals.get("bakgone") == 1


def purge_numbered_runs(pdir, dirname):
    """Delete /projects/<pdir>/<dirname>_<digits>.yaml ONLY.

    Deliberately NOT a startswith(prefix) sweep: `"555_".startswith` matches
    555_run.yaml, and a suite that deletes THAT deletes the user's circuit.
    Digits-only keeps the numbered-mode cleanup working without touching the
    single run file."""
    out = jl_exec(f"""
pre = {dirname!r} + "_"
n = 0
if fs_exists({pdir!r}):
    for nm in jfs.listdir({pdir!r}):
        if not (nm.startswith(pre) and nm.endswith(".yaml")):
            continue
        mid = nm[len(pre):-5]
        if len(mid) == 0 or not mid.isdigit():
            continue
        try:
            jfs.remove({pdir!r} + "/" + nm)
            n += 1
        except Exception as e:
            print("rmerr=", e)
print("purged=", n)
""", timeout=30)
    return parse_kv(out).get("purged")


def reboot_board():
    """Reset the board via machine.reset() and wait for it to come back.

    jl_exec() cannot be used directly: the REPL dies mid-exec (that IS the
    reset), so jumperless.py returns non-zero and jl_exec would sys.exit. The
    subprocess is therefore run here with its failure ignored, and readiness is
    proven by port1_wait_ready() rather than assumed.

    Returns True when the firmware answers again.
    """
    jl = find_jumperless_py()
    with tempfile.NamedTemporaryFile(
        "w", suffix=".py", delete=False, prefix="hil_reset_"
    ) as f:
        f.write("import machine\nmachine.reset()\n")
        path = f.name
    try:
        subprocess.run(
            [sys.executable, jl, "exec", "--file", path, "--timeout", "8"],
            capture_output=True, text=True, timeout=30,
            cwd=os.path.dirname(os.path.dirname(jl)),
        )
    except subprocess.TimeoutExpired:
        pass
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    time.sleep(2.0)   # let the USB node actually disappear before we poll
    return port1_wait_ready(35.0)


def port1_wait_ready(timeout=25.0):
    """Wait for port 1 to come back after a reboot, then confirm the firmware
    answers. The board re-enumerates USB on reset, so the device node itself
    disappears and returns - this polls for the node, then for a 'Q' that
    actually replies. Bounded; no silent sleeping."""
    import serial  # pyserial

    deadline = time.time() + timeout
    # 1) wait for the node to exist again
    while time.time() < deadline:
        hits = [p for p in glob.glob("/dev/cu.*JLV5port1") if p.endswith("port1")]
        if hits:
            break
        time.sleep(0.25)
    else:
        return False
    # 2) wait for the firmware behind it to answer
    while time.time() < deadline:
        try:
            slot, path = active_context(1.5)
            if slot is not None:
                return True
        except (serial.SerialException, OSError, SystemExit):
            # This blanket catch is what makes the poll tolerant of a port that
            # is still enumerating - but it would also swallow fault_scan's
            # exit, and this poll runs immediately after every reboot, which is
            # exactly when a post-fault [crashlog] is printed. Re-raise those.
            if _fault_message:
                raise SystemExit(_fault_message)
        time.sleep(0.5)
    return False


def port1_command(cmd, collect_seconds=2.5):
    """Send a single-char command line on port 1, return de-ANSI'd output."""
    import serial  # pyserial

    with serial.Serial(port1_path(), 115200, timeout=0.2) as ser:
        # The FIRST byte on a fresh connection triggers the firmware's
        # connection-init (greeting banner + input flush), which eats
        # whatever follows it - an 'i?' losing its '?' silently becomes a
        # config toggle. Prime the connection with a bare newline, wait for
        # the banner to finish (0.6s of silence, max 4s), then send the real
        # command on the now-initialized terminal.
        ser.write(b"\r\n")
        ser.flush()
        quiet_start = time.time()
        overall = time.time()
        banner = b""
        while time.time() - overall < 4.0:
            chunk = ser.read(4096)
            if chunk:
                banner += chunk
                quiet_start = time.time()
            elif time.time() - quiet_start > 0.6:
                break
        # The banner was previously read and thrown away, which is precisely
        # where a post-fault [crashlog] lands: the firmware prints it once, to
        # the first terminal that attaches after the reboot. Scan it before
        # discarding it, or the suite's only fault witness is a human grepping
        # the log afterwards.
        fault_scan(_ANSI.sub("", banner.decode(errors="replace")),
                   f"the connect banner before '{cmd}'")
        ser.reset_input_buffer()
        ser.write(cmd.encode() + b"\r\n")
        ser.flush()
        deadline = time.time() + collect_seconds
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
        return fault_scan(_ANSI.sub("", buf.decode(errors="replace")),
                          f"the '{cmd}' command")
