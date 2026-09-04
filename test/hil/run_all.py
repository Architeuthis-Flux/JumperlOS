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
    "test_routing_dense.py",   # dense/edge routings vs the schematic's copper
    "test_net_currents.py",
    "test_config.py",
    "test_stress.py",
    "test_paste_state.py",     # Y->S / J->L paste round-trips on port 1
    "test_slot_files.py",      # path-based active context: adopt, trap, boot
    "test_parts_roundtrip.py", # parts: section round-trip + the MP bindings
    # test_ambient_parts.py (the retired guide_flow's salvage) stays OUT
    # until its two racy skeleton checks are fixed: the PARTWARN assertion
    # depends on an async line surviving port1_command's own input reset,
    # and the bare `z steps` check hardcodes cursor 1/ while the cursor
    # deliberately persists. 7/9 green on the bench 2026-08-27.
    "test_projects.py",        # /projects tree, provisioning, the launcher
    "test_encoder_ui.py",      # last: drives the physical UI
]

# test_slot_files.py runs BEFORE the three project/guide suites and REBOOTS
# the board twice (boot-persistence assertions). It restores boot_mode/
# boot_slot and the original context in its finally, so anything after it
# starts from a normal bench - but it must not run between two suites that
# share state, hence its position here rather than later in the list.
#
# ORDER MATTERS for the project/guide suites: test_projects.py's launcher
# phase asserts the picker lists exactly the projects IT authored, and
# test_ambient_parts.py pushes /projects/ambtest (removed in its own
# finally) - so ambient runs BEFORE projects, never in the middle of it.
# (test_guide_flow.py retired with the blocking guide runner, b1ce6fb; its
# salvage is test_ambient_parts.py.) Both hold port 1 for their drives,
# like test_paste_state.

# ---------------------------------------------------------------------------
# Deliberate resets  (W3-T7)
# ---------------------------------------------------------------------------
# The board is NOT reset between suites as a matter of course: a reset costs
# ~12 s and, worse, it would hide exactly the kind of accumulation this harness
# should notice. It is applied only where the damage was MEASURED and is
# genuinely irreversible.
#
# MicroPython's GC does not compact. Freeing an object leaves a hole where it
# sat, so the largest CONTIGUOUS block - the number that decides whether the
# next compile raises MemoryError - only ever recovers on a reset. Measured on
# the bench (W3-T7): running the shipped 12.3 KB i2cscrn script through the
# launcher took the heap from ~29 KB largest block to ~6 KB, and deleting every
# global that script left behind gave back 11% of it. Nothing short of a reset
# fixes that. (test_guide_flow, the suite W3-T2 saw the MemoryError coming
# out of, has since retired - but anything running after projects would
# otherwise inherit the 6 KB largest block, and MP work after this suite is
# a legitimate future shape.)
#
# The value is the reason it exists; keep them together.
RESET_AFTER = {
    "test_projects.py":
        "it runs the 12.3 KB i2cscrn script through the launcher, which leaves "
        "the largest contiguous block at ~6 KB (from ~29 KB). MicroPython "
        "cannot compact, so only a reset restores it for whatever runs next.",
}

here = os.path.dirname(os.path.abspath(__file__))
selector = sys.argv[1] if len(sys.argv) > 1 else ""

# Snapshot the bench BEFORE the suite touches anything: the tests' standard
# cleanup (nodes_clear + zeroed rails/DACs) used to simply stay on the board,
# and twice now that read as a firmware bug ("rails aren't setting",
# "current sensing isn't working" - both were this).
snapshot = jl.board_state_capture()
if snapshot is None:
    print("WARNING: could not snapshot the board state - it will NOT be restored after the suite")

# ...and the ACTIVE CONTEXT, which the runner alone used to skip. Every suite
# pairs board_state_capture with active_context at phase 0 and restore_context
# in its finally (test_projects:360/1971, test_parts_roundtrip:187/901,
# test_slot_files:292/1152, test_guide_flow:696/2601); the runner captured the
# electrical state and nothing else. A suite that dies before its own
# restore_context - jl_exec sys.exits on any REPL failure, and three of the four
# run exit-prone cleanups BEFORE their restore_context - leaves the board on ITS
# context, and the runner's final 'S' paste then lands there instead of on the
# user's slot. The bench looks right until the next reboot.
orig_slot, orig_path = jl.active_context(1.5)
print(f"pre-suite context: slot {orig_slot}, path {orig_path!r}")

