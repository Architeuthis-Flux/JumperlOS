#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Concurrency catch-all: hammer connect/disconnect refreshes while the net
voltage scanner runs, then assert the board is healthy. This is the test
that trips when 'changing one thing broke another' across the scanner,
router, LED, and encoder subsystems sharing core 2."""

import re

from jl import jl_exec, port1_command, check, finish

# Ensure the scanner is running during the stress (default on).
resp = port1_command("i")
if "off" in resp:
    port1_command("i")

out = jl_exec("""
import time
nodes_clear()
time.sleep(0.1)
dac_set(DAC0, 2.0)
rows_a = [4, 12, 22, 37, 49]
rows_b = [7, 17, 27, 42, 55]
for it in range(40):
    rows = rows_a if (it & 1) == 0 else rows_b
    connect(DAC0, rows[0])
    for k in range(len(rows) - 1):
        connect(rows[k], rows[k + 1])
    connect(ADC0, rows[-1])
    time.sleep(0.03)
    if it % 10 == 9:
        nodes_clear()
        time.sleep(0.05)
    else:
        disconnect(DAC0, -1)
        disconnect(ADC0, -1)
        time.sleep(0.02)
nodes_clear()
time.sleep(0.1)
print("stress_done= 1")
""", timeout=90)
check("stress_done= 1" in out, "40 connect/disconnect/refresh cycles completed")

# Board must still answer instantly on the REPL...
out = jl_exec("print('alive=', 1 + 1)\n", timeout=10)
check("alive= 2" in out, "REPL responsive after stress")

# ...and the crossbar bookkeeping must be clean: no suspect chips (PIO
# timeouts), no live short in the audit.
audit = port1_command("i?", collect_seconds=3.0)
m = re.search(r"suspect=0x([0-9a-fA-F]+)", audit)
check(m is not None and int(m.group(1), 16) == 0,
      "no PIO timeouts marked any chip suspect during stress")
check("SHORT" not in audit.replace("no live short", ""),
      "audit reports no live short after stress")
check("self-check: PASS" in audit, "RouteSafety self-check still PASS")

finish("test_stress")
