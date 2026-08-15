#!/usr/bin/env python3
"""Flash-save stress for the Jumperless V5, with SWD state sampling.

Interleaves the three flash-write paths (slot saves via node connect/
disconnect, jfs file writes, config toggles) while reading the cross-core
flags over the debug probe between operations. Logs per-op latency; on an
anomaly (slow/failed op, or a flag stuck across consecutive samples) it
snapshots both cores' PCs.

Run from test/hil so jl.py imports.
"""
import os
import sys
import time

# Resolve the sibling modules relative to this file, not to one machine's
# checkout or a long-gone scratchpad directory.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))   # test/hil  -> jl.py
sys.path.insert(0, _HERE)                    # test/hil/swd -> sample_state.py

from jl import jl_exec, port1_command  # noqa: E402
from sample_state import OOCD, sample_vars, sample_pcs  # noqa: E402

ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 12
SLOW_S = 8.0  # flag any single op slower than this

o = OOCD()
prev = None
stuck_candidates = ("core1busy", "core2busy", "pauseCore2", "refreshInProgress",
                    "refreshLocalInProgress", "sendAllPathsCore2")


def snap(tag):
    vals = sample_vars(o)
    line = " ".join(f"{k}={v}" for k, v in vals.items()
                    if k in stuck_candidates or vals[k] not in (0, None))
    print(f"    [{tag}] {line}", flush=True)
    return vals


def anomaly(tag, extra=""):
    print(f"!!! ANOMALY at {tag} {extra}", flush=True)
    pcs = sample_pcs(o)
    for core, (pc, lr) in pcs.items():
        print(f"    {core}: pc={pc} lr={lr}", flush=True)


def timed(tag, fn):
    t0 = time.time()
    try:
        out = fn()
        dt = time.time() - t0
        print(f"  {tag}: {dt:.2f}s", flush=True)
        if dt > SLOW_S:
            anomaly(tag, f"slow ({dt:.1f}s)")
        return out
    except SystemExit as e:
        dt = time.time() - t0
        print(f"  {tag}: FAILED after {dt:.2f}s: {e}", flush=True)
        anomaly(tag, "op failed")
        raise


for i in range(ITER):
    print(f"--- iter {i} ---", flush=True)

    # 1. Routing churn: connect + disconnect -> refreshConnections + slot save
    timed("connect", lambda: jl_exec(
        "connect(TOP_RAIL, %d)\nconnect(GND, %d)\nprint('ok')" %
        (14 + (i % 10), 45 + (i % 10)), timeout=25))

    # 2. jfs write + remove (write-back cache -> SPIFTL flash op on flush)
    timed("jfs_write", lambda: jl_exec(
        'f = jfs.open("/hil_stress.txt", "w")\n'
        'jfs.write(f, "stress %d " * 40)\n'
        'jfs.close(f)\n'
        'print("wrote=", jfs.exists("/hil_stress.txt"))' % i, timeout=25))

    vals = snap("post-write")

    # 3. Config toggle every 3rd iter (updateConfigValue -> incremental save)
    if i % 3 == 2:
        timed("cfg_toggle_i", lambda: port1_command("i", collect_seconds=2))
        timed("cfg_toggle_back", lambda: port1_command("i", collect_seconds=2))

    # 4. Disconnect churn
    timed("disconnect", lambda: jl_exec(
        "disconnect(TOP_RAIL, -1)\ndisconnect(GND, -1)\nprint('ok')",
        timeout=25))

    vals = snap("post-iter")
    if prev is not None:
        for k in stuck_candidates:
            if vals.get(k) and prev.get(k):
                anomaly(f"iter{i}", f"{k} set across two consecutive samples")
    prev = vals

# cleanup
jl_exec('jfs.remove("/hil_stress.txt") if jfs.exists("/hil_stress.txt") else 0\nprint("cleaned")', timeout=20)
print("STRESS DONE", flush=True)
