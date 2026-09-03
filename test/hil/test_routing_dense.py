#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dense / edge-case routings, verified against the schematic.

Each case is pasted as one state document (bridges + zeroed power + the
stacking settings it wants), then the router's view (`b`: bridge array, path
table, chip status) and the hardware view (port-7 `:crossbar`: the crosspoints
the firmware last sent) are checked with routing_check.py's copper model:

  every bridge connected, no two nets on one piece of copper, no net reaching
  a node that is in no net, sent == planned, and the chip-status bookkeeping
  claiming exactly the copper each net rides.

Cases that fit the fabric must route completely; the two that deliberately
exceed it are metrics (they must still be short-free). A couple of cases also
prove the copper electrically (GPIO loopback, DAC through the crossbar to an
ADC) after the model has cleared them - a source is only energised on a
routing the model says has no short. Those proofs sit on rows 18/25 and
18/19/25: with the rails at 0 V a part plugged into the breadboard clamps its
rows (a 4051 on rows 1-8/31-38 pulled DAC0 from 2.4 V to 1.9 V at ADC0, bench
2026-09-03), so the proof rows are ones Kevin's bench leaves empty (columns
18-30 and 48-60).

Set JL_ROUTING_SNAP_DIR=<dir> to save every case's raw captures, then
test/hil/tools/routing_snapshot_diff.py <before> <after> shows what a router
change moved.

The bench is snapshotted first and restored in a finally, so the file is
safe to run on its own.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jl
from jl import jl_exec, port1_command, port1_paste, check, finish, parse_kv
from port7 import port7_command
import routing_check as rc
from fabric_v5 import NODE_NAME

# node ids (src/JumperlessDefines.h)
GND, TOP, BOT = 100, 101, 102
DAC0, DAC1 = 106, 107
ADC0, ADC1, ADC2, ADC3 = 110, 111, 112, 113
UART_TX, UART_RX = 116, 117
GP = {i: 130 + i for i in range(1, 9)}          # GP[1] = RP_GPIO_1 = 131
D = {i: 70 + i for i in range(14)}              # D[0] = NANO_D0
A = {i: 86 + i for i in range(8)}               # A[0] = NANO_A0

ALL_PAIRS_28 = [
    (1, 8), (2, 15), (3, 22), (4, 31), (5, 38), (6, 45), (7, 52),
    (9, 16), (10, 23), (11, 32), (12, 39), (13, 46), (14, 53),
    (17, 24), (18, 33), (19, 40), (20, 47), (21, 54),
    (25, 34), (26, 41), (27, 48), (28, 55),
    (35, 42), (36, 49), (37, 56),
    (43, 50), (44, 57),
    (51, 58),
]

