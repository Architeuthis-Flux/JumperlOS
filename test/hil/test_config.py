#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Config round-trip: runtime toggle applies + persists, and the reorganized
keys ([probe] droop_v0, [measurement] crosspoint_resistance/net_currents,
[debug] net_voltage_scan) exist in /config.txt.

Updated for the table-driven config (2026-08): probe calibration lives in
[probe] with the probe_ prefix dropped (droop_v0, droop_ohms, pad_ohms,
pad_max_measure...), crosspoint_resistance/net_currents moved to
[measurement], and saveConfigIncremental is gone - every save is a full
table-driven rewrite from memory (which is exactly what the sentinel
phase verifies)."""

import os
import time

from jl import jl_exec, port1_command, check, finish, device_text


def read_config():
    # Streams; never builds /config.txt as one on-device string. See
    # jl.device_text() for why that mattered - this call site is one of the two
    # that produced the suite's recurring MemoryError.
    return device_text("/config.txt")


cfg = read_config()
for key in ("droop_v0", "crosspoint_resistance", "net_currents",
            "net_voltage_scan", "[probe]", "[measurement]", "[clickwheel]",
            "[terminal]", "[undo]"):
    check(key in cfg, f"/config.txt contains new key/section '{key}'")


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
# Droop keys survive a deferred save. Historically this guarded the
# incremental writer's missing `updated = true` branches; incremental save is
# gone now (every save is a full table-driven rewrite from memory), so this
# phase now verifies exactly that: sentinels planted in the FILE only must be
# overwritten with the in-memory values on the next deferred save.
#
# Recipe: plant sentinel values in the FILE only (memory untouched), then
# trigger a deferred save with the 'k' toggle - it flips
# top_oled.show_in_terminal in memory and sets configChanged, so the
# ConfigSaveService persists via saveConfig().
# ---------------------------------------------------------------------------

def config_value(cfg_text, key):
    for line in cfg_text.splitlines():
        if line.strip().startswith(key):
            return line.split("=")[1].strip().rstrip(";").strip()
    return None


cfg = read_config()
ohms_before = config_value(cfg, "droop_ohms")
v0_before = config_value(cfg, "droop_v0")
check(ohms_before is not None, f"droop_ohms present ({ohms_before})")
check(v0_before is not None, f"droop_v0 present ({v0_before})")

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
a = "droop_ohms = {ohms_before};"
b = "droop_v0 = {v0_before};"
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
            ln = ln.replace(a, "droop_ohms = -55.5;"); seen_a = True
        if b in ln:
            ln = ln.replace(b, "droop_v0 = 2.599;"); seen_b = True
        lines.append(ln + "\\n")
jfs.close(f)
if buf:
    if a in buf:
        buf = buf.replace(a, "droop_ohms = -55.5;"); seen_a = True
    if b in buf:
        buf = buf.replace(b, "droop_v0 = 2.599;"); seen_b = True
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
        if (config_value(now, "droop_ohms") == ohms_before
                and config_value(now, "droop_v0") == v0_before):
            reverted = True
            break
    check(reverted, "deferred save rewrote droop keys from memory "
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
        if "droop_ohms = -55.5;" in ln:
            ln = ln.replace("droop_ohms = -55.5;", "droop_ohms = {ohms_before};")
            changed = True
        if "droop_v0 = 2.599;" in ln:
            ln = ln.replace("droop_v0 = 2.599;", "droop_v0 = {v0_before};")
            changed = True
        lines.append(ln + "\\n")
jfs.close(f)
if buf:
    if "droop_ohms = -55.5;" in buf:
        buf = buf.replace("droop_ohms = -55.5;", "droop_ohms = {ohms_before};")
        changed = True
    if "droop_v0 = 2.599;" in buf:
        buf = buf.replace("droop_v0 = 2.599;", "droop_v0 = {v0_before};")
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


# Calibration-flagged keys and where they live post-reorg: probe calibration
# in [probe] (probe_ prefix dropped), crosspoint_resistance in [measurement],
# DAC/ADC curves still in [calibration].
CAL_KEYS = [("probe", "droop_ohms"), ("probe", "droop_v0"),
            ("probe", "pad_ohms"), ("measurement", "crosspoint_resistance"),
            ("probe", "switch_threshold_high"), ("probe", "switch_threshold_low"),
            ("probe", "pad_max_measure"), ("probe", "pad_max_measure_gpio"),
            ("probe", "pad_min_measure"), ("probe", "measure_voltage"),
            ("calibration", "dac_0_zero"), ("calibration", "adc_0_spread")]
snapshot = read_config()
snap = parse_cfg(snapshot)
ohms_orig = snap.get(("probe", "droop_ohms"))
# A believable but unmistakable sentinel (inside the self test's 10-400 band
# so nothing clamps it away): if it survives `reset, the CAL-flag copy works.
# Written through the LEGACY alias on purpose - this also regression-tests
# that old `[calibration] probe_droop_ohms lines still land in probe.droop_ohms.
resp = port1_command("`[calibration] probe_droop_ohms = 123.4", collect_seconds=2)
time.sleep(1.5)
planted = parse_cfg(read_config()).get(("probe", "droop_ohms"))
check(planted is not None and abs(float(planted) - 123.4) < 0.05,
      f"sentinel droop_ohms = 123.4 planted via legacy alias ({planted})")
try:
    resp = port1_command("`reset", collect_seconds=4)
    check("reset to defaults" in resp.lower(), "`reset acknowledged")
    time.sleep(2.5)
    after = parse_cfg(read_config())
    got = after.get(("probe", "droop_ohms"))
    check(got is not None and abs(float(got) - 123.4) < 0.05,
          f"probe.droop_ohms survived `reset (got {got})")
    for sec, k in CAL_KEYS:
        if (sec, k) == ("probe", "droop_ohms"):
            continue
        check(after.get((sec, k)) == snap.get((sec, k)),
              f"{sec}.{k} survived `reset ({snap.get((sec, k))})")
    for sec, k in (("hardware", "generation"), ("hardware", "revision"),
                   ("hardware", "probe_revision")):
        check(after.get((sec, k)) == snap.get((sec, k)),
              f"{sec}.{k} survived `reset ({snap.get((sec, k))})")
    # led_on_button_pin moved from the reset-preserved [hardware] struct into
    # [probe]; after `reset it should be at its default (true) - the value
    # that makes the flaky-cable workaround active.
    check(after.get(("probe", "led_on_button_pin")) in ("true", "1"),
          "probe.led_on_button_pin at default after `reset")

    # THE NEGATIVE CONTROL FOR THE RESTORE BELOW, on demand.
    #
    # On a healthy board `reset preserves calibration by struct copy, so the
    # finally's restore loop has nothing to do and "the calibration is put back"
    # is untestable on a green run - which is exactly how the hole survived: the
    # skip was load-bearing ONLY in the red case, where it hurt. Set
    # H2_INJECT_CAL_DAMAGE=1 to damage one calibration key here and watch the
    # teardown either repair it (fixed) or leave the board mis-calibrated while
    # reporting the damage as a `leftover diffs:` list (the old behaviour).
    #
    # Both directions were run on the bench; see the H2 report.
    if os.environ.get("H2_INJECT_CAL_DAMAGE"):
        port1_command("`[calibration] probe_pad_ohms = 77.7", collect_seconds=2)
        time.sleep(1.5)
        print("  info: H2_INJECT_CAL_DAMAGE - probe_pad_ohms forced to 77.7 to "
              "stand in for a `reset that wiped calibration. The teardown must "
              "put it back.")
finally:
    # Put the sentinel's original back first, then every other key `reset
    # changed (skip the [firmware] version line the firmware owns).
    #
    # CALIBRATION IS RESTORED TOO, and that is the H2 fix. This loop used to
    # open `if sec == "calibration" or key == "firmware_version": continue`,
    # which is a no-op on a green run (reset preserves calibration by struct
    # copy, so nothing differs) and load-bearing on a RED one - the run that
    # correctly catches a regression in resetConfigToDefaults was the run that
    # left probe_pad_ohms, crosspoint_resistance, the hysteresis pair,
    # probe_max/min_measure and measure_mode_output_voltage sitting at defaults
    # on Kevin's board, with every probe measurement and every droop-compensated
    # current wrong until he re-ran the self test, and nothing telling him to.
    # The loss was reported only inside a truncated `leftover diffs:` list.
    # Proven both ways on the bench with H2_INJECT_CAL_DAMAGE - see the report.
    #
    # The sentinel's own key goes back FIRST, and `after` is read AFTER it: the
    # phase deliberately planted 123.4 there, so leaving it in the read would
    # make probe_droop_ohms show up as "a calibration key `reset wiped" on every
    # single green run and train everyone to ignore the warning below.
    if ohms_orig is not None:
        port1_command(f"`[calibration] probe_droop_ohms = {ohms_orig}", collect_seconds=1.5)
        time.sleep(1.5)
    after = parse_cfg(read_config())
    restored = 0
    cal_repaired = []
    for (sec, key), val in snap.items():
        if key == "firmware_version":
            continue
        if after.get((sec, key)) != val:
            port1_command(f"`[{sec}] {key} = {val}", collect_seconds=1.5)
            restored += 1
            if sec == "calibration":
                cal_repaired.append(key)
    time.sleep(2.5)
    final = parse_cfg(read_config())
    diffs = [(sk, v, final.get(sk)) for sk, v in snap.items()
             if sk[1] != "firmware_version" and final.get(sk) != v]
    if cal_repaired:
        print("  WARNING: `reset CHANGED calibration keys that it must preserve "
              f"({', '.join(sorted(cal_repaired))}). They have been written back "
              "from this test's phase-0 snapshot of /config.txt, so the board is "
              "usable - but the survival checks above are the real result and "
              "resetConfigToDefaults() has regressed.")
    cal_still_wrong = sorted(k for (sec, k), v in snap.items()
                             if sec == "calibration" and final.get((sec, k)) != v)
    if cal_still_wrong:
        print("  *** YOUR CALIBRATION WAS RESET AND COULD NOT BE RESTORED: "
              + ", ".join(cal_still_wrong) + " - RE-RUN THE SELF TEST before "
              "trusting any probe measurement or current reading. ***")
    check(not diffs, f"config restored to its pre-reset contents ({restored} keys re-applied; leftover diffs: {diffs[:4]})")
    # The restore loop's last writes arm a deferred config save. Its flash
    # window stalls USB, and in run_all.py the NEXT test file opens the port
    # right about here - which surfaces as "device reports readiness to read
    # but returned no data" in a test that passes standalone. Let it land.
    time.sleep(5)

finish("test_config")
