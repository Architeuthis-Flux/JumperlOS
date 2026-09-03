# SPDX-License-Identifier: MIT
"""Routing verification model for the HIL suite.

Reads the router's view (`b` on port 1: bridge array, path table, chip status)
and the hardware view (`:crossbar` on port 7: lastChipXY, the crosspoints the
firmware last SENT), and checks them against the schematic-derived fabric in
fabric_v5.py with a union-find over the copper:

  * connectivity - every bridge's two nodes end up in one electrical component
  * no shorts     - no component carries nodes from two different nets, and no
                    component carries a real node (row, nano pin, rail, GND,
                    DAC/ADC, GPIO, ...) that is not in the net at all
  * sent == planned - the crosspoint set in the path table equals lastChipXY
  * bookkeeping   - every crosspoint a net's path closes is claimed for that
                    net in xStatus/yStatus, every claim sits on copper that is
                    really on that net, and every wire the net rides is claimed

Nothing here talks to the board; the parsers take the text the HIL helpers
already fetch, so the same model can run on a saved capture.
"""
import re

from fabric_v5 import X_NET, Y_NET, NODE_NET, NODE_NAME, SHORT_TO_NODE, CHIPS, LANES, BOUNCE

# wires with a second chip pin another net could close onto
FAR_END_WIRES = set(LANES) | set(BOUNCE)

NET_TOKEN = {"Gn": 1, "T": 2, "B": 3, "d0": 4, "d1": 5, "E": 0}


def net_of_token(tok):
    tok = tok.strip()
    if tok in NET_TOKEN:
        return NET_TOKEN[tok]
    return int(tok)


def node_of_name(name):
    name = name.strip()
    if re.fullmatch(r"-?\d+", name):
        return int(name)
    if name in SHORT_TO_NODE:
        return SHORT_TO_NODE[name]
    raise KeyError("unknown node name in b output: %r" % name)


def node_label(node):
    return NODE_NAME.get(node, str(node))


# ---------------------------------------------------------------------------
# parsers
# ---------------------------------------------------------------------------

def parse_bridge_array(text):
    """[(node1, node2, net), ...] from the 'Bridge Array' block of `b`."""
    out = []
    block = text.split("Bridge Array", 1)[1] if "Bridge Array" in text else ""
    block = block.split("Paths", 1)[0]
    for m in re.finditer(r"\[([^,\]]+),([^,\]]+),Net (\d+)\]", block):
        out.append((node_of_name(m.group(1)), node_of_name(m.group(2)),
                    int(m.group(3))))
    return out


def parse_paths(text):
    """Path rows of `b` as dicts: path, net, node1, node2, hops, alt, same,
    dup, ptype.  hops = [(chip, x, y), ...] for the up-to-four crosspoints
    the row carries (negative coordinates kept so the caller can tell an
    unrouted hop from a routed one)."""
    rows = []
    lines = text.splitlines()
    in_table = False
    for line in lines:
        s = line.rstrip("\r")
        if s.startswith("path\tnet\tnode1"):
            in_table = True
            continue
        if not in_table:
            continue
        if s.strip() == "" or s.strip() == "duplicates":
            continue
        if s.startswith("Chip Status") or s.startswith("chip\t"):
            break
        cols = [c.strip() for c in s.split("\t")]
        if len(cols) < 14 or not re.fullmatch(r"\d+", cols[0]):
            continue
        try:
            row = {
                "path": int(cols[0]),
                "net": net_of_token(cols[1]),
                "node1": node_of_name(cols[2]),
                "node2": node_of_name(cols[6]),
                "alt": int(cols[10]),
                "same": int(cols[11]),
                "dup": int(cols[12]),
                "ptype": cols[13],
            }
        except (ValueError, KeyError) as e:
            raise ValueError("cannot parse path row %r: %s" % (s, e))
        hops = [(cols[3], int(cols[4]), int(cols[5])),
                (cols[7], int(cols[8]), int(cols[9]))]
        extra = [c for c in cols[14:] if c != ""]
        if len(extra) >= 5:
            chip2 = extra[0]
            chip3 = extra[5] if len(extra) >= 6 else chip2
            hops.append((chip2, int(extra[1]), int(extra[2])))
            hops.append((chip3, int(extra[3]), int(extra[4])))
        row["hops"] = hops
        rows.append(row)
    return rows


