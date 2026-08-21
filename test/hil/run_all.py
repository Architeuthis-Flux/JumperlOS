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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jl  # board_state_capture/restore - leave the bench as we found it

TESTS = [
    "test_micropython_fs.py",  # cheapest liveness check first
    "test_routing.py",
    "test_net_currents.py",
    "test_config.py",
    "test_stress.py",
    "test_paste_state.py",     # Y->S / J->L paste round-trips on port 1
    "test_slot_files.py",      # path-based active context: adopt, trap, boot
    "test_parts_roundtrip.py", # parts: section round-trip + the MP bindings
    "test_projects.py",        # /projects tree, provisioning, the launcher
    "test_guide_flow.py",      # guided placement: runtime + electrical checks
    "test_encoder_ui.py",      # last: drives the physical UI
]

# test_slot_files.py runs BEFORE the three project/guide suites and REBOOTS
# the board twice (boot-persistence assertions). It restores boot_mode/
# boot_slot and the original context in its finally, so anything after it
# starts from a normal bench - but it must not run between two suites that
# share state, hence its position here rather than later in the list.
#
# ORDER MATTERS for the three project/guide suites: test_projects.py's
# launcher phase asserts the picker lists exactly the two projects IT
# authored, and test_guide_flow.py pushes a third (/projects/hilguide) that
# it removes in its own finally. Running guide_flow BEFORE projects is fine;
# running it in the middle of projects is not - so they stay adjacent and in
# this order. All three hold port 1 for their drives, like test_paste_state.

here = os.path.dirname(os.path.abspath(__file__))
selector = sys.argv[1] if len(sys.argv) > 1 else ""

# Snapshot the bench BEFORE the suite touches anything: the tests' standard
# cleanup (nodes_clear + zeroed rails/DACs) used to simply stay on the board,
# and twice now that read as a firmware bug ("rails aren't setting",
# "current sensing isn't working" - both were this).
snapshot = jl.board_state_capture()
if snapshot is None:
    print("WARNING: could not snapshot the board state - it will NOT be restored after the suite")

results = {}
for name in TESTS:
    if selector and selector not in name:
        continue
    print(f"\n=== {name} " + "=" * max(0, 60 - len(name)))
    proc = subprocess.run([sys.executable, os.path.join(here, name)], cwd=here)
    results[name] = proc.returncode

if snapshot is not None:
    if jl.board_state_restore(snapshot):
        print("\nboard state restored to the pre-suite snapshot")
    else:
        print("\nWARNING: board state restore FAILED - the bench nets/rails are "
              "whatever the last test left; reload your slot or reboot")

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
