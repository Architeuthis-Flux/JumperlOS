#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Net voltage scan regressions: scan estimate vs INA0 truth on a real
current loop, zero-load deadband, and RouteSafety audit health ('i?')."""

import re
import time

from jl import jl_exec, parse_kv, port1_command, check, finish

# Make sure the scan is on (display.net_currents). 'i' toggles; read state
# from the response and toggle back if we turned it off by accident.
resp = port1_command("i")
if "off" in resp:
    resp = port1_command("i")
check("on" in resp, "net current scan enabled via 'i'")

# --- 1. Real current loop: DAC0 -> ISENSE(2R shunt) -> row 5 -> GND --------
out = jl_exec("""
import time
nodes_clear()
time.sleep(0.1)
dac_set(DAC0, 0.8)
connect(DAC0, ISENSE_PLUS)
connect(ISENSE_MINUS, 5)
connect(GND, 5)
time.sleep(0.2)
i_vals = []
for _ in range(6):
    i_vals.append(ina_get_current(0))
    time.sleep(0.05)
i_vals.sort()
print("ina_mA=", i_vals[len(i_vals) // 2] * 1000.0)
""")
vals = parse_kv(out)
ina_mA = abs(vals.get("ina_mA", 0.0))
# Floor measured 2026-09-03: this loop (DAC0 0.8 V -> ISENSE+ -> shunt ->
# ISENSE- -> row -> GND) reads 2.1 mA with every bridge unstacked and 4.5 mA
# with two copies each; connect() now honours the per-class defaults
# (stack_dacs 0 since 2026-09-02), so the old 3.0 mA floor assumed copper
# the router no longer stacks. 1.5 mA still refuses an open loop (0 mA).
check(1.5 < ina_mA < 80.0, f"INA0 sees a plausible loop current ({ina_mA:.2f} mA)")

# Give the scanner a couple of cycles to tap the loop's nodes, then compare
# its per-path estimate against the INA reading.
time.sleep(3.0)
report = port1_command("i!", collect_seconds=3.0)
path_lines = [ln for ln in report.splitlines() if "[nvscan] path" in ln and "mA" in ln]
check(len(path_lines) > 0, "'i!' report contains per-path currents")

best = None
for ln in path_lines:
    m = re.search(r"([+-]?\d+\.\d+)\s*mA", ln)
    if m:
        mag = abs(float(m.group(1)))
        if best is None or mag > best:
            best = mag
check(best is not None and abs(best - ina_mA) < max(0.5 * ina_mA, 2.0),
      f"scan max path current {best} mA within 50% of INA0 {ina_mA:.2f} mA")

# --- 2. Zero-load net must read ~0 (deadband / differential rail scan) -----
jl_exec("""
import time
nodes_clear()
time.sleep(0.1)
dac_set(TOP_RAIL, 5.0)
connect(TOP_RAIL, 20)
time.sleep(0.1)
""")
time.sleep(3.0)
report = port1_command("i!", collect_seconds=3.0)
zero_ok = True
for ln in report.splitlines():
    if "[nvscan] path" in ln and "mA" in ln:
        # Only the TOP_RAIL net's paths (node 101). The board's own infra
        # shows up in the report too - the probe buffer feed (GPIO8 ->
        # ROUTABLE_BUFFER_IN, nodes 138/139) legitimately carries ~1.4 mA
        # at all times, and the scan reporting it is correct, not phantom.
        pm = re.search(r"(\d+)(?:->|-)(\d+)\s+\d+xp", ln)
        if not pm or "101" not in (pm.group(1), pm.group(2)):
            continue
        m = re.search(r"([+-]?\d+\.\d+)\s*mA", ln)
        if m and abs(float(m.group(1))) > 1.0:
            zero_ok = False
            print(f"  offending line: {ln.strip()}")
check(zero_ok, "zero-load TOP_RAIL net shows < 1 mA phantom current")

# --- 3. RouteSafety self-check + audit ('i?') -------------------------------
audit = port1_command("i?", collect_seconds=3.0)
check("self-check: PASS" in audit, "RouteSafety self-check PASS (includes GPIO-net cases)")
m = re.search(r"suspect=0x([0-9a-fA-F]+)", audit)
check(m is not None and int(m.group(1), 16) == 0, "no chips marked suspect (no PIO timeouts)")
check(" ok" in audit or "SHORT" not in audit, "lastChipXY audit reports no live short")

jl_exec("import time\nnodes_clear()\ndac_set(TOP_RAIL, 0.0)\ntime.sleep(0.05)\n")
finish("test_net_currents")
