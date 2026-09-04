#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate test/hil/fabric_v5.py - the Jumperless V5 crossbar fabric as the
KiCad schematic wires it, independent of the firmware's own tables.

    python3 test/hil/tools/gen_fabric_v5.py [schematic.kicad_sch | netlist.xml]

Default input: the r7 mainboard schematic in the JumperlessV5 hardware repo.
A .kicad_sch is exported with kicad-cli (KiCad 8+); an .xml is a netlist you
already exported with `kicad-cli sch export netlist --format kicadxml`.

The generated module is what test_routing_dense.py's electrical model runs on:
every CH446Q X/Y pin's net name, plus the firmware node-id -> net-name table
that lives HERE (KiCad knows nothing about node ids). Regenerate after a board
revision and diff the module; a changed lane shows up as a changed line.
"""
import collections
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

DEFAULT_SCH = os.path.expanduser(
    "~/Documents/GitHub/JumperlessV5/Jumperless23V50/MainBoard/JumperlessV5r7/"
    "JumperlessV5r7.kicad_sch")
KICAD_CLI = [
    "kicad-cli",
    "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
]

# Firmware node ids (src/JumperlessDefines.h) -> KiCad net name on the crossbar.
# Rows 1..60 map to the nets "1".."60" and are added programmatically.
NODE_TABLE = {
    # nano header (NANO_D0 = 70 .. NANO_D13 = 83, NANO_AREF = 85, NANO_A0 = 86 .. A7 = 93)
    **{70 + i: ("D%d" % i, "D%d" % i) for i in range(14)},
    85: ("AREF", "AREF"),
    **{86 + i: ("A%d" % i, "A%d" % i) for i in range(8)},
    100: ("GND", "GND"),
    101: ("TOP_RAIL", "TOP_RAIL"),
    102: ("BOTTOM_RAIL", "BOTTOM_RAIL"),
    106: ("DAC0", "DAC_0"),
    107: ("DAC1", "DAC_1"),
    108: ("ISENSE_PLUS", "Net-(I1-X11)"),    # JP5 default: CURR_SENSE+ on I.X11
    109: ("ISENSE_MINUS", "Net-(J6-X11)"),   # JP6 default: CURR_SENSE- on J.X11
    110: ("ADC0", "ADC_UNBUFFERED_+-8V_0"),
    111: ("ADC1", "ADC_UNBUFFERED_+-8V_1"),
    112: ("ADC2", "ADC_UNBUFFERED_+-8V_2"),
    113: ("ADC3", "ADC_UNBUFFERED_+-8V_3"),
    114: ("ADC4", "ADC_UNBUFFERED_5V_4"),
    116: ("UART_TX", "UART_Tx"),
    117: ("UART_RX", "UART_Rx"),
    **{131 + i: ("GPIO_%d" % (i + 1), "GPIO_%d" % (20 + i)) for i in range(8)},
    139: ("BUFFER_IN", "ROUTABLE_BUFFER_IN"),
    140: ("BUFFER_OUT", "ROUTABLE_BUFFER_OUT"),
}
for row in range(1, 61):
    NODE_TABLE[row] = (str(row), str(row))

# The short names the firmware prints in the `b` bridge array / path table
# (NetManager.cpp defSpecialToCharShort / defNanoToCharShort) -> node id.
SHORT_NAMES = {
    "GND": 100, "TOP_R": 101, "BOT_R": 102, "DAC_0": 106, "DAC_1": 107,
    "I_POS": 108, "I_NEG": 109, "ADC_0": 110, "ADC_1": 111, "ADC_2": 112,
    "ADC_3": 113, "ADC_4": 114, "UART_Tx": 116, "UART_Rx": 117,
    "BUF_IN": 139, "BUF_OUT": 140, "AREF": 85,
    **{"GP_%d" % (i + 1): 131 + i for i in range(8)},
    **{"D%d" % i: 70 + i for i in range(14)},
    **{"A%d" % i: 86 + i for i in range(8)},
}


def export_netlist(sch):
    for cli in KICAD_CLI:
        if os.path.exists(cli) or cli == "kicad-cli":
            out = os.path.join(tempfile.mkdtemp(), "netlist.xml")
            try:
                subprocess.run([cli, "sch", "export", "netlist", "--format",
                                "kicadxml", "-o", out, sch], check=True,
                               capture_output=True)
                return out
            except (FileNotFoundError, subprocess.CalledProcessError):
                continue
    sys.exit("kicad-cli not found or the export failed; pass a netlist .xml")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SCH
    xml_path = src if src.endswith(".xml") else export_netlist(src)
    root = ET.parse(xml_path).getroot()
    comps = {c.get("ref"): c.findtext("value") for c in root.iter("comp")}
    chips = {ref for ref, v in comps.items() if v and "CH446" in v}
    letter = {ref: ref[0] for ref in chips}  # A2 -> A, K1 -> K ...
    if sorted(letter.values()) != list("ABCDEFGHIJKL"):
        sys.exit("expected exactly twelve CH446Q refs A..L, got %s" % sorted(chips))

    pins = collections.defaultdict(dict)   # chip -> {"X3": net}
    net_pins = collections.defaultdict(list)
    for n in root.iter("net"):
        name = n.get("name")
        for node in n.iter("node"):
            ref = node.get("ref")
            if ref not in chips:
                continue
            tag = (node.get("pinfunction") or "").split("_")[0]
            if tag and tag[0] in "XY" and tag[1:].isdigit():
                pins[letter[ref]][tag] = name
                net_pins[name].append((letter[ref], tag))

    x_net = {c: [pins[c]["X%d" % i] for i in range(16)] for c in "ABCDEFGHIJKL"}
    y_net = {c: [pins[c]["Y%d" % i] for i in range(8)] for c in "ABCDEFGHIJKL"}
    all_nets = set(net_pins)

    # Every node's net must exist on the fabric, on exactly one pin (GND: two).
    for node, (name, net) in sorted(NODE_TABLE.items()):
        if net not in all_nets:
            sys.exit("node %s (%d): net %r is not on any CH446Q pin" % (name, node, net))
        n = len(net_pins[net])
        if n != (2 if net == "GND" else 1):
            sys.exit("node %s: net %r touches %d chip pins" % (name, net, n))

    node_nets = {net for _, net in NODE_TABLE.values()}
    lanes = sorted(n for n, p in net_pins.items()
                   if len(p) >= 2 and n not in node_nets)
    bounce = sorted(n for n in all_nets if n.startswith("unconnected-"))
    stray = sorted(all_nets - node_nets - set(lanes) - set(bounce))
    if stray:
        sys.exit("fabric pins on nets that are neither nodes, lanes nor NC: %s" % stray)

    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "fabric_v5.py")
    with open(out, "w") as f:
        w = f.write
        w("# SPDX-License-Identifier: MIT\n")
        w("# GENERATED by test/hil/tools/gen_fabric_v5.py from the V5 r7 KiCad schematic.\n")
        w("# Do not edit by hand - regenerate after a board revision and diff.\n")
        w('"""Jumperless V5 crossbar fabric as the schematic wires it.\n\n')
        w("X_NET[chip][x] / Y_NET[chip][y]: the KiCad net on that CH446Q pin.\n")
        w("NODE_NET: firmware node id -> net name.  NODE_NAME: node id -> name.\n")
        w("SHORT_TO_NODE: the short names the `b` command prints -> node id.\n")
        w("LANES: inter-chip wires (each touches two chip pins).  BOUNCE: the\n")
        w("no-connect Y0 rows of chips A..H (the router's bounce bus).\n")
        w('"""\n\n')
        w('CHIPS = "ABCDEFGHIJKL"\n\n')
        w("X_NET = {\n")
        for c in "ABCDEFGHIJKL":
            w("    %r: %r,\n" % (c, x_net[c]))
        w("}\n\nY_NET = {\n")
        for c in "ABCDEFGHIJKL":
            w("    %r: %r,\n" % (c, y_net[c]))
        w("}\n\nNODE_NET = {\n")
        for node, (name, net) in sorted(NODE_TABLE.items()):
            w("    %d: %r,\n" % (node, net))
        w("}\n\nNODE_NAME = {\n")
        for node, (name, net) in sorted(NODE_TABLE.items()):
            w("    %d: %r,\n" % (node, name))
        w("}\n\nSHORT_TO_NODE = {\n")
        for name, node in sorted(SHORT_NAMES.items(), key=lambda kv: kv[1]):
            w("    %r: %d,\n" % (name, node))
        w("}\n\nLANES = %r\n\n" % lanes)
        w("BOUNCE = %r\n" % bounce)
    print("wrote", out, "-", len(lanes), "lanes,", len(NODE_TABLE), "nodes")


if __name__ == "__main__":
    main()
