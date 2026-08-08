#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Config round-trip: runtime toggle applies + persists, and the new
release keys (probe_droop_v0, crosspoint_resistance, net_currents,
net_voltage_scan) exist in /config.txt."""

import time

from jl import jl_exec, port1_command, check, finish


def read_config():
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


cfg = read_config()
for key in ("probe_droop_v0", "crosspoint_resistance", "net_currents",
            "net_voltage_scan"):
    check(key in cfg, f"/config.txt contains new key '{key}'")


def net_currents_value(cfg_text):
    for line in cfg_text.splitlines():
        if line.strip().startswith("net_currents"):
            return line.split("=")[1].strip().rstrip(";").strip()
    return None


before = net_currents_value(cfg)
check(before in ("0", "1", "true", "false"), f"net_currents has a sane value ({before})")

# Toggle via the 'i' command (updateConfigValue -> incremental save), wait
# for the idle flush, verify the file changed, then restore.
resp = port1_command("i")
check("net current scan" in resp, "'i' toggle acknowledged")

flipped = None
for _ in range(20):  # idle flush is lazy; poll up to ~20s
    time.sleep(1.0)
    now = net_currents_value(read_config())
    if now is not None and now != before:
        flipped = now
        break
check(flipped is not None, f"toggle persisted to /config.txt ({before} -> {flipped})")

resp = port1_command("i")  # restore original setting
check("net current scan" in resp, "restore toggle acknowledged")

finish("test_config")
