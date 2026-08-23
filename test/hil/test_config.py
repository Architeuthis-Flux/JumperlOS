#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Config round-trip: runtime toggle applies + persists, and the new
release keys (probe_droop_v0, crosspoint_resistance, net_currents,
net_voltage_scan) exist in /config.txt."""

import time

from jl import jl_exec, port1_command, check, finish, device_text


def read_config():
    # Streams; never builds /config.txt as one on-device string. See
    # jl.device_text() for why that mattered - this call site is one of the two
    # that produced the suite's recurring MemoryError.
    return device_text("/config.txt")


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
# The rewrite reads the file into a LIST OF LINES rather than one string.
# Same total bytes, but the peak CONTIGUOUS allocation drops from the whole
# file (~3.1 KB, the exact size W3-T4's `allocating 3115 bytes` failed on) to
# the longest single line (~60 B) - and a MemoryError here is always a
# contiguity failure, not an out-of-memory one. Byte-exactness was verified
# offline against trailing-newline / no-trailing-newline / blank-line / CRLF /
# needle-straddles-a-chunk-boundary inputs at chunk sizes 512, 256, 7 and 1.
plant = jl_exec(f"""
a = "probe_droop_ohms = {ohms_before};"
b = "probe_droop_v0 = {v0_before};"
# FAIL-SAFE sentinels: if this test dies between planting and the save that
# overwrites them, the values left on disk must not be believable. -55.5 is
# non-positive so infraProbeDroopOhms() falls through to its empirical default
# (55.5 sat inside SelfTest's own 10-400 acceptance band and would have flipped
# the board to a ~1.85x underestimate of every droop current, silently); 2.599
# is outside the [3.0, 3.6] clamp in configManager, so it resets on load.
lines = []
buf = ""
seen_a = False
seen_b = False
f = jfs.open("/config.txt", "r")
done = False
while not done:
    chunk = jfs.read(f, 512)
    if not chunk:
        done = True
    else:
        buf += chunk
    while "\\n" in buf:
        ln, buf = buf.split("\\n", 1)
        if a in ln:
            ln = ln.replace(a, "probe_droop_ohms = -55.5;"); seen_a = True
        if b in ln:
            ln = ln.replace(b, "probe_droop_v0 = 2.599;"); seen_b = True
        lines.append(ln + "\\n")
jfs.close(f)
if buf:
    if a in buf:
        buf = buf.replace(a, "probe_droop_ohms = -55.5;"); seen_a = True
    if b in buf:
        buf = buf.replace(b, "probe_droop_v0 = 2.599;"); seen_b = True
    lines.append(buf)
ok = seen_a and seen_b
if ok:
    f = jfs.open("/config.txt", "w")
    for ln in lines:
        jfs.write(f, ln)
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
    # Same line-list rewrite as the plant above: this runs in a finally, on a
    # heap that a failing test may already have chewed up, so it is exactly the
    # place that must not need a 3 KB contiguous allocation to put the user's
    # calibration back.
    jl_exec(f"""
lines = []
buf = ""
changed = False
f = jfs.open("/config.txt", "r")
done = False
while not done:
    chunk = jfs.read(f, 512)
    if not chunk:
        done = True
    else:
        buf += chunk
    while "\\n" in buf:
        ln, buf = buf.split("\\n", 1)
        if "probe_droop_ohms = -55.5;" in ln:
            ln = ln.replace("probe_droop_ohms = -55.5;", "probe_droop_ohms = {ohms_before};")
            changed = True
        if "probe_droop_v0 = 2.599;" in ln:
            ln = ln.replace("probe_droop_v0 = 2.599;", "probe_droop_v0 = {v0_before};")
            changed = True
        lines.append(ln + "\\n")
jfs.close(f)
if buf:
    if "probe_droop_ohms = -55.5;" in buf:
        buf = buf.replace("probe_droop_ohms = -55.5;", "probe_droop_ohms = {ohms_before};")
        changed = True
    if "probe_droop_v0 = 2.599;" in buf:
        buf = buf.replace("probe_droop_v0 = 2.599;", "probe_droop_v0 = {v0_before};")
        changed = True
    lines.append(buf)
if changed:
    f = jfs.open("/config.txt", "w")
    for ln in lines:
        jfs.write(f, ln)
    jfs.close(f)
print("restored" if changed else "already clean")
""", timeout=20)