# name, bridges, all_routed, (stack_paths, stack_rails, stack_dacs), electrical
CASES = [
    ("same_chip_pairs",
     [(1, 2), (3, 4), (5, 6), (8, 9), (10, 11), (15, 16)],
     True, (0, 0, 0), None),
    ("chain_all_chips",
     [(1, 8), (8, 15), (15, 22), (22, 31), (31, 38), (38, 45), (45, 52)],
     True, (0, 0, 0), None),
    ("parallel_AB",
     [(1, 8), (2, 9), (3, 10), (4, 11), (5, 12),
      (GP[1], 18), (GP[2], 25), (18, 25)],
     True, (0, 0, 0), ("loopback", 1, 2)),
    # exactly eight chip-K rows: two rails on two chips each, one DAC/ADC
    # net on two chips, DAC1 on one, plus the probe feed's row
    ("k_rows_all_eight",
     [(TOP, 1), (TOP, 8), (BOT, 31), (BOT, 38), (DAC0, 18), (DAC0, 19),
      (ADC0, 18), (ADC0, 25), (DAC1, 45)],
     True, (0, 0, 0), ("dac_adc", 0, 0, 2.5)),
    ("gnd_spread",
     [(GND, 1), (GND, 8), (GND, 15), (GND, 22), (GND, 31), (GND, 38),
      (GND, 45), (GND, 52)],
     True, (0, 0, 0), None),
    ("nano_moderate",
     [(D[0], 1), (D[1], 2), (D[2], 3), (D[3], 4),
      (A[0], 15), (A[1], 16), (A[2], 17), (A[3], 18)],
     True, (0, 0, 0), None),
    ("gpio_seven_from_chip_a",
     [(GP[i], i) for i in range(1, 8)],
     True, (0, 0, 0), None),
    ("sf_to_sf",
     [(DAC0, ADC0), (GP[1], ADC2), (GP[2], D[0]), (TOP, D[13]),
      (GP[3], GP[4]), (D[2], A[0]), (GND, A[1]), (UART_TX, D[1])],
     True, (0, 0, 0), ("loopback", 3, 4)),
    ("corner_rows",
     [(29, 1), (30, 8), (59, 31), (60, 38), (29, 30), (59, 60)],
     True, (0, 0, 0), None),
    ("big_net_thirteen_nodes",
     [(1, 8), (8, 15), (15, 22), (22, 31), (31, 38), (38, 45), (45, 52),
      (52, 2), (2, 9), (9, 16), (16, 23), (23, TOP)],
     True, (0, 0, 0), None),
    ("all_28_chip_pairs",
     ALL_PAIRS_28,
     True, (0, 0, 0), None),
    # forces the hub tier's H2 shape (row to row THROUGH an SF chip): seven
    # A-E nets spend the single A-E lane and every other chip's bounce row,
    # then four C-G nets (one lane) find no lane and no bounce row left
    ("h2_through_sf_chips",
     [(i, 30 + i) for i in range(1, 8)] + [(15, 45), (16, 46), (17, 47), (18, 48)],
     True, (0, 0, 0), None),
    ("nano_gpio_realistic",
     [(D[2], 1), (D[3], 2), (A[0], 15), (GP[1], 3), (GP[2], 4),
      (UART_TX, D[0]), (UART_RX, D[1]), (TOP, 5), (GND, 6), (GND, 16),
      (DAC0, 17), (ADC0, 18)],
     True, (0, 0, 0), None),
    ("kevin_fixture_2026_09_02",
     [(1, TOP), (38, GND), (2, ADC0), (ADC1, 3), (ADC2, 4), (5, ADC3),
      (6, GP[1]), (7, GP[2]), (GP[3], 8), (TOP, 11), (47, GND), (ADC3, D[0])],
     True, (2, 3, 0), None),
    ("rail_stacking_light",
     [(TOP, 1), (TOP, 15), (BOT, 31), (GND, 45), (DAC0, 8)],
     True, (2, 3, 1), None),
    # beyond the fabric on purpose: metrics only, but never a short
    ("nano_dense_metric",
     [(D[i], i + 1) for i in range(6)] + [(A[i], i + 15) for i in range(6)],
     False, (0, 0, 0), None),
    ("k_rows_overflow_metric",
     [(TOP, 1), (TOP, 15), (BOT, 8), (BOT, 22), (DAC0, 31), (DAC1, 38),
      (ADC0, 45), (ADC1, 52), (ADC2, 3), (ADC3, 17), (GND, 2), (GND, 9)],
     False, (0, 0, 0), None),
]


def case_yaml(bridges, stacking):
    """One pastable state document. Stacking is carried PER BRIDGE as an
    explicit `dup:` count: the router honours a stored count >= 0 as-is, so
    the case controls its duplicates without touching the global
    [routing] stack_* config. (The slot loader turns a count equal to the
    slot's stackPaths back into "default", so the config block pins that at
    a value no case uses.)"""
    paths, rails, dacs = stacking
    lines = ["version: 2", "sourceOfTruth: bridges", "", "bridges:"]
    for n1, n2 in bridges:
        cls = paths
        if n1 in (TOP, BOT, GND) or n2 in (TOP, BOT, GND):
            cls = rails
        elif n1 in (DAC0, DAC1) or n2 in (DAC0, DAC1):
            cls = dacs
        lines.append("  - {n1: %d, n2: %d, dup: %d}" % (n1, n2, cls))
    lines += ["", "power:", "  topRail: 0.00", "  bottomRail: 0.00",
              "  dac0: 0.00", "  dac1: 0.00", "",
              "config:",
              "  routing: {stackPaths: 9, stackRails: 9, stackDacs: 9, railPriority: 1}",
              ""]
    return "\n".join(lines) + "\n"


