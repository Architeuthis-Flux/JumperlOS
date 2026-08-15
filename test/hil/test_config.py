#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Config round-trip: runtime toggle applies + persists, and the new
release keys (probe_droop_v0, crosspoint_resistance, net_currents,
net_voltage_scan) exist in /config.txt."""

import time

from jl import jl_exec, port1_command, check, finish


def read_config():
    return jl_exec("""
f = jfs.open("/config.txt", "r")
data = ""
while True:
    chunk = jfs.read(f, 512)
    if not chunk:
        break
    data += chunk
jfs.close(f)
print(data)
""", timeout=20)


cfg = read_config()
for key in ("probe_droop_v0", "crosspoint_resistance", "net_currents",
            "net_voltage_scan"):
    check(key in cfg, f"/config.txt contains new key '{key}'")


def net_currents_value(cfg_text):
    for line in cfg_text.splitlines():
        if line.strip().startswith("net_currents"):
            return line.split("=")[1].strip().rstrip(";").strip()
    return None


before = net_currents_value(cfg)
check(before in ("0", "1", "true", "false"), f"net_currents has a sane value ({before})")

# Toggle via the 'i' command (updateConfigValue -> incremental save), wait
# for the idle flush, verify the file changed, then restore.
resp = port1_command("i")
check("net current scan" in resp, "'i' toggle acknowledged")

flipped = None
for _ in range(20):  # idle flush is lazy; poll up to ~20s
    time.sleep(1.0)
    now = net_currents_value(read_config())
    if now is not None and now != before:
        flipped = now
        break
check(flipped is not None, f"toggle persisted to /config.txt ({before} -> {flipped})")

resp = port1_command("i")  # restore original setting
check("net current scan" in resp, "restore toggle acknowledged")


# ---------------------------------------------------------------------------
# Droop keys survive an incremental save (regression: the incremental writer's
# probe_droop_v0 / probe_droop_ohms branches were missing `updated = true`, so
# saveConfigIncremental kept the file's stale lines verbatim forever and the
# self test's measured droop calibration never reached flash).
#
# Recipe: plant sentinel values in the FILE only (memory untouched), then
# trigger an incremental save with the 'k' toggle - it flips
# top_oled.show_in_terminal in memory and sets configChanged, so the
# ConfigSaveService persists via saveConfig() -> saveConfigIncremental (the
# 'i' toggle won't do: updateConfigValue ends in a FULL saveConfigToFile,
# which rewrites droop lines even without the fix). A correct incremental
# writer rebuilds every known key from memory, so the sentinels must be
# overwritten with the in-memory values; the broken writer kept the file
# lines as-is.
# ---------------------------------------------------------------------------

def config_value(cfg_text, key):
    for line in cfg_text.splitlines():
        if line.strip().startswith(key):
            return line.split("=")[1].strip().rstrip(";").strip()
    return None


cfg = read_config()
ohms_before = config_value(cfg, "probe_droop_ohms")
v0_before = config_value(cfg, "probe_droop_v0")
check(ohms_before is not None, f"probe_droop_ohms present ({ohms_before})")
check(v0_before is not None, f"probe_droop_v0 present ({v0_before})")

# Sentinels: values no calibration would produce. Substitute on-device so
# the file content never round-trips through the REPL transport (which
# would risk newline mangling on the write-back).
plant = jl_exec(f"""
f = jfs.open("/config.txt", "r")
data = ""
while True:
    chunk = jfs.read(f, 512)
    if not chunk:
        break
    data += chunk
jfs.close(f)
a = "probe_droop_ohms = {ohms_before};"
b = "probe_droop_v0 = {v0_before};"
ok = (a in data) and (b in data)
# FAIL-SAFE sentinels: if this test dies between planting and the save that
# overwrites them, the values left on disk must not be believable. -55.5 is
# non-positive so infraProbeDroopOhms() falls through to its empirical default
# (55.5 sat inside SelfTest's own 10-400 acceptance band and would have flipped
# the board to a ~1.85x underestimate of every droop current, silently); 2.599
# is outside the [3.0, 3.6] clamp in configManager, so it resets on load.
data = data.replace(a, "probe_droop_ohms = -55.5;")
data = data.replace(b, "probe_droop_v0 = 2.599;")
f = jfs.open("/config.txt", "w")
jfs.write(f, data)
jfs.close(f)
print("planted" if ok else "droop lines MISSING from file")
""", timeout=20)
check("planted" in plant, "sentinel droop lines planted in /config.txt")

# From here the file holds sentinels, so every exit path must put the real
# values back. check() never aborts, but jl_exec() sys.exit()s on a transport
# failure and the user can Ctrl-C - both unwind through finally.
try:
    resp = port1_command("k")  # dirty a key -> deferred incremental save
    check("OLED in terminal" in resp, "'k' toggle (incremental save trigger) acknowledged")

    reverted = False
    for _ in range(20):
        time.sleep(1.0)
        now = read_config()
        if (config_value(now, "probe_droop_ohms") == ohms_before
                and config_value(now, "probe_droop_v0") == v0_before):
            reverted = True
            break
    check(reverted, "incremental save rewrote droop keys from memory "
                    f"(ohms {ohms_before}, v0 {v0_before} restored over sentinels)")

    resp = port1_command("k")  # restore show_in_terminal
    check("OLED in terminal" in resp, "restore toggle acknowledged")
finally:
    # Belt and braces: put the originals back on device even if the save above
    # never ran. Cheap and idempotent when the save already did the job.
    jl_exec(f"""
f = jfs.open("/config.txt", "r")
data = ""
while True:
    chunk = jfs.read(f, 512)
    if not chunk:
        break
    data += chunk
jfs.close(f)
changed = False
if "probe_droop_ohms = -55.5;" in data:
    data = data.replace("probe_droop_ohms = -55.5;", "probe_droop_ohms = {ohms_before};")
    changed = True
if "probe_droop_v0 = 2.599;" in data:
    data = data.replace("probe_droop_v0 = 2.599;", "probe_droop_v0 = {v0_before};")
    changed = True
if changed:
    f = jfs.open("/config.txt", "w")
    jfs.write(f, data)
    jfs.close(f)
print("restored" if changed else "already clean")
""", timeout=20)

finish("test_config")
