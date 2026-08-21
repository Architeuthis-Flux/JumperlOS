#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parts layer roundtrip, file-driven AND through the Python bindings.

File-driven (phases 1-6): write a slot YAML with a parts: section, load it
through the REAL slot-load path (`<3` on port 1 -> SlotManager::loadSlot),
assert the expansion bridges and {NAME}_{PIN} net names, then trigger a
wholesale toYAML rewrite (nodes_save) and assert the parts: section SURVIVED
it - the auto-save regression that motivated the serializer. Unknown-key
tolerance (frobnicate: 7 inside a part) and unknown-section containment
(futuresection: before bridges:) ride along in the same file.

API (phases 7-8, task 4's bindings): place_part / list_parts / remove_part /
guide_progress against an empty parts table, then load_project() of a project
pushed to /projects/tmptest/wiring.yaml - including the slot-clobber
regression (the active slot number and /slots/slot0.yaml must be untouched by
a project load, because a project wiring.yaml is deliberately NOT named
slot*.yaml).

Bench convention: snapshot board state + slot3.yaml + active slot up front,
restore all three in a finally (an uncaught exception must not strand the
bench - the same lesson test_guide_flow.py's header records)."""

import re
import time

from jl import (jl_exec, parse_kv, port1_command, check, finish,
                board_state_capture, board_state_restore)

SLOT_PATH = "/slots/slot3.yaml"

# Phase 8's throwaway project. Declared up here (not at its phase) because
# the finally that cleans it up must be able to name it even when the run
# never reached phase 8.
TMP_PROJ = "/projects/tmptest"
TMP_WIRING = TMP_PROJ + "/wiring.yaml"

# The slot under test. Covers: unknown top-level section with indented
# content BEFORE bridges (containment), a plain bridge with dup, a dip8 with
# pin numbers + connects incl. GND / TOP_RAIL / a row + an unknown key, a
# sip2 with offsets, and the guideProgress scalar.
#
# C1's value is the string "overlays:" ON PURPOSE - it is the section-hijack
# needle. deserializeOverlaysFromYAML used to strstr() the WHOLE file for
# "overlays:" and start parsing at the first hit, so this value (which sits
# ABOVE the real section) took over the scan - and since the parts section
# is full of `- name:` lines, the parts would have been read as overlays.
# The scanner now matches only an UN-INDENTED `overlays:` line, so the real
# RTTEST overlay below must survive intact.
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
    value: "overlays:"
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

overlays:
  - name: "RTTEST"
    row: 2
    col: 3
    width: 2
    height: 2
    colors:
           [0x110000, 0x001100, 0x000011, 0x111111]
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

# The "somewhere that isn't slot 3" the phases below bounce through to force
# a re-read. Computed here, with orig_slot, so the finally can use it too.
bounce = orig_slot if orig_slot != 3 else 2

# If slot 3 is somehow the active slot, move off it first - the idle
# auto-save of the ACTIVE slot would clobber our fs_write.
if orig_slot == 3:
    port1_command("<2", 4.0)
    time.sleep(1.5)

# Everything that MUTATES the board lives inside this try; the tmptest
# teardown + phase 9's restore are its finally. Without it an uncaught
# exception anywhere below walks out with slot 3 overwritten, /projects/
# tmptest still on the flash and the active slot moved - a stranded bench
# that reads as a firmware bug the next time anyone looks. Same shape
# test_guide_flow.py and test_projects.py use. Phase 0 stays OUTSIDE: the
# finally needs snapshot/orig_slot/slot3_before/bounce bound first.
try:
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

    # --- 2b. The overlays: section survived a parts value that says
    # "overlays:" -------------------------------------------------------------
    # Section-hijack regression (see the SLOT_YAML note): a bare
    # strstr(file, "overlays:") started the overlay scan inside the PARTS
    # section, where it would have read `- name: "U1"` etc. as overlay
    # entries - wrong count, wrong names, and the real overlay lost. The
    # scanner now anchors on an un-indented `overlays:` line, so exactly one
    # overlay must be present and it must be RTTEST with its own geometry.
    out = jl_exec("""
print("ovcount=", overlay_count())
print("<<<OV>>>")
print(overlay_serialize())
print("<<<END>>>")
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("ovcount") == 1,
          f"exactly one overlay loaded (got {vals.get('ovcount')}) - the parts "
          f"section did not hijack the overlays scan")
    ov = re.search(r"<<<OV>>>\r?\n(.*)<<<END>>>", out, re.DOTALL)
    ovtext = ov.group(1) if ov else ""
    check("RTTEST" in ovtext,
          f"the loaded overlay is RTTEST, not a part name (got {ovtext[:200]!r})")
    check(not re.search(r'"(U1|U2|R1|C1)"', ovtext),
          "no part name leaked into the overlay table")

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
    gp = jl_exec("print('gp=', guide_progress())")
    check(parse_kv(gp).get("gp") == 2,
          "guide_progress() == 2 (the guideProgress: scalar this slot carries)")

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
                   "B: {offset: 1, class: signal}",
                   # ...and the overlays: section beside it, so the hijack
                   # needle (C1's value) has to survive a rewrite too.
                   "\noverlays:\n", '- name: "RTTEST"'):
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

    # --- 7. The Python bindings: place_part / list_parts / remove_part ---------
    # Slot 3 is active and its parts table is empty (phase 6 loaded the no-parts
    # YAML), so every part seen from here on came from the API. Geometry:
    #   U9 sip2 @ row 5 (footprint inferred): A pin 1 -> node 5, B pin 2 -> node 6
    #   U8 dip8 @ row 14 (explicit):          VCC pin 8 -> 14+30+(8-8) = node 44
    out = jl_exec("""
print("gp_none=", guide_progress())
print("rc=", place_part("U9", 5, '{"A": {"pin": 1, "connect": "GND"}, "B": {"pin": 2, "connect": 7}}'))
print("rc_dip=", place_part("U8", 14, '{"VCC": {"pin": 8, "connect": "TOP_RAIL"}}', "dip8", "ic", "TEST"))
print("rc_dup=", place_part("U9", 20, '{"A": {"pin": 1, "connect": 30}}'))
print("rc_odd=", place_part("BAD1", 20, '{"A": {"pin": 1, "connect": 30}}', "dip7"))
print("rc_row=", place_part("BAD2", 99, '{"A": {"pin": 1, "connect": 30}}'))
print("rc_nopins=", place_part("BAD3", 20, '{}'))
print("rc_quote=", place_part('U"X', 30, '{"A": {"pin": 1, "connect": 31}}'))
print("rc_nl=", place_part("U\\nX", 30, '{"A": {"pin": 1, "connect": 31}}'))
print("rc_pinnl=", place_part("UNL", 30, '{"A\\nB": {"pin": 1, "connect": 31}}'))
print("rc_val=", place_part("UVAL", 30, '{"A": {"pin": 1, "connect": 31}}', "sip2", "ic", 'bad\\nvalue'))
print("rc_pindash=", place_part("UDASH", 30, '{"- X": {"pin": 1, "connect": 31}}'))
print("rc_pinhash=", place_part("UHASH", 30, '{"#X": {"pin": 1, "connect": 31}}'))
print("names_after=", ",".join(sorted(p["name"] for p in list_parts())))
""", timeout=40)
    vals = parse_kv(out)
    check(vals.get("gp_none") == -1, "guide_progress() == -1 when no guide source is loaded")
    check(vals.get("rc") == 0, "place_part('U9', 5, pins_json) returned 0")
    check(vals.get("rc_dip") == 0, "place_part with an explicit dip8 + type + value returned 0")
    check(vals.get("rc_dup") == -1, "place_part on an existing name is refused (-1)")
    # The three refusals below are commitPart()'s predicate: an entry that passed
    # here but failed the parser would be auto-saved and then silently DROPPED on
    # the next load (the erasure bug of commit 352bb23).
    check(vals.get("rc_odd") == -1, "place_part with an odd DIP pin count is refused (-1)")
    check(vals.get("rc_row") == -1, "place_part with row 99 is refused (-1)")
    check(vals.get("rc_nopins") == -1, "place_part with no parseable pins is refused (-1)")
    # Charset guard: serializeParts emits name/value quoted and type/pin names
    # bare, so a '"' truncates the field at reload and a newline writes an
    # UN-INDENTED line into parts: - where deserializeParts breaks out and drops
    # every part after it. All four must be refused, not sanitized.
    check(vals.get("rc_quote") == -1, "place_part with a '\"' in the name is refused (-1)")
    check(vals.get("rc_nl") == -1, "place_part with a newline in the name is refused (-1)")
    check(vals.get("rc_pinnl") == -1, "place_part with a newline in a PIN name is refused (-1)")
    check(vals.get("rc_val") == -1, "place_part with a newline in the value is refused (-1)")
    # Leading-marker guard on PIN names, same data-corruption class one
    # character away: pin names serialize UNQUOTED as `      <NAME>: {...}`,
    # so "- X" reloads as a parts-LIST entry (deserializeParts' startsWith
    # "- " test) and every following pin is misattributed to a phantom part;
    # "#X" reloads as a comment and the pin vanishes.
    check(vals.get("rc_pindash") == -1,
          "place_part with a PIN name starting '- ' is refused (-1)")
    check(vals.get("rc_pinhash") == -1,
          "place_part with a PIN name starting '#' is refused (-1)")
    # ...and nothing partial landed: only the two parts that were accepted.
    check(vals.get("names_after") == "U8,U9",
          f"every refused place_part left NO entry behind (table holds "
          f"{vals.get('names_after')!r}, expected 'U8,U9')")

    out = jl_exec("""
print("u9a=", 1 if is_connected(5, "GND") else 0)
print("u9b=", 1 if is_connected(6, 7) else 0)
print("u8vcc=", 1 if is_connected(44, "TOP_RAIL") else 0)
print("bad=", 1 if is_connected(20, 30) else 0)
print("badchar=", 1 if is_connected(30, 31) else 0)
for i in range(1, 59):
    nodes = get_net_nodes(i)
    if nodes:
        print("NET|%d|%s|%s" % (i, str(get_net_name(i)), str(nodes)))
""")
    vals = parse_kv(out)
    check(vals.get("u9a") == 1, "place_part expanded U9 pin A: bridge 5 <-> GND")
    check(vals.get("u9b") == 1, "place_part expanded U9 pin B: bridge 6 <-> 7")
    check(vals.get("u8vcc") == 1, "place_part expanded U8 pin VCC (dip8 pin 8 -> node 44) to TOP_RAIL")
    check(vals.get("bad") == 0, "no bridge from any refused place_part call (20-30 absent)")
    check(vals.get("badchar") == 0,
          "no bridge from any charset-refused place_part call (30-31 absent)")

    nets = {}
    for line in out.splitlines():
        m = re.match(r"\s*NET\|(\d+)\|(.*)\|(.*?)\s*$", line)
        if m:
            nets[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())
    net7 = None
    gndnet = None
    for num, (name, nodes) in nets.items():
        toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
        if "7" in toks:
            net7 = (num, name)
        if "5" in toks and name == "GND":
            gndnet = num
    check(net7 is not None and net7[1] == "U9_B",
          f"the net holding node 7 is named U9_B (got {net7})")
    check(gndnet is not None,
          "U9 pin A landed on GND and the special net kept its name (never renamed by parts)")

    out = jl_exec("""
parts = list_parts()
print("n=", len(parts))
by = {}
for p in parts:
    by[p['name']] = p
u9 = by['U9']
print("u9row=", u9['row'])
print("u9fp=", u9['footprint'])
print("u9placed=", 1 if u9['placed'] else 0)
print("u9npins=", len(u9['pins']))
print("u9bnode=", u9['pins']['B']['node'])
print("u9bconn=", u9['pins']['B']['connect'])
print("u9aclass=", u9['pins']['A']['class'])
u8 = by['U8']
print("u8fp=", u8['footprint'])
print("u8type=", u8['type'])
print("u8val=", u8['value'])
print("u8vnode=", u8['pins']['VCC']['node'])
""")
    vals = parse_kv(out)
    check(vals.get("n") == 2,
          "list_parts() returned both parts and nothing from the 7 refused calls")
    check(vals.get("u9row") == 5, "list_parts: U9 row is 5")
    check(vals.get("u9fp") == "sip2", "list_parts: U9 footprint inferred as sip2")
    check(vals.get("u9placed") == 1, "list_parts: U9 placed is True")
    check(vals.get("u9npins") == 2, "list_parts: U9 pins dict has both legs")
    check(vals.get("u9bnode") == 6, "list_parts: U9 pin B resolves to node 6")
    check(vals.get("u9bconn") == 7, "list_parts: U9 pin B connects to 7")
    check(vals.get("u9aclass") == "signal", "list_parts: U9 pin A class defaults to signal")
    check(vals.get("u8fp") == "dip8", "list_parts: U8 footprint is dip8")
    check(vals.get("u8type") == "ic", "list_parts: U8 type is ic")
    check(vals.get("u8val") == "TEST", "list_parts: U8 value is TEST")
    check(vals.get("u8vnode") == 44, "list_parts: U8 pin VCC resolves to node 44")

    # The silent-vanish check: a part BUILT by place_part must survive the
    # serializer AND re-parse on the next load (place_part enforces the same
    # predicate commitPart does, so nothing it accepts can be dropped).
    out = jl_exec("print('saved=', nodes_save(3))")
    check(parse_kv(out).get("saved") == 3, "nodes_save(3) wrote the API-placed parts")
    time.sleep(1.0)
    _, api_yaml = read_device_file(SLOT_PATH)
    for needle in ('- name: "U9"', "footprint: sip2", "row: 5", "placed: true",
                   "A: {pin: 1, connect: GND, class: signal}",
                   "B: {pin: 2, connect: 7, class: signal}",
                   '- name: "U8"', "footprint: dip8", 'value: "TEST"', "type: ic",
                   "VCC: {pin: 8, connect: TOP_RAIL, class: signal}"):
        check(needle in api_yaml, f"place_part serialized: {needle}")

    port1_command(f"<{bounce}", 4.0)
    time.sleep(1.5)
    port1_command("<3", 4.0)
    time.sleep(2.0)
    out = jl_exec("""
print("n=", len(list_parts()))
print("u9b=", 1 if is_connected(6, 7) else 0)
print("u8vcc=", 1 if is_connected(44, "TOP_RAIL") else 0)
""")
    vals = parse_kv(out)
    check(vals.get("n") == 2, "reload: both API-placed parts re-parsed (none silently dropped)")
    check(vals.get("u9b") == 1, "reload: U9 pin B bridge 6-7 re-expanded")
    check(vals.get("u8vcc") == 1, "reload: U8 pin VCC bridge 44-TOP_RAIL re-expanded")

    out = jl_exec("""
print("rm=", remove_part("U9"))
print("rm2=", remove_part("U8"))
print("rm_missing=", remove_part("U9"))
print("n=", len(list_parts()))
print("u9a=", 1 if is_connected(5, "GND") else 0)
print("u9b=", 1 if is_connected(6, 7) else 0)
print("u8vcc=", 1 if is_connected(44, "TOP_RAIL") else 0)
names = []
for i in range(1, 59):
    nm = get_net_name(i)
    if nm and str(nm).startswith("U9_"):
        names.append(str(nm))
print("leftover=", ",".join(names) if names else "none")
""", timeout=40)
    vals = parse_kv(out)
    check(vals.get("rm") == 0, "remove_part('U9') returned 0")
    check(vals.get("rm2") == 0, "remove_part('U8') returned 0")
    check(vals.get("rm_missing") == -1, "remove_part on an unknown name returns -1")
    check(vals.get("n") == 0, "remove_part dropped the entries (list_parts() empty)")
    check(vals.get("u9a") == 0, "remove_part pulled the 5 <-> GND bridge")
    check(vals.get("u9b") == 0, "remove_part pulled the 6 <-> 7 bridge")
    check(vals.get("u8vcc") == 0, "remove_part pulled the 44 <-> TOP_RAIL bridge")
    check(vals.get("leftover") == "none", "remove_part cleared the {NAME}_{PIN} net names too")

    # --- 8. load_project() + the slot-clobber regression -----------------------
    # A project wiring.yaml is deliberately NOT named slot*.yaml, so
    # extractSlotNumberFromPath() returns -1 and loadSlotFromPath leaves slot
    # tracking alone. Assert that: the active slot number and /slots/slot0.yaml
    # must both come through a project load untouched.
    TMP_YAML = """version: 2
sourceOfTruth: bridges
guideProgress: {source: "/projects/tmptest/wiring.yaml", step: 4}

meta:
  name: tmptest

bridges:
  - {n1: 23, n2: 24}

parts:
  - name: "Q1"
    type: bjt
    footprint: sip3
    row: 28
    placed: true
    pins:
      B: {offset: 0, connect: 23}
      C: {offset: 1}
      E: {offset: 2, connect: 26}

power:
  topRail: 0.00
  bottomRail: 0.00
  dac0: 3.33
  dac1: 0.00
"""

    slot0_existed, slot0_before = read_device_file("/slots/slot0.yaml")
    print(f"  info: slot0.yaml existed before load_project: {slot0_existed}")

    out = jl_exec(f"""
for d in ("/projects", {TMP_PROJ!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({TMP_PROJ!r}) else 0)
print("wrote=", 1 if fs_write({TMP_WIRING!r}, {TMP_YAML!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("projdir") == 1, f"created {TMP_PROJ} on the board")
    check(vals.get("wrote") == 1, f"pushed {TMP_WIRING}")

    out = jl_exec("""
print("loaded=", 1 if load_project("tmptest") else 0)
""", timeout=30)
    check(parse_kv(out).get("loaded") == 1,
          "load_project('tmptest') resolved /projects/tmptest/wiring.yaml and loaded it")
    time.sleep(1.5)

    out = jl_exec("""
print("b=", 1 if is_connected(23, 24) else 0)
print("q1b=", 1 if is_connected(28, 23) else 0)
print("q1e=", 1 if is_connected(30, 26) else 0)
print("gp=", guide_progress())
print("nparts=", len(list_parts()))
print("stale=", 1 if is_connected(55, 42) else 0)
""")
    vals = parse_kv(out)
    check(vals.get("b") == 1, "load_project applied the project's bridges (23-24)")
    check(vals.get("q1b") == 1, "load_project re-expanded the placed part (Q1 B: node 28 -> 23)")
    check(vals.get("q1e") == 1, "load_project re-expanded the placed part (Q1 E: node 30 -> 26)")
    check(vals.get("gp") == 4, "guide_progress() == 4 from the project's guideProgress scalar")
    check(vals.get("nparts") == 1, "list_parts() sees the project's part")
    check(vals.get("stale") == 0,
          "load_project replaced the previous state (slot 3's 55-42 bridge is gone)")

    q = port1_command("Q", 1.5)
    m = re.search(r"ACTIVE_SLOT:(\d+)", q)
    check(m is not None and int(m.group(1)) == 3,
          f"slot-clobber regression: the active slot is still 3 after load_project ({q.strip()[:60]!r})")
    time.sleep(2.0)  # give the idle auto-save a chance to misbehave
    slot0_after_exists, slot0_after = read_device_file("/slots/slot0.yaml")
    check(slot0_after_exists == slot0_existed and slot0_after == slot0_before,
          "slot-clobber regression: /slots/slot0.yaml is byte-identical after load_project")

    out = jl_exec(f"""
print("literal=", 1 if load_project({TMP_WIRING!r}) else 0)
print("missing=", 1 if load_project("nosuchproject") else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("literal") == 1, "load_project() takes a literal path when the arg contains '/'")
    check(vals.get("missing") == 0, "load_project() of a missing project returns False")

finally:
    # --- 8b/9. Teardown: tmptest off the flash, then the bench back ----------
    # Reached on the happy path AND on any exception above. Every step is
    # written to be safe when the phase that created its subject never ran
    # (removing a project that was never written is a no-op that still
    # reports gone=1).
    out = jl_exec(f"""
for p in ({TMP_WIRING!r},):
    if fs_exists(p):
        jfs.remove(p)
try:
    jfs.rmdir({TMP_PROJ!r})
except Exception as e:
    print("rmdirerr=", e)
print("gone=", 0 if fs_exists({TMP_PROJ!r}) else 1)
""", timeout=25)
    check(parse_kv(out).get("gone") == 1, f"removed {TMP_PROJ} (the 555 project stays, tmptest does not)")

    # Leave slot 3 before the restore: the state is dirty from the project load
    # and the idle auto-save of the ACTIVE slot would clobber the fs_write below.
    port1_command(f"<{bounce}", 4.0)
    time.sleep(1.5)

    # --- 9. Restore the bench --------------------------------------------------
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
