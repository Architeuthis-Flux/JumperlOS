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
# reader must pass what it drains through fault_scan(). The ones that exist
# today do (test_projects.py's run_app probe and GuideDriver, test_guide_flow's
# GuideDriver and its unwedge drain, test_paste_state's _collect) - keep it that
# way when adding another.
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


def jl_exec(code, timeout=15):
    """Run a MicroPython snippet on the device, return its stdout."""
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
