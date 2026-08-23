#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Encoder/menu UI smoke test via SWD input simulation (jumperless-swd-input skill).

Requires a Raspberry Pi Debug Probe with OpenOCD serving telnet on
localhost:4444 and the skill's jl_input.py with a fresh ADDR table for the
flashed build. Auto-skips when the probe isn't there.

Run this LAST: it drives the click menu on the physical board.
"""

import os
import re
import socket
import subprocess
import sys
import time

from jl import check, finish, skip

JL_INPUT = os.path.expanduser(
    os.environ.get("JL_INPUT",
                   "~/.cursor/skills/jumperless-swd-input/scripts/jl_input.py"))

# --- availability gates (skip, never troubleshoot) --------------------------
if not os.path.isfile(JL_INPUT):
    skip(f"jl_input.py not found at {JL_INPUT} (set JL_INPUT to override)")
try:
    s = socket.create_connection(("localhost", 4444), timeout=1.0)
    s.close()
except OSError:
    skip("OpenOCD telnet :4444 not reachable (no debug probe session)")


def jl_input(*args, timeout=20):
    proc = subprocess.run([sys.executable, JL_INPUT, *args],
                          capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr


def in_click_menu(out):
    m = re.search(r"inClickMenu\D*(\d+)", out)
    return int(m.group(1)) if m else None


def telnet_lines(lines, hold_between=0.0):
    """Send raw OpenOCD TCL lines over telnet :4444."""
    s = socket.create_connection(("localhost", 4444), timeout=3.0)
    try:
        time.sleep(0.3)
        s.recv(4096)  # banner
        for ln in lines:
            s.sendall(ln.encode() + b"\n")
            time.sleep(max(hold_between, 0.2))
            s.recv(4096)
    finally:
        s.close()


rc, out = jl_input("status")
if rc != 0:
    skip("jl_input.py status failed - regenerate its ADDR table for this build")
check(in_click_menu(out) is not None, "status reports inClickMenu")

# Open the menu with a simulated click, then scroll one step each way. The
# firmware auto-retries are in jl_input itself; menu state is the assertion.
rc, out = jl_input("click")
time.sleep(1.0)
rc, out = jl_input("status")
check(in_click_menu(out) == 1, "click opened the menu (inClickMenu == 1)")

# WHAT THESE TWO CHECKS PROVE, AND WHAT THEY DO NOT. Read the message strings
# literally: they assert the SWD INJECTION was accepted - jl_input's step path
# writes encoderDirectionState/encoderDirectionConsumed over OpenOCD and prints
# "stepped N" without ever reading firmware state back, so rc == 0 is a
# statement about OpenOCD, not about the menu. A menu loop that had stopped
# honouring UP/DOWN entirely would still show green here.
#
# They are deliberately NOT strengthened. The injection writes
# encoderDirectionState, which is the OUTPUT of the decode in
# src/RotaryEncoder.cpp - so the failure people most want covered (the C7
# rewrite decoding turns backwards) is BELOW this injection point and is
# undetectable from here with or without a stronger assertion. Asserting the
# selection index would test the menu's consumption of an injected direction,
# which is worth having but is not encoder coverage; making it look like
# encoder coverage would be worse than the honest label. The real thing needs
# wheel-level injection (an A/B quadrature drive) or a human turning the knob.
#
# The other three checks in this file DO assert firmware state (inClickMenu).
rc, _ = jl_input("step", "fwd", "1")
check(rc == 0, "step-forward injection accepted by the SWD tool (does NOT "
                "prove the menu selection moved - see the note above)")
time.sleep(0.5)
rc, _ = jl_input("step", "back", "1")
check(rc == 0, "step-back injection accepted by the SWD tool (does NOT prove "
                "the menu selection moved - see the note above)")
time.sleep(0.5)

# Close with the universal quit: physical-pin long hold (>=1500ms) on GPIO 11
# via SIO (skill-documented sequence: OUT_CLR + OE_SET, hold, OE_CLR).
telnet_lines(["mww 0xD0000020 0x800", "mww 0xD0000038 0x800"])
time.sleep(1.6)
telnet_lines(["mww 0xD0000040 0x800"])
time.sleep(1.0)

rc, out = jl_input("status")
check(in_click_menu(out) == 0, "long-hold quit closed the menu (inClickMenu == 0)")

finish("test_encoder_ui")
