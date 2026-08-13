#!/usr/bin/env python3
"""Interactive tap-session monitor. Run while Kevin taps pads in select mode.

Samples over SWD at ~20Hz and logs, with timestamps:
  - every change in lastReadRaw (the raw pad-ladder ADC value the probe
    row-mapper consumed) - shows exactly what the firmware saw per tap
  - every configChanged / filesystemActive / pauseCore2 transition - shows
    save windows overlapping the taps

Usage: python3 tap_session.py [seconds]   (default 60)
"""
import sys
import time

sys.path.insert(0, "/private/tmp/claude-501/-Users-kevinsanto-Documents-GitHub-JumperlOS/71bd2bdc-2364-4288-956f-bed51d3f7d16/scratchpad")
from sample_state import OOCD  # noqa: E402

CONFIG_CHANGED = 0x2004547a
FS_ACTIVE = 0x2004549c
PAUSE = 0x200454bb
LAST_READ_RAW = 0x2003ac90
SWITCH_POS = 0x20044118
PROBE_ACTIVE = 0x20040df8

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
    flags = (o.read_mem(CONFIG_CHANGED, 1), o.read_mem(FS_ACTIVE, 1),
             o.read_mem(PAUSE, 1))
    ts = time.time() - t0
    if raw != prev_raw:
        print(f"+{ts:7.2f}s  lastReadRaw {prev_raw} -> {raw}", flush=True)
        prev_raw = raw
    if flags != prev_flags:
        print(f"+{ts:7.2f}s  cfgChanged={flags[0]} fsActive={flags[1]} "
              f"pauseCore2={flags[2]}", flush=True)
        prev_flags = flags
    time.sleep(0.04)

print("done", flush=True)
