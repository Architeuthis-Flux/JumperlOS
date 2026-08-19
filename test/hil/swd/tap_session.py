#!/usr/bin/env python3
"""Interactive tap-session monitor. Run while Kevin taps pads in select mode.

Samples over SWD at ~20Hz and logs, with timestamps:
  - every change in lastReadRaw (the raw pad-ladder ADC value the probe
    row-mapper consumed) - shows exactly what the firmware saw per tap
  - every configChanged / filesystemActive / core-1 frame hold transition -
    shows save windows overlapping the taps

Symbol addresses are resolved from the flashed build's ELF at start (same
rules as sample_state.py: JL_ELF overrides, else release then debug ELF).

Usage: python3 tap_session.py [seconds]   (default 60)
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sample_state import OOCD, find_elf, find_nm  # noqa: E402

SYMBOLS = ("configChanged", "filesystemActive", "core1FrameHoldDepth",
           "lastReadRaw", "switchPosition", "probeActive")


def resolve(names):
    elf = find_elf()
    if elf is None:
        sys.exit("FAIL: no firmware.elf found - set JL_ELF or build jumperless_v5")
    out = subprocess.run([find_nm(), elf], capture_output=True, text=True,
                         check=True).stdout
    addrs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in names:
            addrs[parts[2]] = int(parts[0], 16)
    missing = [n for n in names if n not in addrs]
    if missing:
        sys.exit(f"FAIL: symbols not in {elf}: {missing}")
    return addrs


A = resolve(SYMBOLS)
CONFIG_CHANGED = A["configChanged"]
FS_ACTIVE = A["filesystemActive"]
FRAME_HOLD = A["core1FrameHoldDepth"]   # uint32[2] - hold depth per core (T3.4)
LAST_READ_RAW = A["lastReadRaw"]
SWITCH_POS = A["switchPosition"]
PROBE_ACTIVE = A["probeActive"]

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 60
o = OOCD()
t0 = time.time()
prev_flags = None
prev_raw = None
print(f"watching {secs}s - tap pads now "
      f"(switchPosition={o.read_mem(SWITCH_POS, 4)}, "
      f"probeActive={o.read_mem(PROBE_ACTIVE, 4)})", flush=True)

while time.time() - t0 < secs:
    raw = o.read_mem(LAST_READ_RAW, 4)
    hold = (o.read_mem(FRAME_HOLD, 4), o.read_mem(FRAME_HOLD + 4, 4))
    flags = (o.read_mem(CONFIG_CHANGED, 1), o.read_mem(FS_ACTIVE, 1), hold)
    ts = time.time() - t0
    if raw != prev_raw:
        print(f"+{ts:7.2f}s  lastReadRaw {prev_raw} -> {raw}", flush=True)
        prev_raw = raw
    if flags != prev_flags:
        print(f"+{ts:7.2f}s  cfgChanged={flags[0]} fsActive={flags[1]} "
              f"frameHold={hold[0]}+{hold[1]}", flush=True)
        prev_flags = flags
    time.sleep(0.04)

print("done", flush=True)
