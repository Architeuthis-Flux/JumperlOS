#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parts layer roundtrip (file-driven - the place_part Python bindings land
in a later task): write a slot YAML with a parts: section, load it through
the REAL slot-load path (`<3` on port 1 -> SlotManager::loadSlot), assert the
expansion bridges and {NAME}_{PIN} net names, then trigger a wholesale toYAML
rewrite (nodes_save) and assert the parts: section SURVIVED it - the
auto-save regression that motivated the serializer. Unknown-key tolerance
(frobnicate: 7 inside a part) and unknown-section containment
(futuresection: before bridges:) ride along in the same file.

Bench convention: snapshot board state + slot3.yaml + active slot up front,
restore all three at the end."""

import re
import time

from jl import (jl_exec, parse_kv, port1_command, check, finish,
                board_state_capture, board_state_restore)

SLOT_PATH = "/slots/slot3.yaml"

# The slot under test. Covers: unknown top-level section with indented
# content BEFORE bridges (containment), a plain bridge with dup, a dip8 with
# pin numbers + connects incl. GND / TOP_RAIL / a row + an unknown key, a
# sip2 with offsets, and the guideProgress scalar.
SLOT_YAML = """version: 2
sourceOfTruth: bridges
guideProgress: {source: "/projects/test/wiring.yaml", step: 2}

futuresection:
  - {n1: 12, n2: 48}
  nested: {weird: 1}

bridges:
  - {n1: 55, n2: 42, dup: 2}

parts:
  - name: "U1"
    type: ic
    value: "NE555"
    footprint: dip8
    row: 5
    placed: true
    frobnicate: 7
    pins:
      GND: {pin: 1, connect: GND, class: gnd}
      TRIG: {pin: 2, connect: 37, class: signal}
      OUT: {pin: 3, class: signal}
      RESET: {pin: 4, connect: TOP_RAIL, class: power}
      CTRL: {pin: 5, class: nc}
      VCC: {pin: 8, connect: TOP_RAIL, class: power}
  - name: "R1"
    type: resistor
    value: "10k"
    footprint: sip2
    row: 20
    placed: true
    pins:
      A: {offset: 0, connect: 37}
      B: {offset: 1, connect: 45}
  - name: "U2"
    type: ic
    footprint: dip28
    row: 3
    placed: true
    pins:
      P1: {pin: 1, connect: 17}
      P28: {pin: 28, connect: 18}
  - name: "C1"
    type: capacitor
    value: "100n"
    footprint: sip2
    row: 50
    placed: false
    pins:
      A: {offset: 0, connect: 52}
      B: {offset: 1}

config:
  fakeGpio:
    - {slot: 8, node: 25, mode: 0, th_high: 2.00, th_low: 0.80}

power:
  topRail: 0.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00
"""

# Expansion geometry (dip8 at row 5: pins 1-4 -> 5,6,7,8; pins 5-8 ->
# 38,37,36,35; sip2 at row 20 with offsets 0/1 -> 20,21; dip28 at row 3:
# pin 1 -> 3, pin 28 -> 3+30+(28-28) = 33 - the physical pin count 28
# exceeds MAX_PART_PINS on purpose: only LISTED pins are storage-bounded):
#   U1 GND   pin 1  -> node 5  -> GND
#   U1 TRIG  pin 2  -> node 6  -> 37
#   U1 RESET pin 4  -> node 8  -> TOP_RAIL
#   U1 VCC   pin 8  -> node 35 -> TOP_RAIL
#   R1 A     off 0  -> node 20 -> 37
#   R1 B     off 1  -> node 21 -> 45
#   U2 P1    pin 1  -> node 3  -> 17
#   U2 P28   pin 28 -> node 33 -> 18
# C1 has placed: false - it must NOT expand (no 50 -> 52 bridge).
EXPECTED_PAIRS = [(5, "GND"), (6, 37), (8, "TOP_RAIL"), (35, "TOP_RAIL"),
                  (20, 37), (21, 45), (3, 17), (33, 18), (55, 42)]


def read_device_file(path):
    """Return (exists, content) for a device file via the REPL."""
    out = jl_exec(f"""
p = {path!r}
if fs_exists(p):
    print("EXISTS= 1")
    print("<<<FILE>>>")
    print(fs_read(p))
    print("<<<END>>>")
else:
    print("EXISTS= 0")