results = {}
restore_error = None
try:
    for name in TESTS:
        if selector and selector not in name:
            continue
        print(f"\n=== {name} " + "=" * max(0, 60 - len(name)), flush=True)
        proc = subprocess.run([sys.executable, os.path.join(here, name)],
                              cwd=here)
        results[name] = proc.returncode

        # Heap read-out between suites. Cheap (~1 s) and permanent: "free" and
        # "maxblk" are different diseases, and "retained" is the harness's own
        # namespace hygiene (jl_exec's epilogue should keep it at 0 all run long,
        # so a suite that starts leaking is visible here instead of silent).
        # Every line names its suite and flushes: this file is usually read as a
        # redirected log, where unflushed prints arrive in 8 KB blocks long after
        # the subprocess output they belong to, and an unlabelled "heap:" line in
        # the middle of that is unattributable.
        #
        # jl.guarded because this is a READ-OUT, not an assertion, and jl_exec
        # sys.exits on any REPL failure: an exit here used to end the run BEFORE
        # the restore below, which is the runner's one duty. A failed read is a
        # failed read - it says so and the suites carry on.
        heap, err = jl.guarded(jl.device_heap)
        globs, gerr = (jl.guarded(jl.device_globals) if heap is not None
                       else (None, err))
        if heap is None or globs is None:
            print(f"  heap after {name}: read failed ({(err or gerr)!r})",
                  flush=True)
        else:
            free, alloc, maxblk = heap
            held_n, held_b = globs
            warn = "   <-- LOW CONTIGUOUS BLOCK" if 0 <= maxblk < 8192 else ""
            held = f", retained {held_n} globals/{held_b}B" if held_n else ""
            print(f"  heap after {name}: free {free}, largest block "
                  f"{maxblk}{held}{warn}", flush=True)

        if name in RESET_AFTER:
            print(f"  resetting the board after {name}: {RESET_AFTER[name]}",
                  flush=True)
            if jl.reboot_board():
                heap, err = jl.guarded(jl.device_heap)
                if heap is None:
                    print(f"  heap after the reset: read failed ({err!r})",
                          flush=True)
                else:
                    free, alloc, maxblk = heap
                    print(f"  heap after the reset: free {free}, largest block "
                          f"{maxblk}", flush=True)
            else:
                print("  WARNING: the board did not come back from the reset",
                      flush=True)

# CONTEXT BEFORE STATE. The restore is an 'S' paste, which lands in whatever
# context is active - so the context has to be the user's again first, or the
# paste writes the user's circuit into a suite fixture that the next teardown
# deletes.
#
# IN A FINALLY, AND EVERY CALL IN IT GUARDED, because putting the bench back is
# the runner's one duty and almost everything above can end the process without
# raising: jl_exec sys.exits on any REPL failure and fault_scan on any
# [crashlog]/[abort]. Twice the runner died mid-loop and left the bench on the
# last test's nets, in a suite fixture context. A swallowed exit is still fatal
# - see restore_error at the bottom - it just stops being fatal FIRST.
finally:
    if orig_slot is not None or orig_path:
        _, err = jl.guarded(jl.restore_context, orig_slot, orig_path)
        restore_error = restore_error or err
        now, err = jl.guarded(jl.active_context, 1.5)
        restore_error = restore_error or err
        now_slot, now_path = now if now is not None else (None, None)
        if (now_slot, now_path) != (orig_slot, orig_path):
            print(f"\nWARNING: could not put the board back on its pre-suite "
                  f"context: wanted slot {orig_slot} / {orig_path!r}, got slot "
                  f"{now_slot} / {now_path!r}. The state restore below will land in "
                  f"the WRONG context - fix the context by hand before trusting the "
                  f"bench.")
        else:
            print(f"\nactive context restored: slot {now_slot}, {now_path!r}")

    if snapshot is not None:
        restored, err = jl.guarded(jl.board_state_restore, snapshot)
        restore_error = restore_error or err
        if restored:
            print("board state restored to the pre-suite snapshot")
        else:
            print("\nWARNING: board state restore FAILED - the bench nets/rails are "
                  "whatever the last test left; reload your slot or reboot")

print("\n" + "=" * 66)
fails = 0
skipped = 0
for name, rc in results.items():
    # A SKIPPED suite is not a passing one. jl.skip() exits with SKIP_EXIT so
    # this line can tell "asserted nothing, by design" from "asserted
    # everything and they held" - it used to print PASS for both, which is how
    # a probe-less bench read as full green on test_encoder_ui.
    if rc == jl.SKIP_EXIT:
        status = "SKIP"
        skipped += 1
    elif rc != 0:
        status = "FAIL"
        fails += 1
    else:
        status = "PASS"
    print(f"  {status}  {name}")
passed = len(results) - fails - skipped
print(f"HIL suite: {'PASS' if fails == 0 else 'FAIL'} "
      f"({passed}/{len(results)} files passed"
      + (f", {skipped} SKIPPED - they asserted nothing" if skipped else "")
      + ")")
if restore_error is not None:
    # Caught only so the bench came back first; it used to end the run where it
    # happened, and it still ends it - after the summary, so the log is intact.
    sys.exit(f"FAIL: the post-suite bench restore did not complete: "
             f"{restore_error!r}\nThe bench may be on a test's nets or context.")
sys.exit(1 if fails else 0)