def net_scan_enabled():
    """[measurement] net_currents from /config.txt (the background net-voltage
    scan). While it runs it closes ephemeral sense taps to ADC0 that a
    crossbar snapshot can catch mid-flight, so the cases run with it off."""
    cfg = jl.device_text("/config.txt")
    m = re.search(r"\[measurement\](.*?)(?=\n\[|\Z)", cfg, re.S)
    if not m:
        return None
    m2 = re.search(r"net_currents\s*=\s*(\d)", m.group(1))
    return int(m2.group(1)) if m2 else None


def load_case(bridges, stacking):
    prompt, out = port1_paste("S", case_yaml(bridges, stacking).encode(), settle=3.0)
    return out


def capture():
    b = port1_command("b", 3.0, quiet_after=0.6, max_seconds=20)
    x = port7_command(":crossbar\n", 1.0)
    return b, x


def normalized_paths(b_text):
    """The path table with the colour and column noise gone, for diffs."""
    rows = rc.parse_paths(b_text)
    return ["%d net%d %s-%s %s" % (r["path"], r["net"], NODE_NAME.get(r["node1"], r["node1"]),
                                   NODE_NAME.get(r["node2"], r["node2"]),
                                   " ".join("%s%d.%d" % h for h in r["hops"]))
            for r in rows if r["net"] >= 0]


def electrical(spec):
    kind = spec[0]
    if kind == "loopback":
        a, b = spec[1], spec[2]
        out = jl_exec(f"""
import time
gpio_set_dir(GPIO_{b}, INPUT)
try:
    gpio_set_pull(GPIO_{b}, PULL_NONE)
except NameError:
    gpio_set_pull(GPIO_{b}, 0)
gpio_set_dir(GPIO_{a}, OUTPUT)
gpio_set(GPIO_{a}, HIGH)
time.sleep(0.02)
s = str(gpio_get(GPIO_{b})).lower()
print("hi=", 1 if ("high" in s or s == "1" or s == "true") else 0)
gpio_set(GPIO_{a}, LOW)
time.sleep(0.02)
s = str(gpio_get(GPIO_{b})).lower()
print("lo=", 1 if ("high" in s or s == "1" or s == "true") else 0)
gpio_set_dir(GPIO_{a}, INPUT)
""")
        v = parse_kv(out)
        return v.get("hi") == 1 and v.get("lo") == 0, f"GPIO_{a} -> GPIO_{b} loopback hi={v.get('hi')} lo={v.get('lo')}"
    if kind == "dac_adc":
        dac, adc, volts = spec[1], spec[2], spec[3]
        out = jl_exec(f"""
import time
dac_set(DAC{dac}, {volts}, False)
time.sleep(0.05)
vs = [adc_get({adc}) for _ in range(8)]
print("v=", sum(vs) / len(vs))
dac_set(DAC{dac}, 0.0, False)
""")
        v = parse_kv(out)
        got = v.get("v", -99)
        return abs(got - volts) < 0.3, f"DAC{dac} {volts}V reads {got:.3f}V at ADC{adc} through the crossbar"
    raise ValueError(spec)


snap_dir = os.environ.get("JL_ROUTING_SNAP_DIR")
if snap_dir:
    os.makedirs(snap_dir, exist_ok=True)

snapshot = jl.board_state_capture()
check(snapshot is not None, "bench snapshot taken before the cases")
orig_slot, orig_path = jl.active_context(1.5)
print(f"  pre-suite context: slot {orig_slot}, path {orig_path!r}")

scan_was_on = net_scan_enabled()
print(f"  net-voltage scan: {scan_was_on}")
if scan_was_on == 1:
    port1_command("i", 2.0)           # bare i toggles (and persists) the scan
    check(net_scan_enabled() == 0, "net-voltage scan paused for the cases")