""", timeout=20)
    if "EXISTS= 1" not in out:
        return False, ""
    m = re.search(r"<<<FILE>>>\r?\n(.*)<<<END>>>", out, re.DOTALL)
    return True, (m.group(1) if m else "")


# --- 0. Snapshot the bench: board state, slot3.yaml, active slot ----------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

q = port1_command("Q", 1.5)
m = re.search(r"ACTIVE_SLOT:(\d+)", q)
check(m is not None, "queried active slot ('Q')")
orig_slot = int(m.group(1)) if m else 0

slot3_existed, slot3_before = read_device_file(SLOT_PATH)
print(f"  info: active slot {orig_slot}, slot3.yaml existed: {slot3_existed}")

# If slot 3 is somehow the active slot, move off it first - the idle
# auto-save of the ACTIVE slot would clobber our fs_write.
if orig_slot == 3:
    port1_command("<2", 4.0)
    time.sleep(1.5)

# --- 1. Write the parts slot and load it through the real path ------------
out = jl_exec(f"print('wrote=', 1 if fs_write({SLOT_PATH!r}, {SLOT_YAML!r}) else 0)")
check(parse_kv(out).get("wrote") == 1, "wrote parts slot YAML to /slots/slot3.yaml")

resp = port1_command("<3", 4.0)
check("SLOT_CHANGED:3" in resp, "'<3' switched to slot 3 (SLOT_CHANGED:3 seen)")
time.sleep(2.0)  # let the load + refresh + reconcile settle

# --- 2. Expansion bridges + containment ------------------------------------
pairs_py = "[" + ", ".join(f"({a!r}, {b!r})" for a, b in EXPECTED_PAIRS) + "]"
out = jl_exec(f"""
pairs = {pairs_py}
for i, (a, b) in enumerate(pairs):
    print("conn%d=" % i, 1 if is_connected(a, b) else 0)
print("bogus=", 1 if is_connected(12, 48) else 0)
print("nopin=", 1 if is_connected(7, 36) else 0)
print("unplaced=", 1 if is_connected(50, 52) else 0)
""")
vals = parse_kv(out)
for i, (a, b) in enumerate(EXPECTED_PAIRS):
    check(vals.get(f"conn{i}") == 1, f"bridge {a} <-> {b} exists after load")
check(vals.get("bogus") == 0,
      "futuresection: content was contained (12-48 did NOT become a bridge)")
check(vals.get("nopin") == 0, "pins without connect: made no bridge (7-36 absent)")
check(vals.get("unplaced") == 0,
      "placed: false part C1 was NOT expanded (50-52 absent)")

# --- 3. Net names ----------------------------------------------------------
out = jl_exec("""
found = {}
for i in range(1, 59):
    nm = get_net_name(i)
    if nm:
        s = str(nm)
        if "_" in s:
            found[s] = str(get_net_nodes(i))
for k in sorted(found):
    print(k, "=", found[k])
""")
check("U1_TRIG" in out, "net name U1_TRIG asserted from the parts table")
check("R1_B" in out, "net name R1_B asserted (offset pins)")
trig_nodes = ""
for line in out.splitlines():
    if line.startswith("U1_TRIG"):
        trig_nodes = line
check(all(str(n) in re.findall(r"\d+", trig_nodes) for n in (6, 37, 20)),
      f"U1_TRIG net contains nodes 6, 37, 20 ({trig_nodes.strip()})")
# Collision rule: R1 pin A shares the 6/37/20 net; lowest part index (U1) won.
check("R1_A" not in out, "collision rule: U1_TRIG outranks R1_A on the shared net")
# GND / TOP_RAIL are special-function nets - never renamed by parts.
check("U1_GND" not in out and "U1_RESET" not in out and "U1_VCC" not in out,
      "power/ground nets keep their names (special nets not auto-renamed)")

# --- 4. The auto-save regression: wholesale rewrite must keep parts: ------
out = jl_exec("print('saved=', nodes_save(3))")
check(parse_kv(out).get("saved") == 3, "nodes_save(3) rewrote the slot via toYAML")
time.sleep(1.0)

_, rewritten = read_device_file(SLOT_PATH)
check("parts:" in rewritten, "parts: section SURVIVED the wholesale rewrite")
for needle in ('- name: "U1"', "type: ic", 'value: "NE555"', "footprint: dip8",
               "row: 5", "placed: true",
               "GND: {pin: 1, connect: GND, class: gnd}",
               "TRIG: {pin: 2, connect: 37, class: signal}",
               "OUT: {pin: 3, class: signal}",
               "RESET: {pin: 4, connect: TOP_RAIL, class: power}",
               "CTRL: {pin: 5, class: nc}",
               "VCC: {pin: 8, connect: TOP_RAIL, class: power}",
               '- name: "R1"', "type: resistor", 'value: "10k"',
               "footprint: sip2", "row: 20",
               "A: {offset: 0, connect: 37, class: signal}",
               "B: {offset: 1, connect: 45, class: signal}",
               '- name: "U2"', "footprint: dip28",
               "P1: {pin: 1, connect: 17, class: signal}",
               "P28: {pin: 28, connect: 18, class: signal}",
               '- name: "C1"', "placed: false",
               "A: {offset: 0, connect: 52, class: signal}",
               "B: {offset: 1, class: signal}"):
    check(needle in rewritten, f"rewrite kept: {needle}")
check(re.search(r'guideProgress: \{source: "/projects/test/wiring.yaml", step: 2\}',
                rewritten) is not None, "guideProgress scalar survived the rewrite")
check(re.search(r"- \{n1: 55, n2: 42, dup: 2", rewritten) is not None,
      "plain bridge 55-42 (dup 2) survived the rewrite")
check("frobnicate" not in rewritten,
      "unknown part key was tolerated on parse (and not resurrected)")
# The serializer nests fakeGpio under config ("  fakeGpio:") - the parser's
# indent-hardening explicitly exempts it. Guard the exemption.
check(re.search(r"fakeGpio:\s*\n\s*- \{slot: 8, node: 25, mode: 0", rewritten) is not None,
      "config-nested fakeGpio entry survived indent-hardening + rewrite")

# --- 5. parse(serialize(state)): reload the REWRITTEN file ----------------
bounce = orig_slot if orig_slot != 3 else 2
port1_command(f"<{bounce}", 4.0)
time.sleep(1.5)
port1_command("<3", 4.0)
time.sleep(2.0)
out = jl_exec("""
print("conn_trig=", 1 if is_connected(6, 37) else 0)
print("conn_r1b=", 1 if is_connected(21, 45) else 0)
names = []
for i in range(1, 59):
    nm = get_net_name(i)
    if nm and "_" in str(nm):
        names.append(str(nm))
