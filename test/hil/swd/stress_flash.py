#!/usr/bin/env python3
"""Flash-save stress for the Jumperless V5, with SWD state sampling.

Interleaves the three flash-write paths (slot saves via node connect/
disconnect, jfs file writes, config toggles) while reading the cross-core
flags over the debug probe between operations. Logs per-op latency; on an
anomaly (slow/failed op, or a flag stuck across consecutive samples) it
snapshots both cores' PCs.

Without a debug probe (no OpenOCD on :4444, or --no-swd) it still runs the
same op sequence and falls back to USB liveness as the health check: the
failure this soak exists to catch ends with the board off the bus, and the
op that was in flight fails. You lose the flag samples and the PC autopsy -
and the recovery path (flash_swd.sh) - so only run probe-less on a board you
can power-cycle.

The 'X' resource status is captured before and after so the shared-IRQ census
and the flash-write park's timeout counter bracket the run.

Usage: stress_flash.py [iters] [--no-swd]
"""
import glob
import os
import re
import sys
import time

# Resolve the sibling modules relative to this file, not to one machine's
# checkout or a long-gone scratchpad directory.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))   # test/hil  -> jl.py
sys.path.insert(0, _HERE)                    # test/hil/swd -> sample_state.py

from jl import jl_exec, port1_command  # noqa: E402
from sample_state import OOCD, sample_vars, sample_pcs  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
ITER = int(args[0]) if args else 12
SLOW_S = 8.0  # flag any single op slower than this
USB_PORTS = 4  # CDC ports the V5 enumerates; fewer = it fell off the bus

o = None
if "--no-swd" not in sys.argv:
    try:
        o = OOCD()
    except OSError as e:
        print(f"(no OpenOCD on :4444 - {e}; running without SWD sampling)", flush=True)
else:
    print("(--no-swd: running without SWD sampling)", flush=True)

prev = None
stuck_candidates = ("core1busy", "core2busy", "core1FrameHoldDepth",
                    "refreshInProgress", "refreshLocalInProgress")


def usb_ports():
    return len([p for p in glob.glob("/dev/cu.usbmodem*JLV5port*")])


def snap(tag):
    if o is None:
        n = usb_ports()
        print(f"    [{tag}] usb_ports={n}", flush=True)
        if n < USB_PORTS:
            anomaly(tag, f"board off the bus ({n}/{USB_PORTS} ports)")
        return {}
    vals = sample_vars(o)
    line = " ".join(f"{k}={v}" for k, v in vals.items()
                    if k in stuck_candidates or vals[k] not in (0, None))
    print(f"    [{tag}] {line}", flush=True)
    return vals


def anomaly(tag, extra=""):
    print(f"!!! ANOMALY at {tag} {extra}", flush=True)
    if o is None:
        print("    (no SWD: cannot snapshot PCs)", flush=True)
        return
    pcs = sample_pcs(o)
    for core, (pc, lr) in pcs.items():
        print(f"    {core}: pc={pc} lr={lr}", flush=True)


def status_x(tag):
    """The shared-IRQ census + flash-write park line from 'X'."""
    out = port1_command("X", collect_seconds=3.0)
    m = re.search(r"shared-IRQ handler slots:.*?flash-write park[^\n]*", out, re.S)
    print(f"=== X {tag} ===\n{m.group(0) if m else '(no census in X output)'}", flush=True)


status_x("before")


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
status_x("after")
print("STRESS DONE", flush=True)
