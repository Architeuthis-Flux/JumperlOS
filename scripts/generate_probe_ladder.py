#!/usr/bin/env python3
"""Generate src/Probing/ProbeLadderTable.h from the probe-sense KiCad schematic.

The Jumperless V5 probe decodes which pad the tip touches from a resistor
ladder: tip drive -> pad -> ladder resistance -> ADC_PROBE_SENSE node -> 10K
to GND -> ADC5. This script extracts the per-pad ladder resistance (the
series resistance from each pad to the sense node) straight from the design
schematic, so the firmware's expected-count table is built from the real BOM
instead of a user calibration.

Source of truth:
  Jumperless23V50/EverythingOnOneSchematic/EverythingOnOneSchematic.kicad_sch
(the complete shipped V5 design in one flat sheet - it contains the row
strips, rail pads, nano arrays, and SF/logo pads in one connected network,
unlike the per-board project files). The netlist is built from wires,
junctions, labels, and pin positions, then the resistor graph is walked
from the ADC_PROBE_SENSE label; the ladder is a tree, so each pad's series
resistance to the sense node is the unique path sum.

Output: a 102-entry table in probeRowMap order (index 0 = sentinel,
1..30 rows, 31/32 top rail/gnd, 33/34 bottom rail/gnd, 35..64 rows 31-60,
65..94 nano header, 95..101 logo/SF/building pads).

Self-check (the runnable check for this table): exactly 101 pads resolve,
resistance is strictly monotonic in pad index, and the chain-region
deviation from the legacy linear map stays under 2 count-bands.

Usage:  python3 scripts/generate_probe_ladder.py [path-to-kicad_sch]
"""

import math
import os
import re
import sys
from collections import defaultdict

DEFAULT_SCH = (
    "/Users/kevinsanto/Documents/GitHub/JumperlessV5/Jumperless23V50/"
    "EverythingOnOneSchematic/EverythingOnOneSchematic.kicad_sch"
)
OUT_HEADER = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src", "Probing", "ProbeLadderTable.h",
)

# ---------------------------------------------------------------------------
# s-expression parsing
# ---------------------------------------------------------------------------

def parse_sexpr(text):
    tokens = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+', text)

    def build(i):
        assert tokens[i] == "("
        out = []
        i += 1
        while tokens[i] != ")":
            if tokens[i] == "(":
                sub, i = build(i)
                out.append(sub)
            else:
                t = tokens[i]
                if t.startswith('"'):
                    t = t[1:-1]
                out.append(t)
                i += 1
        return out, i + 1

    tree, _ = build(0)
    return tree


def find_all(node, name):
    res = []
    if isinstance(node, list):
        if node and node[0] == name:
            res.append(node)
        for c in node:
            res += find_all(c, name)
    return res


def child(node, name):
    for c in node:
        if isinstance(c, list) and c and c[0] == name:
            return c
    return None


def parse_value(v):
    """Resistor value string -> ohms (handles 10K, 4.7k, 1M, plain ints)."""
    v = v.strip()
    m = re.fullmatch(r"([0-9.]+)\s*([kKmMrR]?)", v)
    if not m:
        raise ValueError(f"unparseable resistor value: {v!r}")
    num = float(m.group(1))
    suffix = m.group(2).lower()
    if suffix == "k":
        num *= 1e3
    elif suffix == "m":
        num *= 1e6
    return num


# ---------------------------------------------------------------------------
# netlist by pin abutment
# ---------------------------------------------------------------------------

def pin_transform(px, py, rot, mirror):
    """KiCad symbol-instance transform: rotate, mirror, then schematic Y-down.

    Verified against the V5 r8 sheet: J1711 (rot 90, no mirror) and J1712
    (rot 90, mirror x) land on the same pin grid only with rotate-first.
    """
    r = math.radians(rot)
    rx = px * math.cos(r) - py * math.sin(r)
    ry = px * math.sin(r) + py * math.cos(r)
    if mirror == "x":
        ry = -ry
    elif mirror == "y":
        rx = -rx
    return rx, -ry  # schematic Y axis points down


def snap(x, y):
    return (round(x * 100), round(y * 100))  # 0.01mm grid