try:
    ver = port1_command("?", 1.5)
    m = re.search(r"firmware version:\s*([0-9.]+)", ver)
    print(f"  firmware {m.group(1) if m else '?'}")

    metrics = {}
    determinism = {}
    for name, bridges, all_routed, stacking, elec in CASES:
        print(f"\n--- {name}: {len(bridges)} bridges, stacking {stacking}")
        paste_out = load_case(bridges, stacking)
        check("State applied successfully" in paste_out, f"{name}: state paste applied")
        time.sleep(0.4)
        b, x = capture()
        if snap_dir:
            for suffix, text in (("paste", paste_out), ("b", b), ("xbar", x)):
                with open(os.path.join(snap_dir, f"{name}.{suffix}.txt"), "w") as f:
                    f.write(text)
        # strict claims only when nothing was left unrouted: a path that
        # fails after ijklPaths keeps its X claims (harmless within one
        # rebuild, see the sweep ledger F7), and a culled duplicate keeps
        # its claims too
        strict = (stacking == (0, 0, 0)) and all_routed
        v, m = rc.full_check(b, x, expect_all_routed=all_routed, strict_claims=strict)
        # the paste itself is where "Couldn't find a path" is printed
        for line in rc.unrouted_lines(paste_out):
            (v.bad if all_routed else v.notes.append)("during paste: " + line)
        hub_rows = [r for r in rc.parse_paths(b)
                    if r["net"] >= 0 and r["dup"] == 0 and len(r["hops"]) == 4
                    and r["hops"][2][0] in "IJKL" and r["hops"][3][0] in "IJKL"
                    and r["ptype"].startswith("BB to")]
        m["hub_tier"] = len(hub_rows)
        metrics[name] = m
        print(f"  metrics: {m}")
        for r in hub_rows:
            print("  hub-tier path %d %s-%s: %s" % (
                r["path"], NODE_NAME.get(r["node1"], r["node1"]),
                NODE_NAME.get(r["node2"], r["node2"]),
                " ".join("%s%d.%d" % h for h in r["hops"])))
        for n in v.notes:
            print(f"  note: {n}")
        check(v.ok(), f"{name}: routing model clean" +
              ("" if v.ok() else "\n      " + "\n      ".join(v.problems)))
        if all_routed:
            check(m.get("routed_primaries") == m.get("primaries"),
                  f"{name}: every primary path routed ({m.get('routed_primaries')}/{m.get('primaries')})")
        if elec is not None and v.ok():
            ok, msg = electrical(elec)
            check(ok, f"{name}: {msg}")
        elif elec is not None:
            print(f"  skipping the electrical proof: the model found problems")
        determinism[name] = normalized_paths(b)

    # --- determinism: the same document pasted again must route the same ---
    for name in ("parallel_AB", "gnd_spread", "k_rows_all_eight"):
        spec = next(c for c in CASES if c[0] == name)
        load_case(spec[1], spec[3])
        time.sleep(0.4)
        b, _ = capture()
        again = normalized_paths(b)
        same = again == determinism[name]
        check(same, f"{name}: identical routing when the same state is loaded again")
        if not same:
            for l1, l2 in zip(determinism[name], again):
                if l1 != l2:
                    print(f"      first : {l1}\n      second: {l2}")

    # --- the hub tier's second shape must have been exercised ---
    check(metrics.get("h2_through_sf_chips", {}).get("hub_tier", 0) >= 1,
          "h2_through_sf_chips routed at least one row-to-row path through an SF chip")

    # --- rail stacking: what the duplicates actually bought ---
    m = metrics.get("rail_stacking_light", {})
    print(f"\n  rail_stacking_light routed duplicates: {m.get('routed_duplicates')}")
    check(m.get("routed_duplicates", 0) >= 1,
          "rail stacking produced at least one routed duplicate on a light netlist")

finally:
    jl_exec("nodes_clear()\nprint('cleared')", timeout=25)
    if scan_was_on == 1:
        port1_command("i", 2.0)
        check(net_scan_enabled() == 1, "net-voltage scan turned back on")
    if orig_slot is not None or orig_path:
        jl.guarded(jl.restore_context, orig_slot, orig_path)
    if snapshot is not None:
        restored, err = jl.guarded(jl.board_state_restore, snapshot)
        check(bool(restored), f"bench restored to the pre-suite snapshot ({err!r})" if not restored
              else "bench restored to the pre-suite snapshot")

finish("test_routing_dense")
