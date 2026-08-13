#!/usr/bin/env python3
"""Sample Jumperless cross-core state over OpenOCD's telnet port (4444).

Reads the concurrency flags from RAM (background DAP access, non-halting when
possible) and optionally snapshots both cores' PC/LR with a brief halt+resume.
"""
import socket
import sys
import re
import time

VARS = {
    "core1busy":            (0x20045480, 1),
    "core2busy":            (0x20045481, 1),
    "pauseCore2":           (0x200454bb, 1),
    "readingADC":           (0x200454c1, 1),
    "refreshInProgress":    (0x200454c3, 1),
    "refreshLocalInProgress": (0x200454c4, 1),
    "filesystemActive":     (0x2004549c, 1),
    "usbMountedByHost":     (0x200454d6, 1),
    "sendAllPathsCore2":    (0x200421dc, 4),
    "showLEDsCore2":        (0x200422bc, 4),
    "chipSelect":           (0x2002c3d0, 4),
    "ch446q_timeout_count": (0x2002bb14, 4),
    "sendxy_blocked_count": (0x200421e0, 4),
    "fs_mutex_core1_acquires": (0x2002d6d4, 4),
}


class OOCD:
    def __init__(self, host="localhost", port=4444):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(2)
        self._drain()

    def _drain(self):
        buf = b""
        try:
            while True:
                chunk = self.s.recv(4096)
                if not chunk:
                    break
                buf += chunk
                if buf.rstrip().endswith(b">"):
                    break
        except socket.timeout:
            pass
        return buf.decode("utf-8", "replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        return self._drain()

    def read_mem(self, addr, size):
        unit = {1: "mdb", 4: "mdw"}[size]
        out = self.cmd(f"{unit} 0x{addr:08x}")
        m = re.search(r":\s*([0-9a-fA-F]+)", out)
        return int(m.group(1), 16) if m else None


def sample_vars(o):
    vals = {}
    for name, (addr, size) in VARS.items():
        vals[name] = o.read_mem(addr, size)
    return vals


def sample_pcs(o):
    """Brief halt of each core to read PC/LR, then resume. ~10ms disturbance."""
    pcs = {}
    for core in ("rp2350.dap.core0", "rp2350.dap.core1"):
        o.cmd(f"targets {core}")
        o.cmd("halt")
        out = o.cmd("reg pc")
        m = re.search(r"pc[^:]*:\s*(0x[0-9a-fA-F]+)", out)
        pc = m.group(1) if m else "?"
        out = o.cmd("reg lr")
        m = re.search(r"lr[^:]*:\s*(0x[0-9a-fA-F]+)", out)
        lr = m.group(1) if m else "?"
        o.cmd("resume")
        pcs[core.split(".")[-1]] = (pc, lr)
    o.cmd("targets rp2350.dap.core0")
    return pcs


if __name__ == "__main__":
    want_pcs = "--pcs" in sys.argv
    o = OOCD()
    vals = sample_vars(o)
    line = " ".join(f"{k}={v}" for k, v in vals.items())
    print(f"[{time.strftime('%H:%M:%S')}] {line}")
    if want_pcs:
        pcs = sample_pcs(o)
        for core, (pc, lr) in pcs.items():
            print(f"  {core}: pc={pc} lr={lr}")
