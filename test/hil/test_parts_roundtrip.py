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
                board_state_capture, board_state_restore,
                active_context, restore_context, project_run_mode)

SLOT_PATH = "/slots/slot3.yaml"

# Phase 8's throwaway project. Declared up here (not at its phase) because
# the finally that cleans it up must be able to name it even when the run
# never reached phase 8.
TMP_PROJ = "/projects/tmptest"
TMP_WIRING = TMP_PROJ + "/wiring.yaml"

# The slot under test. Covers: unknown top-level section with indented
# content BEFORE bridges (containment), a plain bridge with dup, a dip8 with
# pin numbers + connects incl. GND / TOP_RAIL / a row + an unknown key, a
# sip2 with offsets, C1's `placement: compact` (the PARSE half of the
# placement byte - it is placed: false, so it round-trips as pure data), and
# the guideProgress scalar.
#
# U1's value is the string "overlays:" ON PURPOSE - it is the section-hijack
# needle. deserializeOverlaysFromYAML used to strstr() the WHOLE file for
# "overlays:" and begin parsing at the first hit, then scan FORWARD for
# `- name:` entries. The old scanner therefore reads this file as four
# overlays - R1, U2, C1 and RTTEST - because the three parts BELOW U1 each
# present a `- name:` line inside the hijacked region.
#
# It must be the FIRST part, not the last. On C1 the needle is INERT: the
# forward scan starts below C1's own header, so the first `- name:` it finds
# is the real RTTEST overlay and old and new scanners agree. Both scanners
# were ported to Python and run against both placements to confirm that
# (old: ['R1','U2','C1','RTTEST'] here vs ['RTTEST'] on C1). If this value
# ever migrates to a later part, phase 2b stops testing anything.
#
# The scanner now anchors on an UN-INDENTED `overlays:` line and stops at the
# next un-indented header, so only the real RTTEST overlay may load.
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
    value: "overlays:"
    footprint: dip8
    row: 35
    placed: true
    frobnicate: 7
    pins:
      GND: {pin: 1, connect: GND, class: gnd}
      TRIG: {pin: 2, connect: 44, class: signal}
      OUT: {pin: 3, class: signal}
      RESET: {pin: 4, connect: TOP_RAIL, class: power}
      CTRL: {pin: 5, class: nc}
      VCC: {pin: 8, connect: TOP_RAIL, class: power}
  - name: "R1"
    type: resistor
    value: "10k"
    tol: 5
    footprint: sip2
    row: 20
    placed: true
    pins:
      A: {offset: 0, connect: 44}
      B: {offset: 1, connect: 45}
  - name: "U2"
    type: ic
    footprint: dip28
    row: 33
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
    placement: compact
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

# Expansion geometry (wave 2 binding geometry - DIP pin 1's row is the bottom
# half, 31-60; dip8 at row 35: pins 1-4 -> 35,36,37,38 (bottom, left->right);
# pins 5-8 -> 8,7,6,5 (top, right->left); sip2 at row 20 with offsets 0/1 ->
# 20,21; dip28 at row 33: pin 1 -> 33, pin 28 -> (33-30)+(28-28) = 3 - the
# physical pin count 28 exceeds MAX_PART_PINS on purpose: only LISTED pins
# are storage-bounded):
#   U1 GND   pin 1  -> node 35 -> GND
#   U1 TRIG  pin 2  -> node 36 -> 44   (44 is a fresh row, NOT U1's own OUT
#                                        hole (37) - TRIG's own leg AND R1's
#                                        A leg both bridge to it, so they
#                                        still land on the same net for the
#                                        collision-rule check below without
#                                        also shorting to OUT)
#   U1 RESET pin 4  -> node 38 -> TOP_RAIL
#   U1 VCC   pin 8  -> node 5  -> TOP_RAIL
#   R1 A     off 0  -> node 20 -> 44
#   R1 B     off 1  -> node 21 -> 45
#   U2 P1    pin 1  -> node 33 -> 17
#   U2 P28   pin 28 -> node 3  -> 18
# C1 has placed: false - it must NOT expand (no 50 -> 52 bridge).
EXPECTED_PAIRS = [(35, "GND"), (36, 44), (38, "TOP_RAIL"), (5, "TOP_RAIL"),
                  (20, 44), (21, 45), (33, 17), (3, 18), (55, 42)]