# --- 4. `reset preserves the WHOLE calibration (and hardware) struct --------
# resetConfigToDefaults() used to restore calibration from a hand-maintained
# field list that omitted probe_droop_ohms / probe_droop_v0, probe_pad_ohms,
# crosspoint_resistance, the hysteresis pair and probe_max/min_measure - so a
# `reset (or the menu's reset) silently zeroed the self test's droop
# calibration ("probe_droop_ohms reverts to 0.0" in the handoff). It copies the
# structs now. Plant a sentinel droop resistance, run `reset, and read it back.
# Every non-calibration key the reset touched is put back afterwards through
# the normal `[section] key = value; path, so the board leaves this test with
# the config it came in with.

def parse_cfg(text):
    out = {}
    section = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif "=" in line and section:
            k, v = line.split("=", 1)
            out[(section, k.strip())] = v.strip().rstrip(";").strip()
    return out


CAL_KEYS = ["probe_droop_ohms", "probe_droop_v0", "probe_pad_ohms",
            "crosspoint_resistance", "probe_switch_threshold_high",
            "probe_switch_threshold_low", "probe_max_measure",
            "probe_max_measure_gpio", "probe_min_measure",
            "measure_mode_output_voltage"]
snapshot = read_config()
snap = parse_cfg(snapshot)
ohms_orig = snap.get(("calibration", "probe_droop_ohms"))
# A believable but unmistakable sentinel (inside the self test's 10-400 band
# so nothing clamps it away): if it survives `reset, the struct copy works.
resp = port1_command("`[calibration] probe_droop_ohms = 123.4", collect_seconds=2)
time.sleep(1.5)
planted = parse_cfg(read_config()).get(("calibration", "probe_droop_ohms"))
check(planted is not None and abs(float(planted) - 123.4) < 0.05,
      f"sentinel probe_droop_ohms = 123.4 planted ({planted})")
try:
    resp = port1_command("`reset", collect_seconds=4)
    check("reset to defaults" in resp.lower(), "`reset acknowledged")
    time.sleep(2.5)
    after = parse_cfg(read_config())
    got = after.get(("calibration", "probe_droop_ohms"))
    check(got is not None and abs(float(got) - 123.4) < 0.05,
          f"probe_droop_ohms survived `reset (got {got})")
    for k in CAL_KEYS:
        if k == "probe_droop_ohms":
            continue
        check(after.get(("calibration", k)) == snap.get(("calibration", k)),
              f"calibration.{k} survived `reset ({snap.get(('calibration', k))})")
    for k in ("generation", "revision", "probe_revision", "probe_led_on_button_pin"):
        check(after.get(("hardware", k)) == snap.get(("hardware", k)),
              f"hardware.{k} survived `reset ({snap.get(('hardware', k))})")
finally:
    # Put the sentinel's original back first, then every other key `reset
    # changed (skip [config]/[firmware] version lines the firmware owns).
    after = parse_cfg(read_config())
    if ohms_orig is not None:
        port1_command(f"`[calibration] probe_droop_ohms = {ohms_orig}", collect_seconds=1.5)
    restored = 0
    for (sec, key), val in snap.items():
        if sec == "calibration" or key == "firmware_version":
            continue
        if after.get((sec, key)) != val:
            port1_command(f"`[{sec}] {key} = {val}", collect_seconds=1.5)
            restored += 1
    time.sleep(2.5)
    final = parse_cfg(read_config())
    diffs = [(sk, v, final.get(sk)) for sk, v in snap.items()
             if sk[1] != "firmware_version" and final.get(sk) != v]
    check(not diffs, f"config restored to its pre-reset contents ({restored} keys re-applied; leftover diffs: {diffs[:4]})")
    # The restore loop's last writes arm a deferred config save. Its flash
    # window stalls USB, and in run_all.py the NEXT test file opens the port
    # right about here - which surfaces as "device reports readiness to read
    # but returned no data" in a test that passes standalone. Let it land.
    time.sleep(5)

finish("test_config")
