#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""InfraPaths regressions: probe-power candidate arbitration (relocate on
user claim, switch-back on release), no-duplicate feed routing, slot-file
purity (no BUF_IN in either YAML section), lock survival across clears,
and the i@ status readout."""

import re
import time

from jl import jl_exec, port1_command, check, finish


def infra_status():
    return port1_command("i@", collect_seconds=2.5)


def bridge_dump():
    return port1_command("b", collect_seconds=3)


def feed_source(dump):
    """The node feeding BUF_IN per the bridge array line '[BUF_IN,XXX,Net n]'."""
    m = re.search(r"\[BUF_IN,([A-Za-z0-9_]+),Net", dump)
    if not m:
        m = re.search(r"\[([A-Za-z0-9_]+),BUF_IN,Net", dump)
    return m.group(1) if m else None


def feed_path_rows(dump):
    """Routed path-table rows whose nodes include BUF_IN."""
    rows = []
    for line in dump.splitlines():
        if "BUF_IN" in line and "\t" in line:
            rows.append(line.strip())
    return rows


# --- 0. clean state: a prior aborted run can leave user bridges (even a
# BUFFER_IN one, which makes probe power yield) that corrupt every check ----
jl_exec("nodes_clear()\nprint('clean')", timeout=25)
time.sleep(4)  # let the clear's autosave land before opening port 1

# --- 1. boot state: exactly one routed feed path, dup=0 ---------------------
d = bridge_dump()
src0 = feed_source(d)
check(src0 is not None, f"probe power feed present at boot (source={src0})")
rows = feed_path_rows(d)
check(len(rows) == 1, f"feed has exactly ONE routed path (no duplicates), got {len(rows)}")
if rows:
    cols = rows[0].split("\t")
    check(len(cols) > 12 and cols[12].strip() == "0", "feed path dup column is 0")

status = infra_status()
check("probe_power" in status and "->" in status, "i@ shows probe_power with an active candidate")
check("droop ohms" in status, "i@ reports the droop resistance")

# --- 2. claim the active feed resource -> relocation ------------------------
# Boot feed is DAC_0 (the INA1-sensed path); claiming it must drop the feed
# down the fall-through chain (DAC_0 -> GPIO8..GPIO1).
claim_py = {"DAC_0": "DAC0", "DAC_1": "DAC1"}.get(src0, src0.replace("GP_", "GPIO_"))
out = jl_exec(f"connect({claim_py}, 25)\nprint('ok')", timeout=25)
time.sleep(1.5)
d = bridge_dump()
src1 = feed_source(d)
check(src1 is not None and src1 != src0,
      f"feed RELOCATED after claiming {claim_py} ({src0} -> {src1})")

# --- 3. release -> switch-back ---------------------------------------------
out = jl_exec(f"disconnect({claim_py}, -1)\nprint('ok')", timeout=25)
time.sleep(1.5)
d = bridge_dump()
src2 = feed_source(d)
check(src2 == src0, f"feed SWITCHED BACK on release ({src1} -> {src2})")

# --- 4. lock/feed survival across nodes_clear ------------------------------
out = jl_exec("nodes_clear()\nprint('ok')", timeout=25)
time.sleep(1.5)
d = bridge_dump()
check(feed_source(d) is not None, "feed survives nodes_clear() (re-added by evaluation)")

# --- 5. slot purity: fresh save has no BUF_IN in bridges OR nets ------------
out = jl_exec("connect(12, 17)\nprint('ok')", timeout=25)
time.sleep(6)  # dirty-state autosave
out = jl_exec("""
# Streamed with a 14-byte overlap window (one less than the longest
# needle, "ROUTABLE_BUFFER") so a match straddling a chunk boundary is
# still seen, and the device never holds the whole file as one string.
# The old `data += chunk` shape here is the same one that produced this
# suite's recurring MemoryError - see jl.device_text().
f = jfs.open("/slots/slot0.yaml", "r")
buf = ""
bufin = 0
usernet = 0
while True:
    chunk = jfs.read(f, 512)
    if not chunk:
        break
    buf += chunk
    if "BUF_IN" in buf or "ROUTABLE_BUFFER" in buf:
        bufin = 1
    if "12, 17" in buf or "12,17" in buf:
        usernet = 1
    if len(buf) > 14:
        buf = buf[-14:]
