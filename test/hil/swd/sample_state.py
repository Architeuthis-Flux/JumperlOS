#!/usr/bin/env python3
"""Sample Jumperless cross-core state over OpenOCD's telnet port (4444).

Reads the concurrency flags from RAM (background DAP access, non-halting when
possible) and optionally snapshots both cores' PC/LR with a brief halt+resume.
"""
import glob
import os
import socket
import subprocess
import sys
import re
import time

# Symbol -> size. Addresses are resolved from the ELF at import time (below),
# so this no longer goes stale with every build. The ELF is JL_ELF if set,
# else the release build, else the debug build - whichever is flashed.
VAR_SIZES = {
    "core1busy":              1,
    "core2busy":              1,
    "pauseCore2":             1,
    "readingADC":             1,
    "refreshInProgress":      1,
    "refreshLocalInProgress": 1,
    "filesystemActive":       1,
    "usbMountedByHost":       1,
    "sendAllPathsCore2":      4,
    "showLEDsCore2":          4,
    "chipSelect":             4,
    "ch446q_timeout_count":   4,
    "sendxy_blocked_count":   4,
    "fs_mutex_core1_acquires": 4,
}

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))


def find_elf():
    env = os.environ.get("JL_ELF")
    cands = [env] if env else []
    cands += [os.path.join(_REPO, ".pio/build/jumperless_v5/firmware.elf"),
              os.path.join(_REPO, ".pio/build/jumperless_v5_debug/firmware.elf")]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def find_nm():
    env = os.environ.get("JL_NM")
    if env and os.path.isfile(env):
        return env
    hits = glob.glob(os.path.expanduser(
        "~/.platformio/packages/toolchain-rp2040-earlephilhower/bin/arm-none-eabi-nm"))
    if hits:
        return hits[0]
    return "arm-none-eabi-nm"   # hope it is on PATH


def resolve_vars(elf=None):
    """{name: (addr, size)} for VAR_SIZES, read from the ELF's symbol table.
    Empty (with a warning) if there is no ELF - callers that only need USB
    liveness, like stress_flash.py --no-swd, must still import."""
    elf = elf or find_elf()
    if elf is None:
        print("(sample_state: no firmware.elf found - set JL_ELF or build "
              "jumperless_v5; SWD sampling unavailable)", file=sys.stderr)
        return {}
    out = subprocess.run([find_nm(), elf], capture_output=True, text=True, check=True).stdout
    addrs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in VAR_SIZES:
            addrs[parts[2]] = int(parts[0], 16)
    missing = [n for n in VAR_SIZES if n not in addrs]
    if missing:
        sys.exit(f"FAIL: symbols not in {elf}: {missing}")
    return {n: (addrs[n], sz) for n, sz in VAR_SIZES.items()}


VARS = resolve_vars()


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
    if not VARS:
        sys.exit("FAIL: no symbol addresses (no firmware.elf) - cannot sample over SWD.")
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
    print(f"symbols from {find_elf()}")
    o = OOCD()
    vals = sample_vars(o)
    line = " ".join(f"{k}={v}" for k, v in vals.items())
    print(f"[{time.strftime('%H:%M:%S')}] {line}")
    if want_pcs:
        pcs = sample_pcs(o)
        for core, (pc, lr) in pcs.items():
            print(f"  {core}: pc={pc} lr={lr}")
