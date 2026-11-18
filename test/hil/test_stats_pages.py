#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The scroll's status stops (eyecandy/StatsPages) and the rail adjust window.

Drives the wheel through port 1 - an arrow on an empty line is one detent
(Jerial -> terminalWheelStep) - and reads the STATS trace each stop pins on
the status row, plus the OLED frame over port 7. Read-only: nothing is routed,
saved or adjusted (the adjust window is judged by its hint, never clicked).

Needs port 1 free (quit the jumperless client) and an OLED on the board."""

import base64
import re
import time

import serial  # pyserial

from jl import check, finish, port1_path, skip
from port7 import port7_command

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]")
ser = serial.Serial(port1_path(), 115200, timeout=0.05)


def drain(quiet=0.25, limit=2.0):
    """Port 1's output until it has been silent for `quiet`, as clean lines."""
    buf, last, t0 = b"", time.time(), time.time()
    while time.time() - last < quiet and time.time() - t0 < limit:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            last = time.time()
    text = _ANSI.sub("\n", buf.decode(errors="replace")).replace("\r", "\n")
    return [ln.strip() for ln in text.split("\n") if ln.strip()]


def step(direction):
    """One detent; the lines it produced. An arrow the terminal dropped is
    sent again - the trace says which stop we are on, so a resend is safe."""
    for _ in range(3):
        ser.write(b"\x1b[C" if direction > 0 else b"\x1b[D")
        ser.flush()
        lines = drain()
        if lines:
            return lines
    return []


def stats_stop(lines):
    """(name, index, count) from a 'STATS page=power 1/4' trace, else None."""
    for ln in lines:
        m = re.match(r"STATS page=(\w+) (\d+)/(\d+)", ln)
        if m:
            return m.group(1), int(m.group(2)), int(m.group(3))
    return None


def frame():
    m = re.search(r"oled_b64\{\d+x\d+:([A-Za-z0-9+/=]+)\}", port7_command(":oled:b64\n", 1.2))
    return base64.b64decode(m.group(1)) if m else b""


# The first byte on a fresh connection triggers the firmware's connection-init
# (banner + input flush), which eats whatever follows it - so an arrow sent
# first would lose its tail. Prime with a bare newline, as port1_command does.
ser.write(b"\n")
ser.flush()
time.sleep(1.0)
drain()
# Going DOWN, the stops sit just before row 1: find the last one.
stop = None
for _ in range(120):
    stop = stats_stop(step(-1))
    if stop:
        break
if not stop:
    skip("no STATS stop in a full turn of the wheel (no OLED on this board?)")
name, idx, n = stop
check(idx == n, f"scrolling down enters the stops at the last one ({name} {idx}/{n})")

# Down through every page, each with its own picture, then off onto the rows.
seen = [frame()]
check(any(seen[0]), f"{name}: the panel is not blank")
for want in range(n - 1, 0, -1):
    stop = stats_stop(step(-1))
    check(stop is not None and stop[1] == want, f"down: stop {want}/{n} ({stop})")
    seen.append(frame())
    check(seen[-1] != seen[-2], f"stop {want}/{n} paints a different page")
lines = step(-1)
check(lines and stats_stop(lines) is None, f"down off the first stop lands on a row ({lines[-1:]})")

# And back up through them in order, off the last one onto the first row stop.
for want in range(1, n + 1):
    stop = stats_stop(step(+1))
    check(stop is not None and stop[1] == want, f"up: stop {want}/{n} ({stop})")
lines = step(+1)
check(lines and stats_stop(lines) is None, f"up off the last stop lands on a row ({lines[-1:]})")

# The adjust window: the Top Rail's card says "adjust?" when the highlight
# lands and stops saying it 5 s later. (rail_click_adjust off = no hint at all.)
rail = None
for _ in range(120):
    lines = step(+1)
    if any(ln.startswith("Top Rail") for ln in lines):
        rail = [ln for ln in lines if ln.startswith("Top Rail")][-1]
        break
check(rail is not None, "the wheel reaches the Top Rail")
if rail and "adjust?" in rail:
    late = drain(quiet=6.5, limit=6.5)
    rails = [ln for ln in late if ln.startswith("Top Rail")]
    check(rails and "adjust?" not in rails[-1], f"the hint lapses after the window ({rails[-1:]})")
else:
    print("  note: no adjust? hint on the rail (rail_click_adjust off) - window not checked")

ser.close()
finish("test_stats_pages")
