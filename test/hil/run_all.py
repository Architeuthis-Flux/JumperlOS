#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the whole HIL regression suite in order. Exit 0 only if every
non-skipped test passes.

Usage:
    python3 test/hil/run_all.py            # everything
    python3 test/hil/run_all.py routing    # only tests matching a substring
"""

import os
import subprocess
import sys

TESTS = [
    "test_micropython_fs.py",  # cheapest liveness check first
    "test_routing.py",
    "test_net_currents.py",
    "test_config.py",
    "test_stress.py",
    "test_encoder_ui.py",      # last: drives the physical UI
]

here = os.path.dirname(os.path.abspath(__file__))
selector = sys.argv[1] if len(sys.argv) > 1 else ""

results = {}
for name in TESTS:
    if selector and selector not in name:
        continue
    print(f"\n=== {name} " + "=" * max(0, 60 - len(name)))
    proc = subprocess.run([sys.executable, os.path.join(here, name)], cwd=here)
    results[name] = proc.returncode

print("\n" + "=" * 66)
fails = 0
for name, rc in results.items():
    status = "PASS" if rc == 0 else "FAIL"
    if rc != 0:
        fails += 1
    print(f"  {status}  {name}")
print(f"HIL suite: {'PASS' if fails == 0 else 'FAIL'} "
      f"({len(results) - fails}/{len(results)} files passed)")
sys.exit(1 if fails else 0)