def netlist(sch_path):
    tree = parse_sexpr(open(sch_path).read())

    # library pin definitions
    lib_pins = {}  # lib_id -> list of (number, name, rel_x, rel_y)
    libs = find_all(tree, "lib_symbols")[0]
    for sym in libs[1:]:
        pins = []
        for pin in find_all(sym, "pin"):
            at = child(pin, "at")
            num = child(pin, "number")
            nm = child(pin, "name")
            pins.append((num[1], nm[1] if nm else "?", float(at[1]), float(at[2])))
        lib_pins[sym[1]] = pins

    # symbol instances (top-level only: direct children of the root)
    instances = []
    for node in tree:
        if isinstance(node, list) and node and node[0] == "symbol":
            lib_id = child(node, "lib_id")[1]
            at = child(node, "at")
            x, y = float(at[1]), float(at[2])
            rot = float(at[3]) if len(at) > 3 else 0.0
            mirror_node = child(node, "mirror")
            mirror = mirror_node[1] if mirror_node else None
            ref = value = None
            for prop in find_all(node, "property"):
                if prop[1] == "Reference":
                    ref = prop[2]
                elif prop[1] == "Value":
                    value = prop[2]
            instances.append(dict(lib_id=lib_id, ref=ref, value=value,
                                  x=x, y=y, rot=rot, mirror=mirror))

    # absolute pin positions
    parent = {}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        parent.setdefault(a, a)
        parent.setdefault(b, b)
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    # wires: each segment's two endpoints are one net
    segments = []
    for wire in find_all(tree, "wire"):
        pts = child(wire, "pts")
        xys = [c for c in pts if isinstance(c, list) and c[0] == "xy"]
        if len(xys) < 2:
            continue
        a = snap(float(xys[0][1]), float(xys[0][2]))
        b = snap(float(xys[1][1]), float(xys[1][2]))
        parent.setdefault(a, a)
        parent.setdefault(b, b)
        union(a, b)
        segments.append((a, b))

    def on_segment(p, a, b, tol=2):
        # p on segment a-b (snapped 0.01mm ints), tolerance in grid units
        (px, py), (ax, ay), (bx, by) = p, a, b
        if not (min(ax, bx) - tol <= px <= max(ax, bx) + tol and
                min(ay, by) - tol <= py <= max(ay, by) + tol):
            return False
        cross = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
        seg_len = max(abs(bx - ax), abs(by - ay), 1)
        return abs(cross) <= tol * seg_len

    def attach_point(pt):
        """Union pt with every wire segment that passes through it."""
        parent.setdefault(pt, pt)
        for (a, b) in segments:
            if on_segment(pt, a, b):
                union(pt, a)

    # junctions merge crossing wires
    for j in find_all(tree, "junction"):
        at = child(j, "at")
        attach_point(snap(float(at[1]), float(at[2])))

    pin_records = []  # (instance, pin_number, pin_name, snapped_point)
    for inst in instances:
        if inst["lib_id"] not in lib_pins:
            continue  # power symbols etc. without an embedded lib entry
        for (num, name, px, py) in lib_pins[inst["lib_id"]]:
            dx, dy = pin_transform(px, py, inst["rot"], inst["mirror"])
            pt = snap(inst["x"] + dx, inst["y"] + dy)
            attach_point(pt)
            pin_records.append((inst, num, name, pt))

    # labels (local + global): attach to wires, join same-name labels
    label_points = defaultdict(list)
    for kind in ("global_label", "label", "hierarchical_label"):
        for gl in find_all(tree, kind):
            at = child(gl, "at")
            pt = snap(float(at[1]), float(at[2]))
            attach_point(pt)
            label_points[gl[1]].append(pt)
    for name, pts in label_points.items():
        for p in pts[1:]:
            union(pts[0], p)

    # The 1-pin connectors model the physical spring-clip contacts between
    # the row-strip PCBs and the main board: "TopOut" (strip side) mates
    # with "TopOutMAIN" (main board side), etc. Union each X / XMAIN pair.
    link_pts = {}
    for inst, num, name, pt in pin_records:
        if inst["lib_id"] == "Connector_Generic:Conn_01x01":
            link_pts[inst["value"]] = pt
    for val, pt in link_pts.items():
        if val.endswith("MAIN") and val[:-4] in link_pts:
            union(pt, link_pts[val[:-4]])

    return pin_records, label_points, find


# ---------------------------------------------------------------------------
# resistor tree walk
# ---------------------------------------------------------------------------

