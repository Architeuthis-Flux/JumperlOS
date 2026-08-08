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

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

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
        while time.time() - overall < 4.0:
            if ser.read(4096):
                quiet_start = time.time()
            elif time.time() - quiet_start > 0.6:
                break
        ser.reset_input_buffer()
        ser.write(cmd.encode() + b"\r\n")
        ser.flush()
        deadline = time.time() + collect_seconds
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
        return _ANSI.sub("", buf.decode(errors="replace"))