jfs.close(f)
print("has_bufin=", bufin)
print("has_user_net=", usernet)
disconnect(12, -1)
print("done")
""", timeout=30)
check("has_bufin= 0" in out, "fresh slot save contains no BUF_IN (bridges or nets section)")
check("has_user_net= 1" in out, "fresh slot save DOES contain the user net (save actually ran)")
# The disconnect above arms another autosave; its flash window stalls USB
# briefly and breaks the next raw-REPL open (fail-fast, no retries by
# policy). Let it land before continuing.
time.sleep(4)

# --- 6. user bridge to BUF_IN is ALLOWED and the feed yields ----------------
out = jl_exec("""
ok = connect(139, 30)
print("bk=", 1 if is_connected(139, 30) else 0)
""", timeout=25)
check("bk= 1" in out, "user bridge to ROUTABLE_BUFFER_IN is allowed (permissive)")
time.sleep(1.5)
d = bridge_dump()
check(feed_source(d) in (None, "30"), "probe power YIELDED to the user's BUFFER_IN bridge")
out = jl_exec("disconnect(139, -1)\nprint('ok')", timeout=25)
time.sleep(1.5)
d = bridge_dump()
check(feed_source(d) is not None and feed_source(d) != "30",
      f"feed returned after the user disconnected (source={feed_source(d)})")

# --- 7. RouteSafety self-check still passes with the infra feed live --------
out = port1_command("i?", collect_seconds=6)
check("PASS" in out or "pass" in out, "RouteSafety self-check PASS with infra feed routed")


# --- 8. GPIO-first order (dacs.probe_power_source = 1) ---------------------
# The same function, the other preference: the feed must sit on a routable
# GPIO through ONE 4-crosspoint path, DAC0 must not be written by rebuilds
# that don't move it (the driver's set-once shadow), every GPIO claimed must
# fall the feed through to DAC0, and freeing one must hop it back.

def mcp_writes(status):
    """[A,B,C,D] MCP4728 I2C write counts from the i@ 'mcp4728 writes' line."""
    m = re.search(r"mcp4728 writes (\d+)/(\d+)/(\d+)/(\d+)", status)
    return [int(x) for x in m.groups()] if m else None


def probe_line(status):
    for line in status.splitlines():
        if line.startswith("probe_power"):
            return line
    return ""


try:
    port1_command("`[dacs] probe_power_source = 1", collect_seconds=2)
    time.sleep(1.5)
    st = infra_status()
    pl = probe_line(st)
    check("order:GPIO>DAC0" in pl, "i@ reports order:GPIO>DAC0 after setting the flag")
    check("-> GPIO" in pl, f"feed is on the GPIO candidate under GPIO-first ({pl.strip()[:60]})")
    check("paths:1 dup:0 xp:4" in pl, "GPIO feed: one path, dup 0, 4 crosspoints (i@)")
    d = bridge_dump()
    src = feed_source(d)
    check(src is not None and src.startswith("GP_"), f"bridge array feed source is a GP_x ({src})")
    rows = feed_path_rows(d)
    check(len(rows) == 1, f"GPIO feed has exactly ONE routed path, got {len(rows)}")

    w = mcp_writes(st)
    check(w is not None, "i@ prints the mcp4728 write counters")
    jl_exec("connect(12, 17)\ndisconnect(12, -1)\nprint('ok')", timeout=25)
    time.sleep(1.5)
    st2 = infra_status()
    w2 = mcp_writes(st2)
    check(w is not None and w2 is not None and w2[0] == w[0],
          f"DAC0 (A) writes unchanged across a connect/disconnect ({w and w[0]} -> {w2 and w2[0]})")

    jl_exec("for p in range(20, 28):\n    gpio_claim_pin(p)\nprint('ok')", timeout=25)
    time.sleep(2.5)  # infraServiceTick (~500ms) notices the claim and nudges
    pl = probe_line(infra_status())
    check("-> DAC0" in pl and "xp:2" in pl,
          f"all 8 GPIO claimed from MicroPython -> feed fell through to DAC0, xp:2 ({pl.strip()[:70]})")

    jl_exec("gpio_release_pin(27)\nprint('ok')", timeout=25)
    time.sleep(2.5)  # the tick's GPIO-first switch-back
    pl = probe_line(infra_status())
    check("-> GPIO" in pl and "xp:4" in pl,
          f"freeing GPIO 8 -> feed hopped back to the GPIO candidate ({pl.strip()[:70]})")
finally:
    jl_exec("gpio_release_all_pins()\nprint('ok')", timeout=25)
    port1_command("`[dacs] probe_power_source = 0", collect_seconds=2)
    time.sleep(1.5)
    pl = probe_line(infra_status())
    check("order:DAC0>GPIO" in pl and "-> DAC0" in pl,
          f"flag restored to 0: order DAC0>GPIO and the feed is back on DAC0 ({pl.strip()[:60]})")

finish("test_infra_paths")