def build_table(sch_path):
    pin_records, label_points, find = netlist(sch_path)

    # resistor edges between nets
    graph = defaultdict(list)  # net -> [(other_net, ohms, ref)]
    resistors = defaultdict(dict)
    row_pads = {}          # row number (1..60) -> net
    conn15 = defaultdict(dict)   # 15-pin connector ref -> {pin: net}
    conn2 = defaultdict(dict)    # 2-pin connector ref -> {pin: net}
    sf_pin_nets = []       # nets hosting TP*/JP* pins (SF pad candidates)

    for inst, num, name, pt in pin_records:
        net = find(pt)
        lib = inst["lib_id"]
        ref = inst["ref"] or ""
        if "R_Small_US" in lib and ref.startswith("R"):
            try:
                ohms = parse_value(inst["value"])
            except ValueError:
                continue
            resistors[ref][num] = net
            resistors[ref]["ohms"] = ohms
        elif lib.endswith("topRowProbeContacts") or lib.endswith("bottomRowProbeContacts"):
            row_pads[int(num)] = net
        elif lib == "Connector_Generic:Conn_01x15":
            conn15[ref][int(num)] = net
        elif lib == "Connector_Generic:Conn_01x02":
            conn2[ref][int(num)] = net
        elif ref.startswith(("TP", "JP")):
            sf_pin_nets.append(net)

    for ref, r in resistors.items():
        if "1" in r and "2" in r:
            graph[r["1"]].append((r["2"], r["ohms"], ref))
            graph[r["2"]].append((r["1"], r["ohms"], ref))

    adc_pts = label_points.get("ADC_PROBE_SENSE")
    if not adc_pts:
        raise SystemExit("ADC_PROBE_SENSE label not found")
    sense_net = find(adc_pts[0])

    # Don't walk out through the supply/ground side (the 10K to GND, or any
    # accidental path through a rail).
    stop_nets = set()
    for name in ("GND", "+3V3", "+5V", "2.048_VREF"):
        for pt in label_points.get(name, []):
            stop_nets.add(find(pt))

    dist = {sense_net: 0.0}
    frontier = [sense_net]
    while frontier:
        nxt = []
        for net in frontier:
            for (other, ohms, ref) in graph[net]:
                if other in stop_nets:
                    continue
                if other not in dist:
                    dist[other] = dist[net] + ohms
                    nxt.append(other)
        frontier = nxt

    def pad_r(net, what):
        if net is None or net not in dist:
            raise SystemExit(f"{what} not reachable from ADC_PROBE_SENSE")
        return dist[net]

    # rows 1..60 straight off the contact-strip symbols
    rows = {n: pad_r(net, f"row {n}") for n, net in row_pads.items()}
    if sorted(rows) != list(range(1, 61)):
        raise SystemExit(f"expected row pads 1..60, got {sorted(rows)}")

    # rails: the two reachable 2-pin connectors, ordered by resistance.
    # The lower pair hangs off the row 1-30 strip (top rails), the higher
    # pair off the row 31-60 strip (bottom rails).
    rail_pairs = []
    for ref, pmap in conn2.items():
        rs = sorted(dist[net] for net in pmap.values() if net in dist)
        if len(rs) == 2:
            rail_pairs.append(rs)
    rail_pairs.sort(key=lambda p: p[0])
    if len(rail_pairs) != 2:
        raise SystemExit(f"expected 2 rail connectors, found {len(rail_pairs)}")
    top_rail, bottom_rail = rail_pairs

    # nano header: the two reachable 15-pin connectors; all 30 pin
    # resistances sorted ascending follow probeRowMap's nano order
    # (NANO_D1 first). Verified against the NanoHeaderInOrder pin names in
    # the design docs.
    nano_rs = []
    for ref, pmap in conn15.items():
        for pin, net in pmap.items():
            if net in dist:
                nano_rs.append(dist[net])
    nano_rs.sort()
    if len(nano_rs) != 30:
        raise SystemExit(f"expected 30 nano pads, found {len(nano_rs)}")

    # SF/logo/building pads: reachable TP*/JP* pins beyond the nano chain,
    # ascending = probeRowMap order 95..101
    sf_rs = sorted({dist[n] for n in sf_pin_nets if n in dist and dist[n] > nano_rs[-1]})
    if len(sf_rs) != 7:
        raise SystemExit(f"expected 7 SF pads beyond nano chain, found {sf_rs}")

    # assemble in probeRowMap order (index 0 is the out-of-range sentinel)
    table = [(-1.0, "sentinel")]
    for row in range(1, 31):
        table.append((rows[row], f"row {row}"))
    table.append((top_rail[0], "TOP_RAIL"))
    table.append((top_rail[1], "TOP_RAIL_GND"))
    table.append((bottom_rail[0], "BOTTOM_RAIL"))
    table.append((bottom_rail[1], "BOTTOM_RAIL_GND"))
    for row in range(31, 61):
        table.append((rows[row], f"row {row}"))
    nano_names = ["NANO_D1", "NANO_D0", "NANO_RESET_1", "NANO_GND_1",
                  "NANO_D2", "NANO_D3", "NANO_D4", "NANO_D5", "NANO_D6",
                  "NANO_D7", "NANO_D8", "NANO_D9", "NANO_D10", "NANO_D11",
                  "NANO_D12", "NANO_D13", "NANO_3V3", "NANO_AREF",
                  "NANO_A0", "NANO_A1", "NANO_A2", "NANO_A3", "NANO_A4",
                  "NANO_A5", "NANO_A6", "NANO_A7", "NANO_5V",
                  "NANO_RESET_0", "NANO_GND_0", "NANO_VIN"]
    for r, name in zip(nano_rs, nano_names):
        table.append((r, name))
    sf_names = ["LOGO_PAD_BOTTOM", "LOGO_PAD_TOP", "GPIO_PAD", "DAC_PAD",
                "ADC_PAD", "BUILDING_PAD_TOP", "BUILDING_PAD_BOTTOM"]
    for r, name in zip(sf_rs, sf_names):
        table.append((r, name))

    return table