print("names=", ",".join(sorted(names)))
""")
vals = parse_kv(out)
check(vals.get("conn_trig") == 1, "second load of the rewritten file: 6-37 bridge intact")
check(vals.get("conn_r1b") == 1, "second load of the rewritten file: 21-45 bridge intact")
check("U1_TRIG" in str(vals.get("names", "")), "second load: U1_TRIG re-asserted")

# --- 6. numParts == 0 emits NO parts: section ------------------------------
# Write a plain (no-parts) slot, load it, force a rewrite, and assert the
# serializer omits parts: and guideProgress: entirely for an empty table.
NO_PARTS_YAML = """version: 2
sourceOfTruth: bridges

bridges:
  - {n1: 55, n2: 42, dup: 2}

power:
  topRail: 0.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00
"""
port1_command(f"<{bounce}", 4.0)   # move off slot 3 before touching its file
time.sleep(1.5)
out = jl_exec(f"print('wrote2=', 1 if fs_write({SLOT_PATH!r}, {NO_PARTS_YAML!r}) else 0)")
check(parse_kv(out).get("wrote2") == 1, "wrote no-parts slot YAML")
port1_command("<3", 4.0)
time.sleep(2.0)
out = jl_exec("print('saved2=', nodes_save(3))")
check(parse_kv(out).get("saved2") == 3, "nodes_save(3) rewrote the no-parts slot")
time.sleep(1.0)
_, noparts = read_device_file(SLOT_PATH)
check("parts:" not in noparts, "numParts==0: rewrite emits NO parts: section")
check("guideProgress:" not in noparts, "empty guideSource: rewrite emits NO guideProgress:")
check("- {n1: 55, n2: 42" in noparts, "no-parts slot still round-trips its bridge")

# --- 7. Restore the bench --------------------------------------------------
# Restore the FILE first, switch slots second: if orig_slot happened to be 3,
# switching first would make slot 3 active again and the idle auto-save could
# clobber the restored content (the same hazard handled before phase 1).
if slot3_existed:
    out = jl_exec(f"print('restored=', 1 if fs_write({SLOT_PATH!r}, {slot3_before!r}) else 0)")
    check(parse_kv(out).get("restored") == 1, "restored slot3.yaml prior content")
else:
    out = jl_exec(f"""
if fs_exists({SLOT_PATH!r}):
    jfs.remove({SLOT_PATH!r})
print("removed=", 0 if fs_exists({SLOT_PATH!r}) else 1)
""")
    check(parse_kv(out).get("removed") == 1, "removed the test's slot3.yaml (did not exist before)")

port1_command(f"<{orig_slot}", 4.0)
time.sleep(1.5)

if snapshot is not None:
    check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

finish("test_parts_roundtrip")