def parse_chip_status(text):
    """{chip: (xStatus[16], yStatus[8])} with None for '.'."""
    out = {}
    block = text.split("Chip Status", 1)[1] if "Chip Status" in text else text
    for line in block.splitlines():
        m = re.match(r"^([A-L])\t(.*)$", line.rstrip("\r"))
        if not m:
            continue
        toks = m.group(2).split()
        if len(toks) != 24:
            raise ValueError("chip status row %r has %d tokens" % (line, len(toks)))
        conv = [None if t == "." else net_of_token(t) for t in toks]
        out[m.group(1)] = (conv[:16], conv[16:])
    return out


def parse_xbar(text):
    """{chip: [8 x 16-bit row masks]} from the port-7 ':crossbar' reply."""
    m = re.search(r"xbar\{12x8:([0-9A-Fa-f]{384})\}", text)
    if not m:
        raise ValueError("no xbar{12x8:...} in %r" % text[:200])
    hexs = m.group(1)
    out = {}
    for ci, chip in enumerate(CHIPS):
        rows = []
        for y in range(8):
            off = (ci * 8 + y) * 4
            rows.append(int(hexs[off:off + 4], 16))
        out[chip] = rows
    return out


def unrouted_lines(text):
    return [l.strip() for l in text.splitlines() if "Couldn't find a path" in l]


# ---------------------------------------------------------------------------
# crosspoint sets
# ---------------------------------------------------------------------------

def routed_hops(row):
    return [(c, x, y) for (c, x, y) in row["hops"]
            if c in CHIPS and x >= 0 and y >= 0]


def path_is_routed(row):
    """A primary path counts as routed when its first two hops are real and
    any bounce it declares is real too (couldntFindPath's rule)."""
    hops = row["hops"]
    for i, (c, x, y) in enumerate(hops[:3]):
        if i >= 2 and c not in CHIPS:
            continue
        if x < 0 or y < 0:
            return False
    return True


def crosspoints_from_paths(rows, include_dups=True):
    """{(chip, x, y): set(nets)} over every routed hop."""
    out = {}
    for r in rows:
        if r["net"] < 0:
            continue                      # a culled duplicate
        if not include_dups and r["dup"]:
            continue
        for h in routed_hops(r):
            out.setdefault(h, set()).add(r["net"])
    return out


def crosspoints_from_xbar(xbar):
    out = set()
    for chip, rows in xbar.items():
        for y, mask in enumerate(rows):
            for x in range(16):
                if mask & (1 << x):
                    out.add((chip, x, y))
    return out


# ---------------------------------------------------------------------------
# electrical model
# ---------------------------------------------------------------------------

class UnionFind:
    def __init__(self):
        self.p = {}

    def find(self, a):
        self.p.setdefault(a, a)
        while self.p[a] != a:
            self.p[a] = self.p[self.p[a]]
            a = self.p[a]
        return a

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[ra] = rb


def components(crosspoints):
    """Union-find over KiCad nets: closing (chip, x, y) joins the X pin's net
    with the Y pin's net.  Returns {root: set(net names)}."""
    uf = UnionFind()
    for (chip, x, y) in crosspoints:
        uf.union(X_NET[chip][x], Y_NET[chip][y])
    comps = {}
    for net in list(uf.p):
        comps.setdefault(uf.find(net), set()).add(net)
    return uf, comps


def net_membership(bridges):
    """{node: net} and {net: set(nodes)} from the bridge array."""
    node_net = {}
    net_nodes = {}
    for n1, n2, net in bridges:
        for n in (n1, n2):
            node_net[n] = net
            net_nodes.setdefault(net, set()).add(n)
    return node_net, net_nodes


class Verdict:
    def __init__(self):
        self.problems = []
        self.notes = []

    def bad(self, msg):
        self.problems.append(msg)

    def ok(self):
        return not self.problems