def read_device_file(path):
    """Return (exists, content) for a device file via the REPL.

    `exists` is TRI-STATE: True (the board said EXISTS= 1), False (the board
    said EXISTS= 0, i.e. CONFIRMED ABSENT) or None (neither - the answer was
    garbled, so we know nothing).

    The distinction is load-bearing in exactly one place: the teardown's arm
    that DELETES /slots/slot3.yaml and reports "did not exist before". Deleting
    a user's real slot 3 because one REPL answer came back truncated is the
    class of loss this wave outlawed after guide_flow's sweep took Kevin's
    555_1/555_2, and test_slot_files.py has carried the tri-state since. It was
    never propagated here. None is falsy and the content stays a str, so every
    comparison caller is unaffected; only the deleting caller inspects it.
    """
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
        return (False if "EXISTS= 0" in out else None), ""
    m = re.search(r"<<<FILE>>>\r?\n(.*)<<<END>>>", out, re.DOTALL)
    return True, (m.group(1) if m else "")


# --- 0. Snapshot the bench: board state, slot3.yaml, active slot ----------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

# Shared helper, not a local regex: 'Q' now answers ACTIVE_SLOT:-1 +
# ACTIVE_PATH:<path> when a FILE context is active, and the old
# r"ACTIVE_SLOT:(\d+)" did not match -1 - this phase aborted outright whenever
# the bench happened to be sitting on a project run file.
orig_slot, orig_path = active_context(1.5)
check(orig_slot is not None, "queried active context ('Q')")
check(orig_path is not None, "'Q' reports ACTIVE_PATH")
if orig_slot is None:
    orig_slot = 0

slot3_existed, slot3_before = read_device_file(SLOT_PATH)
print(f"  info: active slot {orig_slot}, slot3.yaml existed: {slot3_existed}")

# Which run-file scheme is flashed (jl.project_run_mode). Only phase 8 cares:
# load_project's NAME form creates one, and its name differs between the
# single-file default and JL_PROJECT_RUN_HISTORY. Everything this suite writes
# lives in /projects/tmptest, a directory it creates and deletes, so no
# shipped project's run file is ever in reach.
RUN_MODE = project_run_mode()
print(f"  info: firmware run-file mode: {RUN_MODE}")

# The "somewhere that isn't slot 3" the phases below bounce through to force
# a re-read. Computed here, with orig_slot, so the finally can use it too.
bounce = orig_slot if (orig_slot is not None and 0 <= orig_slot < 8
                       and orig_slot != 3) else 2

# Everything that MUTATES the board lives inside this try; the tmptest
# teardown + phase 9's restore are its finally. Without it an uncaught
# exception anywhere below walks out with slot 3 overwritten, /projects/
# tmptest still on the flash and the active slot moved - a stranded bench
# that reads as a firmware bug the next time anyone looks. Same shape
# test_guide_flow.py and test_projects.py use. Phase 0 stays OUTSIDE: the
# finally needs snapshot/orig_slot/slot3_before/bounce bound first.
#
# The slot-3 bounce below is the FIRST mutating action, so it belongs inside
# the try, not above it - it moves the active slot, and a failure between it
# and the first phase would otherwise leave the bench on someone else's slot.
try:
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
print("nopin=", 1 if is_connected(37, 7) else 0)
print("unplaced=", 1 if is_connected(50, 52) else 0)
""")
    vals = parse_kv(out)
    for i, (a, b) in enumerate(EXPECTED_PAIRS):
        check(vals.get(f"conn{i}") == 1, f"bridge {a} <-> {b} exists after load")
    check(vals.get("bogus") == 0,
          "futuresection: content was contained (12-48 did NOT become a bridge)")
    check(vals.get("nopin") == 0, "pins without connect: made no bridge (37-7 absent)")
    check(vals.get("unplaced") == 0,
          "placed: false part C1 was NOT expanded (50-52 absent)")

    # --- 2a. list_parts() exposes `placement` (task 9 §A item 4) -------------
    # The mode is what partPinNode resolved every `node` through, so without
    # it a script cannot tell a COMPACT leg sitting in its endpoint hole from
    # an expanded leg that happens to have landed on the same row. C1 is the
    # one part in SLOT_YAML that sets it, so this needle covers the whole
    # path at once: parser -> PartDefinition::placement -> jl_get_part_info's
    # record -> the binding's positional split -> the dict key.
    #
    # The two node assertions are what make it non-vacuous: under compact,
    # pin A (connect: 52, a real hole row) IS node 52, and open pin B follows
    # its partner into the adjacent row 53 - neither is C1's footprint row
    # (50/51), so a hard-coded "expanded" would fail them.
    out = jl_exec("""
