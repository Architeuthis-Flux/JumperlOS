#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Diff two JL_ROUTING_SNAP_DIR captures from test_routing_dense.py.

    JL_ROUTING_SNAP_DIR=/tmp/before python3 test/hil/test_routing_dense.py
    ... flash a router change ...
    JL_ROUTING_SNAP_DIR=/tmp/after  python3 test/hil/test_routing_dense.py
    python3 test/hil/tools/routing_snapshot_diff.py /tmp/before /tmp/after

Prints, per case, the path rows that changed (hop coordinates), the
crosspoints that appeared or vanished in the sent state, and the metrics.
A router change that only adds routes to previously unrouted paths shows
zero moved rows for every fully-routed case.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import routing_check as rc
from fabric_v5 import NODE_NAME


def rows_by_key(b_text):
    out = {}
    for r in rc.parse_paths(b_text):
        if r["net"] < 0:
            continue
        key = (r["dup"], NODE_NAME.get(r["node1"], r["node1"]),
               NODE_NAME.get(r["node2"], r["node2"]))
        out.setdefault(key, []).append(" ".join("%s%d.%d" % h for h in r["hops"]))
    return out


def main(before, after):
    names = sorted(f[:-len(".b.txt")] for f in os.listdir(before) if f.endswith(".b.txt"))
    moved_total = 0
    for name in names:
        pb = os.path.join(before, name + ".b.txt")
        pa = os.path.join(after, name + ".b.txt")
        if not os.path.exists(pa):
            print(f"{name}: missing in {after}")
            continue
        b1, b2 = open(pb).read(), open(pa).read()
        r1, r2 = rows_by_key(b1), rows_by_key(b2)
        m1, m2 = rc.summarize(rc.parse_paths(b1)), rc.summarize(rc.parse_paths(b2))
        moved = []
        for key in sorted(set(r1) | set(r2)):
            if r1.get(key) != r2.get(key):
                moved.append((key, r1.get(key), r2.get(key)))
        x1 = os.path.join(before, name + ".xbar.txt")
        x2 = os.path.join(after, name + ".xbar.txt")
        added = removed = set()
        if os.path.exists(x1) and os.path.exists(x2):
            s1 = rc.crosspoints_from_xbar(rc.parse_xbar(open(x1).read()))
            s2 = rc.crosspoints_from_xbar(rc.parse_xbar(open(x2).read()))
            added, removed = s2 - s1, s1 - s2
        flag = "" if not moved and not added and not removed else "  <-- changed"
        print(f"{name}: {m1} -> {m2}{flag}")
        for key, a, b in moved:
            dup = "dup " if key[0] else ""
            print(f"    {dup}{key[1]}-{key[2]}: {a} -> {b}")
        if added or removed:
            print(f"    crosspoints +{sorted(added)} -{sorted(removed)}")
        moved_total += len(moved)
    print(f"\n{moved_total} path rows moved across {len(names)} cases")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