def check_electrical(crosspoints, bridges, verdict):
    """Connectivity and no-shorts over a crosspoint set (planned or sent)."""
    uf, comps = components(crosspoints)
    node_net, net_nodes = net_membership(bridges)
    wire_owner = {}                       # KiCad net name -> node id
    for node, wire in NODE_NET.items():
        wire_owner.setdefault(wire, []).append(node)

    # connectivity
    for n1, n2, net in bridges:
        w1, w2 = NODE_NET.get(n1), NODE_NET.get(n2)
        if w1 is None or w2 is None:
            verdict.bad("bridge %s-%s: a node is not on the fabric model"
                        % (node_label(n1), node_label(n2)))
            continue
        if uf.find(w1) != uf.find(w2):
            verdict.bad("NOT CONNECTED: %s-%s (net %d)"
                        % (node_label(n1), node_label(n2), net))

    # shorts / strays: real nodes inside each component
    for root, wires in comps.items():
        nodes = [n for w in wires for n in wire_owner.get(w, [])]
        nets = {node_net[n] for n in nodes if n in node_net}
        strays = [n for n in nodes if n not in node_net]
        if len(nets) > 1:
            verdict.bad("SHORT: nets %s share copper via %s"
                        % (sorted(nets), sorted(node_label(n) for n in nodes)))
        if nets and strays:
            # GND on both K.X15 and L.X15 is one net name, never a stray; a
            # stray is a node no bridge mentions that the copper reaches.
            verdict.bad("STRAY: net %s reaches %s which is in no net"
                        % (sorted(nets), sorted(node_label(n) for n in strays)))
    return comps


def check_chip_local(crosspoints, verdict):
    """On one chip every closed crosspoint on an X column (or a Y row) must be
    the same net - two nets on one column is a short before any copper math."""
    cp = crosspoints if isinstance(crosspoints, dict) else {c: set() for c in crosspoints}
    by_col, by_row = {}, {}
    for (chip, x, y), nets in cp.items():
        by_col.setdefault((chip, x), set()).update(nets)
        by_row.setdefault((chip, y), set()).update(nets)
    for (chip, x), nets in by_col.items():
        if len(nets) > 1:
            verdict.bad("SHORT on chip %s X%d: nets %s" % (chip, x, sorted(nets)))
    for (chip, y), nets in by_row.items():
        if len(nets) > 1:
            verdict.bad("SHORT on chip %s Y%d: nets %s" % (chip, y, sorted(nets)))


def check_sent_matches_planned(planned, sent, verdict):
    p = set(planned)
    if p != sent:
        for h in sorted(p - sent):
            verdict.bad("PLANNED but not SENT: chip %s x%d y%d" % h)
        for h in sorted(sent - p):
            verdict.bad("SENT but not PLANNED: chip %s x%d y%d" % h)


def check_bookkeeping(rows, status, bridges, verdict, strict_claims=False):
    """xStatus/yStatus against the paths and the copper they actually ride."""
    planned = crosspoints_from_paths(rows)
    uf, comps = components(planned)
    node_net, _ = net_membership(bridges)
    # which component is which net (by the real nodes it holds)
    comp_net = {}
    for root, wires in comps.items():
        nets = set()
        for w in wires:
            for node, wire in NODE_NET.items():
                if wire == w and node in node_net:
                    nets.add(node_net[node])
        if len(nets) == 1:
            comp_net[root] = nets.pop()

    # 1. every closed crosspoint is claimed for its net on both axes
    for (chip, x, y), nets in planned.items():
        xs, ys = status[chip]
        for net in nets:
            if xs[x] != net:
                verdict.bad("UNCLAIMED X: chip %s x%d closed for net %d, xStatus says %s"
                            % (chip, x, net, xs[x]))
            if ys[y] != net:
                verdict.bad("UNCLAIMED Y: chip %s y%d closed for net %d, yStatus says %s"
                            % (chip, y, net, ys[y]))

    # 2. every claim sits on copper that really carries that net, and every
    #    wire a net rides is claimed at every chip pin it touches
    ridden = {}                            # (chip, 'x'|'y', idx) -> net
    for root, wires in comps.items():
        net = comp_net.get(root)
        if net is None:
            continue
        for chip in CHIPS:
            for x in range(16):
                if X_NET[chip][x] in wires:
                    ridden[(chip, "x", x)] = net
            for y in range(8):
                if Y_NET[chip][y] in wires:
                    ridden[(chip, "y", y)] = net
    for chip in CHIPS:
        xs, ys = status[chip]
        for x in range(16):
            claimed = xs[x]
            real = ridden.get((chip, "x", x))
            if claimed is not None and real is not None and claimed != real:
                verdict.bad("WRONG CLAIM: chip %s x%d claimed for net %s but its wire %s carries net %d"
                            % (chip, x, claimed, X_NET[chip][x], real))
            elif claimed is not None and real is None:
                (verdict.bad if strict_claims else verdict.notes.append)(
                    "CLAIM WITHOUT COPPER: chip %s x%d claimed for net %s but %s carries nothing"
                    % (chip, x, claimed, X_NET[chip][x]))
            elif claimed is None and real is not None and X_NET[chip][x] in FAR_END_WIRES:
                # a lane (two chip pins) or a bounce row that a net rides
                # must be claimed at BOTH pins or another net can close the
                # far end onto it; a node pin (row, GND's second pin, ...)
                # can only ever be claimed for its own node
                verdict.bad("UNCLAIMED WIRE: chip %s x%d (%s) carries net %d but xStatus is free"
                            % (chip, x, X_NET[chip][x], real))
        for y in range(8):
            claimed = ys[y]
            real = ridden.get((chip, "y", y))
            if claimed is not None and real is not None and claimed != real:
                verdict.bad("WRONG CLAIM: chip %s y%d claimed for net %s but its wire %s carries net %d"
                            % (chip, y, claimed, Y_NET[chip][y], real))
            elif claimed is not None and real is None:
                (verdict.bad if strict_claims else verdict.notes.append)(
                    "CLAIM WITHOUT COPPER: chip %s y%d claimed for net %s but %s carries nothing"
                    % (chip, y, claimed, Y_NET[chip][y]))
            elif claimed is None and real is not None and Y_NET[chip][y] in FAR_END_WIRES:
                verdict.bad("UNCLAIMED WIRE: chip %s y%d (%s) carries net %d but yStatus is free"
                            % (chip, y, Y_NET[chip][y], real))


