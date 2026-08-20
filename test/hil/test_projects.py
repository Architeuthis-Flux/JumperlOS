#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Projects subsystem, first slice: the /projects/555/ reference project.

Provisioning (projectFiles[] + initializeProjects) and the Python
load_project() binding land in later tasks, so this test pushes
scripts/projects/555/* to the board itself and then exercises the SAME
parser path the relaxed FileManager guard uses: wiring.yaml's content
copied into /slots/slot3.yaml and loaded with '<3'.

Asserts:
  - the three project files land under /projects/555/ byte-identical
  - the wiring loads: ADC0-7 and ADC1-37 bridges exist, topRail is 5 V
  - the custom `nets:` name resolves - the net holding node 37 is "TIMING"
    (the net NUMBER is discovered, not hardcoded: it is topology-dependent)
  - parts are in the state but NOT expanded (no `placed:` in the file ->
    default false): rows 5/35/12/16 have no part bridges, while a wholesale
    toYAML rewrite still carries all six parts with `placed: false`
  - meta:/guide: are swallowed on parse and deliberately NOT round-tripped

Bench convention: snapshot board state + slot3.yaml + active slot up front,
restore all three at the end. The /projects/555/ files are left in place on
purpose - the clickwheel Files-browser check is a hands-on bench item.

NOTE the loaded wiring sets topRail to 5.0 V for the duration of the test;
the board_state snapshot restores the bench rail voltage afterwards.
"""

import os
import re
import time

from jl import (jl_exec, parse_kv, port1_command, check, finish,
                board_state_capture, board_state_restore)

SLOT_PATH = "/slots/slot3.yaml"
PROJ_DIR = "/projects/555"

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_DIR = os.path.join(REPO, "scripts", "projects", "555")
PROJECT_FILES = ("wiring.yaml", "main.py", "README.md")


def local_file(name):
    with open(os.path.join(SRC_DIR, name), "r") as f:
        return f.read()


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
""", timeout=25)
    if "EXISTS= 1" not in out:
        return False, ""
    m = re.search(r"<<<FILE>>>\r?\n(.*)<<<END>>>", out, re.DOTALL)
    return True, (m.group(1) if m else "")


WIRING = local_file("wiring.yaml")

# --- 0. Snapshot the bench: board state, slot3.yaml, active slot ----------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

q = port1_command("Q", 1.5)
m = re.search(r"ACTIVE_SLOT:(\d+)", q)
check(m is not None, "queried active slot ('Q')")
orig_slot = int(m.group(1)) if m else 0

slot3_existed, slot3_before = read_device_file(SLOT_PATH)
print(f"  info: active slot {orig_slot}, slot3.yaml existed: {slot3_existed}")

# Move off slot 3 before touching its file - the idle auto-save of the
# ACTIVE slot would clobber the fs_write.
if orig_slot == 3:
    port1_command("<2", 4.0)
    time.sleep(1.5)

# --- 1. Push the project tree to the board --------------------------------
out = jl_exec(f"""
for d in ("/projects", {PROJ_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({PROJ_DIR!r}) else 0)
""", timeout=25)
check(parse_kv(out).get("projdir") == 1, f"created {PROJ_DIR} on the board")

for name in PROJECT_FILES:
    content = local_file(name)
    path = f"{PROJ_DIR}/{name}"
    out = jl_exec(f"print('wrote=', 1 if fs_write({path!r}, {content!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, f"pushed {path}")
    exists, on_device = read_device_file(path)
    # fs_read round-trips through print(), which appends a newline; compare
    # on the stripped text so the trailing-newline difference isn't noise.
    check(exists and on_device.strip() == content.strip(),
          f"{path} content matches scripts/projects/555/{name}")

# --- 2. Load the wiring through the real slot-YAML parser ------------------
# Same content, same parser, same loadSlotFromPath-equivalent path that the
# relaxed FileManager guard now reaches for /projects/*.yaml.
out = jl_exec(f"print('wrote=', 1 if fs_write({SLOT_PATH!r}, {WIRING!r}) else 0)",
              timeout=30)
check(parse_kv(out).get("wrote") == 1, "copied wiring.yaml content to /slots/slot3.yaml")

resp = port1_command("<3", 4.0)
check("SLOT_CHANGED:3" in resp, "'<3' loaded the 555 wiring (SLOT_CHANGED:3 seen)")
time.sleep(2.0)  # let the load + refresh + reconcile settle

# --- 3. Bridges, rail, and the un-expanded parts ---------------------------
out = jl_exec("""
print("adc0=", 1 if is_connected("ADC0", 7) else 0)
print("adc1=", 1 if is_connected("ADC1", 37) else 0)
# parts are NOT placed (no `placed:` key -> default false), so none of the
# expansion bridges may exist:
print("u1gnd=", 1 if is_connected(5, "GND") else 0)
print("u1vcc=", 1 if is_connected(35, "TOP_RAIL") else 0)
print("r1a=", 1 if is_connected(12, "TOP_RAIL") else 0)
print("r2b=", 1 if is_connected(16, 37) else 0)
print("c1m=", 1 if is_connected(19, "GND") else 0)
print("nbridges=", get_num_bridges())
""")
vals = parse_kv(out)
check(vals.get("adc0") == 1, "bridge ADC0 <-> 7 exists after load")
check(vals.get("adc1") == 1, "bridge ADC1 <-> 37 exists after load")
check(vals.get("u1gnd") == 0, "part not expanded: U1 GND (row 5) has no GND bridge")
check(vals.get("u1vcc") == 0, "part not expanded: U1 VCC (row 35) has no TOP_RAIL bridge")
check(vals.get("r1a") == 0, "part not expanded: R1 A (row 12) has no TOP_RAIL bridge")
check(vals.get("r2b") == 0, "part not expanded: R2 B (row 16) has no 37 bridge")
check(vals.get("c1m") == 0, "part not expanded: C1 MINUS (row 19) has no GND bridge")

# --- 4. The custom nets: name --------------------------------------------
# Net numbers are topology-dependent (1-5 are the pre-created special nets:
# GND / Top Rail / Bottom Rail / DAC0 / DAC1), so DISCOVER the net holding
# node 37 rather than hardcoding it - and print it, because wiring.yaml's
# `nets:` entry has to name that number for deserializeNets to attach.
out = jl_exec("""
for i in range(1, 40):
    nodes = get_net_nodes(i)
    if not nodes:
        continue
    print("NET|%d|%s|%s" % (i, str(get_net_name(i)), str(nodes)))
""")
nets = {}
for line in out.splitlines():
    m = re.match(r"\s*NET\|(\d+)\|(.*)\|(.*?)\s*$", line)
    if m:
        nets[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())
for num in sorted(nets):
    print(f"  info: net {num}: {nets[num][0]!r} nodes {nets[num][1]}")

net37 = None
for num, (name, nodes) in nets.items():
    toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
    if "37" in toks:
        net37 = (num, name)
check(net37 is not None, "a net containing node 37 exists")
if net37:
    check(net37[1] == "TIMING",
          f"net holding node 37 resolves to the nets: name TIMING (net {net37[0]}, "
          f"name {net37[1]!r}; wiring.yaml declares num: 7)")
# The pre-created special nets (1=GND 2=Top Rail 3=Bottom Rail 4=DAC0 5=DAC1)
# must NOT have been renamed - naming net 1 is exactly the trap the reference
# YAML's original `num: 1` fell into.
for num, expected in ((1, "GND"), (2, "Top Rail")):
    if num in nets:
        check(nets[num][0] == expected,
              f"special net {num} still named {expected!r} (got {nets[num][0]!r})")

# --- 5. Parts survive a wholesale toYAML rewrite, still unplaced -----------
out = jl_exec("print('saved=', nodes_save(3))")
check(parse_kv(out).get("saved") == 3, "nodes_save(3) rewrote the slot via toYAML")
time.sleep(1.0)

_, rewritten = read_device_file(SLOT_PATH)
check("parts:" in rewritten, "parts: section survived the wholesale rewrite")
names = re.findall(r'- name: "([A-Za-z0-9_]+)"', rewritten)
check(names == ["U1", "R1", "R2", "C1", "LED1", "R3"],
      f"all six parts round-tripped in order (got {names})")
check(rewritten.count("placed: false") == 6 and "placed: true" not in rewritten,
      "every part is still placed: false after the rewrite")
for needle in ("footprint: dip8", "row: 5", 'value: "NE555"',
               "GND: {pin: 1, connect: GND, class: gnd}",
               "TRIG: {pin: 2, connect: 37, class: signal}",
               "CTRL: {pin: 5, class: nc}",
               "VCC: {pin: 8, connect: TOP_RAIL, class: power}",
               "footprint: sip2", 'value: "10k"', 'value: "47k"',
               'value: "10uF"', 'value: "330"', "type: led",
               "A: {pin: 1, connect: TOP_RAIL, class: signal}",
               "PLUS: {pin: 1, connect: 37, class: signal}",
               "MINUS: {pin: 2, connect: GND, class: signal}",
               "K: {pin: 2, connect: GND, class: signal}"):
    check(needle in rewritten, f"rewrite kept: {needle}")
# The inline one-line pins form (R1/R2/C1/LED1/R3 in wiring.yaml) parsed:
check("B: {pin: 2, connect: 36, class: signal}" in rewritten,
      "inline `pins: {A: {...}, B: {...}}` form parsed (R1 B -> node 36)")
check('name: "TIMING"' in rewritten, "the TIMING net name survived the rewrite")
check("topRail: 5.00" in rewritten, "power: topRail: 5.0 parsed and round-tripped")
# Documented as-built contract: meta:/guide: are swallowed, never re-emitted.
check("meta:" not in rewritten and "guide:" not in rewritten,
      "meta:/guide: were contained on parse and NOT round-tripped (as designed)")
check("guideProgress:" not in rewritten,
      "no guideProgress: emitted (guideSource empty - the guide runtime sets it)")

# --- 6. Restore the bench --------------------------------------------------
# Restore the FILE first, switch slots second (same hazard as phase 0).
if slot3_existed:
    out = jl_exec(f"print('restored=', 1 if fs_write({SLOT_PATH!r}, {slot3_before!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("restored") == 1, "restored slot3.yaml prior content")
else:
    out = jl_exec(f"""
if fs_exists({SLOT_PATH!r}):
    jfs.remove({SLOT_PATH!r})
print("removed=", 0 if fs_exists({SLOT_PATH!r}) else 1)
""")
    check(parse_kv(out).get("removed") == 1,
          "removed the test's slot3.yaml (did not exist before)")

port1_command(f"<{orig_slot}", 4.0)
time.sleep(1.5)

if snapshot is not None:
    check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

print("  info: /projects/555/ left on the board for the clickwheel bench check")
finish("test_projects")
