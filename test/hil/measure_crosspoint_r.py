#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure the real CH446Q crosspoint resistance, INA0 as current reference.

Plan 2 Stage A of CodeDocs/DEV_PLANS_82026.md. A TOOL, not a test: no
check() gates, prints a per-row table and summary stats. For each row it
builds the suite's known loop DAC0 -> ISENSE(shunt) -> row -> GND, reads
INA0 (truth to its shunt/cal, ~1-2%) and the net scan's raw per-path EMA,
then uses  R_true = R_assumed * I_scan / I_INA  per segment:
the scan computed I_scan = dV/(xp*R_assumed) from the very dV that I_INA
actually flowed through, so the ratio hands back the real per-crosspoint
resistance with no extra measurement.

Segments per loop (all carry the same INA current, in series):
  106->108  DAC0 -> ISENSE_PLUS   (same chips every row: control segment)
  109->row  ISENSE_MINUS -> row   (varies with row)
  row->100  row -> GND            (varies with row)

Bench state is snapshotted before and restored after (run_all's idiom).
Usage: python3 measure_crosspoint_r.py [row ...]   (default: 5, 34..79)
Note: the default range deliberately matches task #32's bisect list; in it,
61-69 are an unmapped hole in the node space (breadboard rows end at 60)
and are reported as open-loop SKIPs, while 70-79 are NANO_D0-D9.
"""

import re
import statistics
import sys
import time

from jl import (jl_exec, parse_kv, port1_command,
                board_state_capture, board_state_restore)

R_ASSUMED = 40.0   # calibration.crosspoint_resistance the flashed build uses
DAC_V = 0.8        # the suite's known-good drive: ~4 mA, >> deadband
SETTLE_S = 1.5     # scan EMA alpha=0.25 @20Hz + pair-tap rotation headroom
LOOP_NODES_FIXED = {100, 106, 108, 109}

# [nvscan] path %d net %d  %d->%d  %dxp dup%d  %+.2f mA (ema %+.2f)%s
PATH_RE = re.compile(
    r"\[nvscan\] path \d+ net \d+\s+(\d+)->(\d+)\s+(\d+)xp dup(\d+)\s+"
    r"[+-]?\d+\.\d+ mA \(ema ([+-]?\d+\.\d+)\)( pair)?")

rows = [int(a) for a in sys.argv[1:]] or [5] + list(range(34, 80))

snapshot = board_state_capture()
if snapshot is None:
    print("WARN: could not snapshot bench state; it will NOT be restored")

# Scan must be on ('i' toggles; put it back if we hit it off->on... it
# stays on either way, which is the bench default).
resp = port1_command("i")
if "off" in resp:
    port1_command("i")

results = []   # (row, n1, n2, xp, pair, ina_mA, ema_mA, r_per_xp)

try:
    for row in rows:
        out = jl_exec(f"""
import time
nodes_clear()
time.sleep(0.1)
dac_set(DAC0, {DAC_V})
connect(DAC0, ISENSE_PLUS)
connect(ISENSE_MINUS, {row})
connect(GND, {row})
time.sleep(0.2)
i_vals = []
for _ in range(6):
    i_vals.append(ina_get_current(0))
    time.sleep(0.05)
i_vals.sort()
print("ina_mA=", i_vals[len(i_vals) // 2] * 1000.0)
""")
        ina_mA = abs(parse_kv(out).get("ina_mA", 0.0))
        if not 2.0 < ina_mA < 80.0:
            print(f"row {row:3d}: SKIP (INA0 {ina_mA:.2f} mA implausible - "
                  "open loop or something else on the row?)")
            continue
        time.sleep(SETTLE_S)
        report = port1_command("i!", collect_seconds=2.5)
        loop_nodes = LOOP_NODES_FIXED | {row}
        got = 0
        for m in PATH_RE.finditer(report):
            n1, n2, xp, dup = (int(m.group(i)) for i in range(1, 5))
            ema, pair = abs(float(m.group(5))), bool(m.group(6))
            if dup != 0 or n1 not in loop_nodes or n2 not in loop_nodes:
                continue
            if xp == 0 or ema < 0.5:      # infra noise / dead line
                continue
            r = R_ASSUMED * ema / ina_mA  # per-crosspoint ohms
            results.append((row, n1, n2, xp, pair, ina_mA, ema, r))
            got += 1
            print(f"row {row:3d}: {n1:3d}->{n2:3d} {xp}xp"
                  f"{' pair' if pair else '     '}  INA {ina_mA:6.2f} mA"
                  f"  ema {ema:6.2f} mA  ->  R/xp {r:6.2f} ohm")
        if got == 0:
            print(f"row {row:3d}: no loop paths in 'i!' "
                  f"(INA {ina_mA:.2f} mA) - scan off or parse drift?")
finally:
    jl_exec("import time\nnodes_clear()\ndac_set(DAC0, 0.0)\ntime.sleep(0.05)\n")
    if snapshot is not None:
        ok = board_state_restore(snapshot)
        print(f"bench state restore: {'ok' if ok else 'FAILED - restore by hand'}")

if not results:
    sys.exit("no measurements collected")


def stats(label, sel):
    vals = [r[7] for r in sel]
    if not vals:
        return
    mean = statistics.fmean(vals)
    sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
    print(f"{label:28s} n={len(vals):3d}  mean {mean:6.2f}  sd {sd:5.2f}"
          f"  min {min(vals):6.2f}  max {max(vals):6.2f} ohm")


print(f"\n=== R per crosspoint, implied vs the {R_ASSUMED:.0f} ohm model ===")
stats("all segments", results)
stats("DAC0->ISENSE+ (control)", [r for r in results if r[1] == 106 or r[2] == 106])
stats("ISENSE- -> row", [r for r in results if 109 in (r[1], r[2])])
stats("row -> GND", [r for r in results if 100 in (r[1], r[2])])
stats("breadboard rows (<=60)", [r for r in results if r[0] <= 60])
stats("nano header (70-79)", [r for r in results if 70 <= r[0] <= 79])
stats("pair-tapped only", [r for r in results if r[4]])
stats("single-ended only", [r for r in results if not r[4]])
