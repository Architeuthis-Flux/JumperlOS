#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Paste round-trips on the main terminal: Y -> S (YAML) and J -> L (JSON).

The state printers emit blank lines between sections; until 2026-08-17 the
paste readers stopped at the FIRST empty line, so a pasted Y round-trip ended
at 'sourceOfTruth:' and every later line ran as a menu command. This test
pastes the board's own Y and J output back (blank lines and all, as one burst
- what a terminal paste looks like) and checks the bridges come back.

Line-buffered mode only, deliberately: this file never sends B0, so it cannot
leave the board in char mode (where 'i?' returns the help page and the
net_currents/stress audits fail).
"""

import re
import time

import serial  # pyserial

from jl import jl_exec, port1_path, port1_command, check, finish

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def _collect(ser, secs):
    deadline = time.time() + secs
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    return _ANSI.sub("", buf.decode(errors="replace"))


def _paste_command(cmd, payload, settle=3.5):
    """Send `cmd`, wait for its prompt, paste `payload` in one write, collect."""
    with serial.Serial(port1_path(), 115200, timeout=0.05) as ser:
        ser.write(b"\r\n")
        ser.flush()
        _collect(ser, 1.5)  # connection-init banner
        ser.reset_input_buffer()
        ser.write(cmd.encode() + b"\r\n")
        ser.flush()
        prompt = _collect(ser, 1.0)
        ser.write(payload)
        ser.flush()
        out = _collect(ser, settle)
    return prompt, out


def _user_nets():
    out = jl_exec(
        "print([(n['number'], n['nodes']) for n in get_all_nets()"
        " if n['number'] >= 6 and n['nodes'] != 'EMPTY'])",
        timeout=15,
    ).strip()
    return out


# --- 0. make sure the terminal is line-buffered (the suite's assumption) ---
port1_command("B1", 1.5)

# --- 1. Y -> S round-trip ---------------------------------------------------
jl_exec("nodes_clear(); connect(1, 5); connect(10, 20); import time; time.sleep(0.3)")
y = port1_command("Y", 3.0)
check("version:" in y and "bridges:" in y, "Y prints a YAML state with a bridges section")
yaml = y[y.index("version:"):]
yaml = "\n".join(l.rstrip("\r") for l in yaml.split("\n")).rstrip() + "\n\n"
check(len([l for l in yaml.split("\n") if l.strip() == ""]) >= 3,
      "the pasted YAML has blank lines between sections (the shape that used to break)")

jl_exec("nodes_clear()")
prompt, out = _paste_command("S", yaml.encode())
check("Paste YAML" in prompt, "S prompts for the paste")
check("State applied successfully" in out, "S applies the pasted YAML")
check("Error" not in out, "no parser error on the Y round-trip")
check(out.count("Menu") <= 1, "no pasted line ran as a menu command (one menu at most)")
nets = _user_nets()
check("'1,5'" in nets and "'10,20'" in nets, f"both bridges are back after the S paste ({nets})")

# --- 2. CR-terminated paste (what a raw macOS terminal sends) --------------
jl_exec("nodes_clear()")
prompt, out = _paste_command("S", yaml.replace("\n", "\r").encode())
check("State applied successfully" in out, "S applies a CR-terminated paste")
nets = _user_nets()
check("'1,5'" in nets and "'10,20'" in nets, f"both bridges are back after the CR paste ({nets})")

# --- 3. J -> L round-trip ---------------------------------------------------
jl_exec("nodes_clear(); connect(2, 7); connect(30, 40); import time; time.sleep(0.3)")
j = port1_command("J", 3.0)
check("{" in j and "}" in j, "J prints a JSON state")
js = j[j.index("{"):]
js = js[: js.rindex("}") + 1].replace("\r", "")
jl_exec("nodes_clear()")
prompt, out = _paste_command("L", (js + "\n\n").encode())
check("Paste JSON" in prompt, "L prompts for the paste")
check("State applied successfully" in out, "L applies the pasted JSON")
nets = _user_nets()
check("'2,7'" in nets and "'30,40'" in nets, f"both bridges are back after the L paste ({nets})")

# Leave the board as found: applying a pasted power section relocates the
# probe feed off DAC0 (infra treats it as a user DAC write); a DAC0 write inside
# the feed window hands it back (test_infra_paths assumes it is there).
jl_exec("nodes_clear(); dac_set(0, 3.33, True)")
finish("test_paste_state")