# ---------------------------------------------------------------------------
# self-check + emit
# ---------------------------------------------------------------------------

def self_check(table):
    assert len(table) == 102, f"expected 102 entries, got {len(table)}"
    rs = [r for r, _ in table[1:]]
    for i in range(1, len(rs)):
        assert rs[i] > rs[i - 1], (
            f"ladder not monotonic at index {i + 1} "
            f"({table[i][1]}={rs[i - 1]} -> {table[i + 1][1]}={rs[i]})"
        )
    # expected-count model vs the legacy uniform map: mid-ladder deviation
    # should be small (the ladder was designed to linearize counts). A large
    # deviation means the connectivity walk mis-ordered something.
    RSENSE, RSRC, FS = 10000.0, 125.0, 4095.0
    counts = [FS * RSENSE / (RSENSE + r + RSRC) for r in rs]
    top, bottom = counts[0], counts[93]  # linear across the chain region
    band = (top - bottom) / 93.0
    worst = 0.0
    for i in range(94):  # chain region only; the pad tail is non-linear by design
        linear = top - band * i
        worst = max(worst, abs(counts[i] - linear) / band)
    print(f"self-check: monotonic ok; chain linearity worst deviation "
          f"{worst:.2f} bands (band = {band:.1f} counts)")
    assert worst < 2.0, "chain deviates >2 bands from linear - check extraction"
    return counts


def emit(table, out_path, sch_path):
    counts = self_check(table)
    lines = []
    lines.append("// GENERATED FILE - do not hand-edit.")
    lines.append("// Regenerate with: python3 scripts/generate_probe_ladder.py")
    lines.append(f"// Source: {os.path.basename(sch_path)} (probe pad ladder BOM)")
    lines.append("//")
    lines.append("// kProbeLadderOhms[i] = series resistance from pad i (probeRowMap")
    lines.append("// index) to the ADC_PROBE_SENSE node. Index 0 is the out-of-range")
    lines.append("// sentinel. Expected ADC5 ratio for pad i:")
    lines.append("//   r_i = 10K / (10K + kProbeLadderOhms[i] + Rsrc)")
    lines.append("#ifndef PROBE_LADDER_TABLE_H")
    lines.append("#define PROBE_LADDER_TABLE_H")
    lines.append("")
    lines.append("#define PROBE_LADDER_ENTRIES 102")
    lines.append("")
    lines.append("static const float kProbeLadderOhms[PROBE_LADDER_ENTRIES] = {")
    for i, (r, name) in enumerate(table):
        note = f" // [{i:3d}] {name}"
        if i > 0:
            note += f"  (~{counts[i-1]:.0f} counts nominal)"
        lines.append(f"    {r:>10.1f}f,{note}")
    lines.append("};")
    lines.append("")
    lines.append("#endif // PROBE_LADDER_TABLE_H")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {out_path}")


def main():
    sch = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SCH
    table = build_table(sch)
    emit(table, OUT_HEADER, sch)


if __name__ == "__main__":
    main()
