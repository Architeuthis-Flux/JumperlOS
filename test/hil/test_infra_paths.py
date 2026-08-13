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
claim_node = src0 if src0 and src0.startswith("GP_") else "GPIO_8"
claim_py = claim_node.replace("GP_", "GPIO_")
out = jl_exec(f"connect({claim_py}, 25)\nprint('ok')", timeout=25)
time.sleep(1.5)
d = bridge_dump()
src1 = feed_source(d)
check(src1 is not None and src1 != src0,
      f"feed RELOCATED after claiming {claim_node} ({src0} -> {src1})")

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
f = jfs.open("/slots/slot0.yaml", "r")
data = ""
while True:
    chunk = jfs.read(f, 512)
    if not chunk:
        break
    data += chunk
jfs.close(f)
print("has_bufin=", 1 if ("BUF_IN" in data or "ROUTABLE_BUFFER" in data) else 0)
print("has_user_net=", 1 if "12, 17" in data or "12,17" in data else 0)
disconnect(12, -1)
print("done")
""", timeout=30)
check("has_bufin= 0" in out, "fresh slot save contains no BUF_IN (bridges or nets section)")
check("has_user_net= 1" in out, "fresh slot save DOES contain the user net (save actually ran)")

# --- 6. user bridge to BUF_IN is rejected (system-reserved) -----------------
out = jl_exec("""
ok = connect(139, 30)
print("connect_rc=", ok)
print("bk=", 1 if is_connected(139, 30) else 0)
""", timeout=25)
check("bk= 0" in out, "user bridge to ROUTABLE_BUFFER_IN is rejected (reserved node)")

# --- 7. RouteSafety self-check still passes with the infra feed live --------
out = port1_command("i?", collect_seconds=6)
check("PASS" in out or "pass" in out, "RouteSafety self-check PASS with infra feed routed")

finish("test_infra_paths")