by = {}
for p in list_parts():
    by[p['name']] = p
print("c1place=", by['C1']['placement'])
print("c1anode=", by['C1']['pins']['A']['node'])
print("c1bnode=", by['C1']['pins']['B']['node'])
print("u1place=", by['U1']['placement'])
print("r1place=", by['R1']['placement'])
""")
    vals = parse_kv(out)
    check(vals.get("c1place") == "compact",
          "list_parts: C1's placement: compact is exposed as 'compact'")
    check(vals.get("c1anode") == 52,
          "list_parts: C1 pin A's node is its connect: hole (52) - the node "
          "really was resolved in compact mode")
    check(vals.get("c1bnode") == 53,
          "list_parts: C1's open pin B followed its partner into row 53")
    check(vals.get("u1place") == "expanded",
          "list_parts: a part with no placement: reports 'expanded' (the default)")
    check(vals.get("r1place") == "expanded",
          "list_parts: R1 too - the field is per-part, not global")

    # --- 2b. The overlays: section survived a parts value that says
    # "overlays:" -------------------------------------------------------------
    # Section-hijack regression (see the SLOT_YAML note): a bare
    # strstr(file, "overlays:") started the overlay scan inside the PARTS
    # section, at U1's value, and then scanned forward - so R1, U2 and C1
    # were each read as an overlay entry and the table came back holding
    # FOUR, three of them named after parts. The scanner now anchors on an
    # un-indented `overlays:` line, so exactly one overlay may be present and
    # it must be RTTEST. Both assertions below are load-bearing: the count
    # catches the extra entries, the name regex catches WHICH ones.
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
    check(all(str(n) in re.findall(r"\d+", trig_nodes) for n in (36, 44, 20)),
          f"U1_TRIG net contains nodes 36, 44, 20 ({trig_nodes.strip()})")
    # Collision rule: R1 pin A shares the 36/44/20 net; lowest part index (U1) won.
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
    for needle in ('- name: "U1"', "type: ic", 'value: "overlays:"', "footprint: dip8",
                   "row: 35", "placed: true",
                   "GND: {pin: 1, connect: GND, class: gnd}",
                   "TRIG: {pin: 2, connect: 44, class: signal}",
                   "OUT: {pin: 3, class: signal}",
                   "RESET: {pin: 4, connect: TOP_RAIL, class: power}",
                   "CTRL: {pin: 5, class: nc}",
                   "VCC: {pin: 8, connect: TOP_RAIL, class: power}",
                   '- name: "R1"', "type: resistor", 'value: "10k"',
                   "footprint: sip2", "row: 20",
                   "A: {offset: 0, connect: 44, class: signal}",
                   "B: {offset: 1, connect: 45, class: signal}",
                   '- name: "U2"', "footprint: dip28",
                   "P1: {pin: 1, connect: 17, class: signal}",
                   "P28: {pin: 28, connect: 18, class: signal}",
                   '- name: "C1"', "placed: false",
                   # The PARSE half of the placement byte. C1 is the only part
                   # in this file that sets it, and it is placed: false, so
                   # this is a pure data round-trip - no expansion changes.
                   # Without this needle a typo in parsePartLine's key would
                   # pass every suite: the field would be skipped as an unknown
                   # key, default to expanded, and the serializer would omit it
                   # again - the wholesale-rewrite erasure class, invisible.
                   "placement: compact",
                   # The same erasure class for wave 2's `tol:` (the author
                   # tolerance the derived continuity band adds to tol_meas).
                   # R1 is the only part that sets it, so this is the parse
                   # half; the count assertion below is the emit half.
                   "tol: 5",
                   "A: {offset: 0, connect: 52, class: signal}",
                   "B: {offset: 1, class: signal}",
                   # ...and the real overlays: section, so the hijack needle
                   # (U1's value, the first part - see the SLOT_YAML note) has
                   # to survive a rewrite too.
                   "\noverlays:\n", '- name: "RTTEST"'):
        check(needle in rewritten, f"rewrite kept: {needle}")
    check(re.search(r'guideProgress: \{source: "/projects/test/wiring.yaml", step: 2\}',
                    rewritten) is not None, "guideProgress scalar survived the rewrite")
    check(re.search(r"- \{n1: 55, n2: 42, dup: 2", rewritten) is not None,
          "plain bridge 55-42 (dup 2) survived the rewrite")
    check("frobnicate" not in rewritten,
          "unknown part key was tolerated on parse (and not resurrected)")
    check(rewritten.count("tol:") == 1,
          "tol: came back on exactly the ONE part that set it - an unset tol "
          "is the 15% default and emitting 'tol: 0' would both lie and rewrite "
          "every pre-wave-2 file")
    check(rewritten.count("placement:") == 1,
          "placement: came back on exactly the ONE part that set it - the "
          "other three are expanded, the default, which is never emitted "
          "(so pre-wave-2 files stay byte-identical through a rewrite)")
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
print("conn_trig=", 1 if is_connected(36, 44) else 0)
print("conn_r1b=", 1 if is_connected(21, 45) else 0)
names = []
for i in range(1, 59):
    nm = get_net_name(i)
    if nm and "_" in str(nm):
        names.append(str(nm))
print("names=", ",".join(sorted(names)))
""")
    vals = parse_kv(out)
    check(vals.get("conn_trig") == 1, "second load of the rewritten file: 36-44 bridge intact")
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
    # YAML), so every part seen from here on came from the API. Geometry (wave 2
    # binding geometry - DIP row MUST be bottom-half, axial2 row MUST be top-half):
    #   U9 sip2 @ row 5 (footprint inferred):  A pin 1 -> node 5, B pin 2 -> node 6
    #   U8 dip8 @ row 44 (explicit):           VCC pin 8 -> (44-30)+(8-8) = node 14
    #   AX1 axial2 @ row 27 (explicit):        A pin 1 -> node 27, B pin 2 -> node 57
    out = jl_exec("""
print("gp_none=", guide_progress())
print("rc=", place_part("U9", 5, '{"A": {"pin": 1, "connect": "GND"}, "B": {"pin": 2, "connect": 7}}'))
print("rc_dip=", place_part("U8", 44, '{"VCC": {"pin": 8, "connect": "TOP_RAIL"}}', "dip8", "ic", "TEST"))
print("rc_ax=", place_part("AX1", 27, '{"A": {"pin": 1, "connect": 40}, "B": {"pin": 2, "connect": 41}}', "axial2"))
print("rc_dup=", place_part("U9", 20, '{"A": {"pin": 1, "connect": 30}}'))
print("rc_odd=", place_part("BAD1", 20, '{"A": {"pin": 1, "connect": 30}}', "dip7"))
print("rc_row=", place_part("BAD2", 99, '{"A": {"pin": 1, "connect": 30}}'))
print("rc_nopins=", place_part("BAD3", 20, '{}'))
print("rc_diptop=", place_part("BAD4", 5, '{"A": {"pin": 1, "connect": 32}}', "dip8"))
print("rc_axbot=", place_part("BAD5", 35, '{"A": {"pin": 1, "connect": 32}}', "axial2"))
print("rc_dipfit=", place_part("BAD6", 58, '{"A": {"pin": 1, "connect": 50}, "D": {"pin": 4, "connect": 51}}', "dip8"))
print("rc_offbad=", place_part("BAD7", 25, '{"A": {"offset": 0, "connect": 40}, "B": {"offset": 10, "connect": 41}}', "sip2"))
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
    check(vals.get("rc_dip") == 0, "place_part with an explicit dip8 (bottom-half row) + type + value returned 0")
    check(vals.get("rc_ax") == 0, "place_part with an explicit axial2 (top-half row) returned 0")
    check(vals.get("rc_dup") == -1, "place_part on an existing name is refused (-1)")
    # The refusals below are commitPart()'s predicate: an entry that passed
    # here but failed the parser would be auto-saved and then silently DROPPED on
    # the next load (the erasure bug of commit 352bb23).
    check(vals.get("rc_odd") == -1, "place_part with an odd DIP pin count is refused (-1)")
    check(vals.get("rc_row") == -1, "place_part with row 99 is refused (-1)")
    check(vals.get("rc_nopins") == -1, "place_part with no parseable pins is refused (-1)")
    # The wave-2 rejection needles: a top-anchored DIP (the mirrored bug) and a
    # bottom-anchored axial2 (pin 2 would fall off the board) must both fail
    # cleanly - no entry, no crash - matching PartPlacement.cpp's new validation.
    check(vals.get("rc_diptop") == -1,
          "place_part with a top-anchored dip8 (row 5) is refused (-1) - bottom half only")
    check(vals.get("rc_axbot") == -1,
          "place_part with a bottom-anchored axial2 (row 35) is refused (-1) - top half only")
    # Column-fit needle: dip8 at row 58 doesn't fit (near side would overflow
    # past row 60 at the same row the far side overflows past row 30 - pin 1
    # (node 58) would have worked on its own, pin 4 (node 61) would not; the
    # WHOLE part must be refused, not just the pins that don't fit.
    check(vals.get("rc_dipfit") == -1,
          "place_part with a dip8 at row 58 (does not fit the board) is refused (-1)")
    # THE OFFSET GAP (wave 2, routed from task 3's re-review). An `offset:`
    # pin bypasses nodeForPin entirely, so the footprint SPAN check never saw
    # it: sip2 at row 25 spans 25-26 and passes, while pin B's offset 10 puts
    # it at row 35 - across the ravine, off its own half. The part used to be
    # accepted and then placed PARTIALLY (pin A bridged, pin B silently
    # nowhere), which is the same silent-loss class as the dip8-at-58 needle
    # above. The whole part must be refused now, on this path AND on the YAML
    # path - both call the same partGeometryOk().
    check(vals.get("rc_offbad") == -1,
          "place_part with an out-of-bounds offset: pin (row 25 + 10 crosses "
          "the ravine) is refused (-1) - the whole part, not just that pin")
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
    # ...and nothing partial landed: only the three parts that were accepted -
    # including BAD6, whose pin 1 (node 58) would have worked on its own.
    check(vals.get("names_after") == "AX1,U8,U9",
          f"every refused place_part left NO entry behind (table holds "
          f"{vals.get('names_after')!r}, expected 'AX1,U8,U9')")

    out = jl_exec("""
print("u9a=", 1 if is_connected(5, "GND") else 0)
print("u9b=", 1 if is_connected(6, 7) else 0)
print("u8vcc=", 1 if is_connected(14, "TOP_RAIL") else 0)
print("ax1a=", 1 if is_connected(27, 40) else 0)
print("ax1b=", 1 if is_connected(57, 41) else 0)
print("bad=", 1 if is_connected(20, 30) else 0)
print("diptopbad=", 1 if is_connected(5, 32) else 0)
print("dipfitbad=", 1 if is_connected(58, 50) else 0)
print("offbad=", 1 if is_connected(25, 40) else 0)
print("badchar=", 1 if is_connected(30, 31) else 0)
for i in range(1, 59):
    nodes = get_net_nodes(i)
    if nodes:
        print("NET|%d|%s|%s" % (i, str(get_net_name(i)), str(nodes)))
""")
    vals = parse_kv(out)
    check(vals.get("u9a") == 1, "place_part expanded U9 pin A: bridge 5 <-> GND")
    check(vals.get("u9b") == 1, "place_part expanded U9 pin B: bridge 6 <-> 7")
    check(vals.get("u8vcc") == 1, "place_part expanded U8 pin VCC (dip8 pin 8 -> node 14) to TOP_RAIL")
    check(vals.get("ax1a") == 1, "place_part expanded AX1 pin A: bridge 27 (row) <-> 40")
    check(vals.get("ax1b") == 1, "place_part expanded AX1 pin B: bridge 57 (row+30) <-> 41")
    check(vals.get("bad") == 0, "no bridge from any refused place_part call (20-30 absent)")
    check(vals.get("diptopbad") == 0,
          "no bridge from the rejected top-anchored dip8 (5-32 absent)")
    check(vals.get("dipfitbad") == 0,
          "no PARTIAL placement from the rejected off-board dip8 (58-50 absent, "
          "even though pin 1 alone would have fit)")
    check(vals.get("offbad") == 0,
          "no PARTIAL placement from the rejected offset: part (25-40 absent, "
          "even though pin A's offset 0 alone would have fit)")
    check(vals.get("badchar") == 0,
          "no bridge from any charset-refused place_part call (30-31 absent)")

    nets = {}
    for line in out.splitlines():
        m = re.match(r"\s*NET\|(\d+)\|(.*)\|(.*?)\s*$", line)
        if m:
            nets[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())
    net7 = None
    gndnet = None
    ax1a_net = None
    ax1b_net = None
    for num, (name, nodes) in nets.items():
        toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
        if "7" in toks:
            net7 = (num, name)
        if "5" in toks and name == "GND":
            gndnet = num
        if "27" in toks:
            ax1a_net = (num, name)
        if "57" in toks:
            ax1b_net = (num, name)
    check(net7 is not None and net7[1] == "U9_B",
          f"the net holding node 7 is named U9_B (got {net7})")
    check(gndnet is not None,
          "U9 pin A landed on GND and the special net kept its name (never renamed by parts)")
    check(ax1a_net is not None and ax1a_net[1] == "AX1_A",
          f"the net holding node 27 (AX1 pin 1 = row) is named AX1_A (got {ax1a_net})")
    check(ax1b_net is not None and ax1b_net[1] == "AX1_B",
          f"the net holding node 57 (AX1 pin 2 = row+30) is named AX1_B (got {ax1b_net})")

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
ax1 = by['AX1']
print("ax1row=", ax1['row'])
print("ax1fp=", ax1['footprint'])
print("ax1anode=", ax1['pins']['A']['node'])
print("ax1bnode=", ax1['pins']['B']['node'])
""")
    vals = parse_kv(out)
    check(vals.get("n") == 3,
          "list_parts() returned all three placed parts and nothing from the refused calls")
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
    check(vals.get("u8vnode") == 14, "list_parts: U8 pin VCC resolves to node 14")
    check(vals.get("ax1row") == 27, "list_parts: AX1 row is 27")
    check(vals.get("ax1fp") == "axial2", "list_parts: AX1 footprint is axial2")
    check(vals.get("ax1anode") == 27, "list_parts: AX1 pin A resolves to node 27 (row)")
    check(vals.get("ax1bnode") == 57, "list_parts: AX1 pin B resolves to node 57 (row+30)")

    # The silent-vanish check: a part BUILT by place_part must survive the
    # serializer AND re-parse on the next load (place_part enforces the same
    # predicate commitPart does, so nothing it accepts can be dropped) - this is
    # also the axial2 needle's "round-trip survives an auto-save rewrite" leg.
    out = jl_exec("print('saved=', nodes_save(3))")
    check(parse_kv(out).get("saved") == 3, "nodes_save(3) wrote the API-placed parts")
    time.sleep(1.0)
    _, api_yaml = read_device_file(SLOT_PATH)
    for needle in ('- name: "U9"', "footprint: sip2", "row: 5", "placed: true",
                   "A: {pin: 1, connect: GND, class: signal}",
                   "B: {pin: 2, connect: 7, class: signal}",
                   '- name: "U8"', "footprint: dip8", 'value: "TEST"', "type: ic",
                   "VCC: {pin: 8, connect: TOP_RAIL, class: signal}",
                   '- name: "AX1"', "footprint: axial2", "row: 27",
                   "A: {pin: 1, connect: 40, class: signal}",
                   "B: {pin: 2, connect: 41, class: signal}"):
        check(needle in api_yaml, f"place_part serialized: {needle}")

    port1_command(f"<{bounce}", 4.0)
    time.sleep(1.5)
    port1_command("<3", 4.0)
    time.sleep(2.0)
    out = jl_exec("""
print("n=", len(list_parts()))
print("u9b=", 1 if is_connected(6, 7) else 0)
print("u8vcc=", 1 if is_connected(14, "TOP_RAIL") else 0)
print("ax1a=", 1 if is_connected(27, 40) else 0)
print("ax1b=", 1 if is_connected(57, 41) else 0)
""")
    vals = parse_kv(out)
    check(vals.get("n") == 3, "reload: all three API-placed parts re-parsed (none silently dropped)")
    check(vals.get("u9b") == 1, "reload: U9 pin B bridge 6-7 re-expanded")
    check(vals.get("u8vcc") == 1, "reload: U8 pin VCC bridge 14-TOP_RAIL re-expanded")
    check(vals.get("ax1a") == 1, "reload: AX1 pin A bridge 27-40 re-expanded")
    check(vals.get("ax1b") == 1, "reload: AX1 pin B bridge 57-41 re-expanded")

    out = jl_exec("""
print("rm=", remove_part("U9"))
print("rm2=", remove_part("U8"))
print("rm3=", remove_part("AX1"))
print("rm_missing=", remove_part("U9"))
print("n=", len(list_parts()))
print("u9a=", 1 if is_connected(5, "GND") else 0)
print("u9b=", 1 if is_connected(6, 7) else 0)
print("u8vcc=", 1 if is_connected(14, "TOP_RAIL") else 0)
print("ax1a=", 1 if is_connected(27, 40) else 0)
print("ax1b=", 1 if is_connected(57, 41) else 0)
names = []
for i in range(1, 59):
    nm = get_net_name(i)
    if nm and (str(nm).startswith("U9_") or str(nm).startswith("AX1_")):
        names.append(str(nm))
print("leftover=", ",".join(names) if names else "none")
""", timeout=40)
    vals = parse_kv(out)
    check(vals.get("rm") == 0, "remove_part('U9') returned 0")
    check(vals.get("rm2") == 0, "remove_part('U8') returned 0")
    check(vals.get("rm3") == 0, "remove_part('AX1') returned 0")
    check(vals.get("rm_missing") == -1, "remove_part on an unknown name returns -1")
    check(vals.get("n") == 0, "remove_part dropped the entries (list_parts() empty)")
    check(vals.get("u9a") == 0, "remove_part pulled the 5 <-> GND bridge")
    check(vals.get("u9b") == 0, "remove_part pulled the 6 <-> 7 bridge")
    check(vals.get("u8vcc") == 0, "remove_part pulled the 14 <-> TOP_RAIL bridge")
    check(vals.get("ax1a") == 0, "remove_part pulled the 27 <-> 40 bridge")
    check(vals.get("ax1b") == 0, "remove_part pulled the 57 <-> 41 bridge")
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
    # Slot 3 is the context we are ON right now, so it is the slot the OLD
    # bug would have written the project into. Snapshot it here (not phase 0 -
    # earlier phases legitimately rewrote it) so the after-comparison is exact.
    slot3_lp_existed, slot3_lp_before = read_device_file(SLOT_PATH)
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

    # CONTRACT CHANGE (task 5): load_project's NAME form now BEGINS A RUN.
    # "load project tmptest" means "open its latest run file, or create
    # its run file from the wiring" - which is what stops a script from
    # adopting a shipped template as its auto-saving context and rewriting it
    # without guide:/meta: (task 4's bench-caught destruction path). The
    # literal-path form below is deliberately still the raw adopting loader.
    # Everything this phase actually tests - the parse, the re-expansion, the
    # guideProgress scalar and the slot-clobber regression - is unchanged,
    # because the run file is a verbatim copy of the wiring.
    out = jl_exec("""
print("loaded=", 1 if load_project("tmptest") else 0)
""", timeout=30)
    check(parse_kv(out).get("loaded") == 1,
          "load_project('tmptest') began a run of /projects/tmptest/wiring.yaml")
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

    # The slot-clobber regression, re-aimed at the new model. This used to
    # assert "the active slot is STILL 3", because loadSlotFromPath left slot
    # tracking alone for an unrecognized name - which meant the loaded project
    # was live on the board while the auto-save still pointed at slot 3, and
    # the project's content would be written into slot 3's file. The fix is
    # ADOPTION: the context follows the file, so tracking must now report the
    # PATH (ACTIVE_SLOT:-1 + ACTIVE_PATH:<the project file>), not slot 3.
    #
    # The part that has NOT changed - and is the whole point either way - is
    # that no numbered slot file is touched.
    slot_after_lp, path_after_lp = active_context(1.5)
    check(slot_after_lp == -1,
          f"load_project adopted a project file as a FILE context "
          f"(ACTIVE_SLOT:{slot_after_lp}, expected -1)")
    # The context is the RUN FILE the name form allocated, not the wiring
    # template it was copied from - the template stays read-only.
    # W3-T3: the run file's NAME depends on JL_PROJECT_RUN_HISTORY - one
    # tmptest_run.yaml by default, tmptest_<N>.yaml on a numbered build. The
    # mode is probed, not assumed; what is asserted either way is that the
    # context is a RUN FILE in the project directory and not the template.
    _run_re = (r"^" + re.escape(TMP_PROJ) + r"/tmptest_run\.yaml$"
               if RUN_MODE == "single"
               else r"^" + re.escape(TMP_PROJ) + r"/tmptest_\d+\.yaml$")
    _run_human = ("tmptest_run.yaml" if RUN_MODE == "single" else "tmptest_<N>.yaml")
    check(path_after_lp is not None and re.match(_run_re, path_after_lp),
          f"ACTIVE_PATH is the run file the name form created (got "
          f"{path_after_lp!r}, expected {TMP_PROJ}/{_run_human})")
    check(path_after_lp != TMP_WIRING,
          "the shipped wiring template did NOT become the active context")
    time.sleep(2.0)  # give the idle auto-save a chance to misbehave
    # `is not None` on BOTH ends: read_device_file's third state is "the answer
    # was garbled". Two garbled reads compare equal (None == None, "" == ""),
    # so without this the check passes on NO EVIDENCE - the vacuous-green shape
    # this wave is hunting.
    slot0_after_exists, slot0_after = read_device_file("/slots/slot0.yaml")
    check(slot0_existed is not None and slot0_after_exists is not None
          and slot0_after_exists == slot0_existed and slot0_after == slot0_before,
          "slot-clobber regression: /slots/slot0.yaml is byte-identical after load_project")
    slot3_lp_after_exists, slot3_lp_after = read_device_file(SLOT_PATH)
    check(slot3_lp_existed is not None and slot3_lp_after_exists is not None
          and slot3_lp_after_exists == slot3_lp_existed and slot3_lp_after == slot3_lp_before,
          "slot-clobber regression: slot3.yaml (the context we loaded FROM) is "
          "byte-identical - the project's content did not land in it")

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
    # ORDER MATTERS, and it changed with adoption. load_project now makes the
    # project file the ACTIVE CONTEXT, so its dirty state auto-saves back to
    # ITSELF. Deleting the file first and switching slots second would let the
    # switch's dirty pre-save RE-CREATE the file we just deleted (or fail
    # noisily against a removed directory). So: leave the file context first -
    # which flushes it to its own file - and only then delete it.
    port1_command(f"<{bounce}", 4.0)
    time.sleep(1.5)

    # Every .yaml, not just the wiring: load_project's name form allocated a
    # tmptest run file, and rmdir refuses a directory that still holds
    # one. (Run files are user data - the firmware never removes them, so the
    # suite that minted this one takes it back off the bench.)
    out = jl_exec(f"""
if fs_exists({TMP_PROJ!r}):
    for nm in jfs.listdir({TMP_PROJ!r}):
        if nm.endswith(".yaml"):
            try:
                jfs.remove({TMP_PROJ!r} + "/" + nm)
            except Exception as e:
                print("rmerr=", e)
try:
    jfs.rmdir({TMP_PROJ!r})
except Exception as e:
    print("rmdirerr=", e)
print("gone=", 0 if fs_exists({TMP_PROJ!r}) else 1)
""", timeout=25)
    check(parse_kv(out).get("gone") == 1, f"removed {TMP_PROJ} (the 555 project stays, tmptest does not)")

    # --- 9. Restore the bench --------------------------------------------------
    # Restore the FILE first, switch slots second: if orig_slot happened to be 3,
    # switching first would make slot 3 active again and the idle auto-save could
    # clobber the restored content (the same hazard handled before phase 1).
    # `slot3_existed is False` and NOT `not slot3_existed`: read_device_file
    # returns None when the phase-0 read was garbled, and THIS ARM DELETES. See
    # the helper's docstring - absence has to be positive evidence (EXISTS= 0),
    # never the absence of evidence.
    if slot3_existed:
        out = jl_exec(f"print('restored=', 1 if fs_write({SLOT_PATH!r}, {slot3_before!r}) else 0)")
        check(parse_kv(out).get("restored") == 1, "restored slot3.yaml prior content")
    elif slot3_existed is False:
        out = jl_exec(f"""
if fs_exists({SLOT_PATH!r}):
    jfs.remove({SLOT_PATH!r})
print("removed=", 0 if fs_exists({SLOT_PATH!r}) else 1)
""")
        check(parse_kv(out).get("removed") == 1, "removed the test's slot3.yaml (did not exist before)")
    else:
        check(False,
              "the phase-0 snapshot read of /slots/slot3.yaml was INCONCLUSIVE "
              "(neither EXISTS= 1 nor EXISTS= 0 came back). Leaving the file "
              "alone rather than guessing - but this run's slot3 holds the "
              "suite's fixture, so restore it by hand.")

    # Path-aware: a file context has no "<n" to go back to.
    restore_context(orig_slot, orig_path)
    time.sleep(1.5)

    if snapshot is not None:
        check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

finish("test_parts_roundtrip")