def summarize(rows):
    """Small routing metrics for the log: primaries, bounces, duplicates."""
    prim = [r for r in rows if r["dup"] == 0 and r["net"] >= 0]
    dups = [r for r in rows if r["dup"] != 0 and r["net"] >= 0]
    bounced = [r for r in prim if len(routed_hops(r)) > 2]
    return {
        "primaries": len(prim),
        "routed_primaries": sum(1 for r in prim if path_is_routed(r)),
        "bounced": len(bounced),
        "routed_duplicates": sum(1 for r in dups if path_is_routed(r)),
        "crosspoints": len(crosspoints_from_paths(rows)),
    }


def full_check(b_text, xbar_text, expect_all_routed=True, strict_claims=False):
    """Run every check on one capture. Returns (Verdict, metrics).

    strict_claims: a chip-status claim on copper no net rides is a failure
    (true when the case has no duplicates - a culled duplicate leaves its
    claims behind, which is harmless within one rebuild but not a bug).
    """
    v = Verdict()
    bridges = parse_bridge_array(b_text)
    rows = parse_paths(b_text)
    status = parse_chip_status(b_text)
    xbar = parse_xbar(xbar_text)
    if not bridges:
        v.bad("no bridge array in the b output")
        return v, {}
    if len(status) != 12:
        v.bad("chip status has %d chips" % len(status))
        return v, {}

    for line in unrouted_lines(b_text):
        if expect_all_routed:
            v.bad(line)
        else:
            v.notes.append(line)
    # the path table's own view of unrouted primaries
    for r in rows:
        if r["dup"] == 0 and r["net"] >= 0 and not path_is_routed(r):
            msg = "unrouted primary path %d: %s-%s (net %d)" % (
                r["path"], node_label(r["node1"]), node_label(r["node2"]), r["net"])
            (v.bad if expect_all_routed else v.notes.append)(msg)

    planned = crosspoints_from_paths(rows)
    sent = crosspoints_from_xbar(xbar)
    check_chip_local(planned, v)
    check_sent_matches_planned(planned, sent, v)
    # electrical checks on what was SENT (the hardware truth) and, separately,
    # on what was planned (so a planned/sent mismatch is not double counted)
    if expect_all_routed:
        check_electrical(sent, bridges, v)
    else:
        # only shorts/strays matter when the case is allowed to leave paths
        # unrouted: connectivity failures are already listed above
        vv = Verdict()
        check_electrical(sent, bridges, vv)
        for p in vv.problems:
            if not p.startswith("NOT CONNECTED"):
                v.bad(p)
    check_bookkeeping(rows, status, bridges, v, strict_claims)
    return v, summarize(rows)
