#!/usr/bin/env python3
"""IC identification HIL regression (DESIGN_IC_IDENTIFICATION.md 5.3 item 6).

Runs against the standing bench rig's 7447, DISCOVERED from its placed
record instead of hardcoded rows - bench layouts drift (this exact chip was
found living rotated 180 with pin 1 on row 10, which is half the reason the
orientation machinery exists). Needs: a placed dip16 part named 7447 with
its GND/VCC pins on real rows. Skips cleanly when absent.

Covers, over the MicroPython port:
  - Tier-1: part_fingerprint() reads the unpowered clamp map (substrate
    diodes to GND, nothing pin-above-VCC - the standard-TTL open-collector
    signature) and the partdb matcher ranks the 7447 above the 74595.
  - Tier-3: part_vectors() powers the chip (board-powered mode on this
    bench - the demo's physical wires feed it) and the truth-table runner
    passes exactly the 7447, rejecting the 74595 and reporting the rotated
    orientation.
  - Refusals: swapped rails try no candidates; a top-half anchor is a bad
    argument; neither leaves anything behind.
  - Teardown: the bridge set is identical before and after every call.
"""
import sys, os, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jl


def kv(line):
    return {t.split('=', 1)[0]: t.split('=', 1)[1]
            for t in line.split() if '=' in t}


def find_bench_7447():
    """-> (baseRow bottom-anchored, width, gndRow, vddRow) or None."""
    out = jl.jl_exec(
        "import jumperless as J\n"
        "for p in J.list_parts():\n"
        "    if p.get('name') != '7447' or not p.get('placed'):\n"
        "        continue\n"
        "    pins = p.get('pins', {})\n"
        "    if 'GND' not in pins or 'VCC' not in pins:\n"
        "        continue\n"
        "    nodes = [q['node'] for q in pins.values()]\n"
        "    base = min(n for n in nodes if n >= 31)\n"
        "    w = len(nodes) // 2\n"
        "    print('CHIP', base, w, pins['GND']['node'], pins['VCC']['node'])\n",
        timeout=60)
    m = re.search(r'CHIP (\d+) (\d+) (\d+) (\d+)', out)
    return tuple(int(x) for x in m.groups()) if m else None


def run_guarded(call, timeout):
    """Run one MP call with a bridge-set snapshot around it (the teardown
    contract: every exit path restores the fabric)."""
    code = (
        "import jumperless as J\n"
        "before = sorted(str(J.get_bridge(i)) for i in range(J.get_num_bridges()))\n"
        f"r = {call}\n"
        "after = sorted(str(J.get_bridge(i)) for i in range(J.get_num_bridges()))\n"
        "print('RESULT', r)\n"
        "print('BRIDGES', 'same' if before == after else 'CHANGED')\n"
    )
    out = jl.jl_exec(code, timeout=timeout)
    res = None
    bridges = None
    for line in out.splitlines():
        if line.startswith('RESULT '):
            res = line[len('RESULT '):].strip()
        elif line.startswith('BRIDGES '):
            bridges = line.split()[1]
    jl.check(res is not None, f"{call}: returned a result line")
    jl.check(bridges == 'same',
             f"{call}: bridge set identical before and after")
    return res or ""


def free_routable_gpios():
    """Lift any part wiring on RP_GPIO_1..8 (nodes 131-138) - the bench
    7-seg record owns all eight, and the vector runner needs drivers.
    Returns the (gpio, row) pairs lifted, for restore."""
    out = jl.jl_exec(
        "import jumperless as J\n"
        "lifted = []\n"
        "for p in J.list_parts():\n"
        "    for q in p.get('pins', {}).values():\n"
        "        c = q.get('connect', -1)\n"
        "        if isinstance(c, int) and 131 <= c <= 138:\n"
        "            J.disconnect(c, q['node'])\n"
        "            lifted.append((c, q['node']))\n"
        "print('LIFTED', lifted)\n",
        timeout=60)
    m = re.search(r'LIFTED (\[.*\])', out)
    return eval(m.group(1)) if m else []


def restore_gpios(pairs):
    if not pairs:
        return
    code = "import jumperless as J\n" + "".join(
        f"J.connect({g}, {r})\n" for g, r in pairs)
    jl.jl_exec(code, timeout=60)


def main():
    chip = find_bench_7447()
    if chip is None:
        jl.skip("no placed 7447 record on this bench - "
                "place one (Auto Scan or place_part) and rerun")
        return
    base, w, gnd, vdd = chip
    print(f"bench 7447: base {base} width {w} gnd {gnd} vdd {vdd}")

    lifted = free_routable_gpios()
    try:
        # ---- Tier-1: the unpowered clamp fingerprint --------------------
        fp_line = run_guarded(
            f"J.part_fingerprint({base}, {w}, {gnd}, {vdd})", timeout=180)
        d = kv(fp_line)
        jl.check(d.get('status') == '0', f"fingerprint status=0 (got {d.get('status')})")
        jl.check(int(d.get('probed', 0)) >= 2 * w - 4,
                 f"most pins probed (got {d.get('probed')}/{2 * w})")
        fp = d.get('fp', '')
        jl.check(len(fp) == 2 * w, f"fp is one char per pin (got {fp!r})")
        jl.check(fp.count('-') == 2, f"exactly the two rails marked (got {fp!r})")
        # the 7447's bench truth: everything conducts to GND only - at most
        # one die quirk (BI/RBO's 0.89V path to VCC) tolerated
        odd = sum(1 for c in fp if c not in 'G-')
        jl.check(odd <= 1, f"all-G within one quirk (got {fp!r})")
        matches = d.get('match', '')
        m47 = re.search(r'7447:(\d+)', matches)
        m595 = re.search(r'74595:(\d+)', matches)
        jl.check(m47 is not None, f"7447 in match list (got {matches!r})")
        jl.check(m595 is not None, f"74595 in match list (got {matches!r})")
        if m47 and m595:
            jl.check(int(m47.group(1)) < int(m595.group(1)),
                     f"7447 outranks 74595 ({matches})")

        # ---- Tier-3: the vectors name it --------------------------------
        vec_line = run_guarded(
            f"J.part_vectors({base}, {w}, {gnd}, {vdd})", timeout=300)
        d = kv(vec_line)
        jl.check(d.get('status') == '0', f"vectors status=0 (got {d.get('status')})")
        jl.check(d.get('pass') == '1',
                 f"exactly one candidate passes (got {vec_line!r})")
        cands = d.get('cands', '')
        jl.check(re.search(r'7447(\(r\))?:pass', cands) is not None,
                 f"the 7447 passes (got {cands!r})")
        jl.check(re.search(r'74595(\(r\))?:(fail|refused)', cands) is not None,
                 f"the 74595 does not pass (got {cands!r})")

        # ---- refusals ----------------------------------------------------
        # Swapped rails are geometrically identical to the flipped
        # orientation, so candidates DO match - but the pre-check reads the
        # claimed GND row, finds it live (the board's own feed), and
        # refuses before bridging anything. Nothing may pass.
        vec_line = run_guarded(
            f"J.part_vectors({base}, {w}, {vdd}, {gnd})", timeout=120)
        d = kv(vec_line)
        jl.check(d.get('pass') == '0',
                 f"swapped rails pass nothing (got {vec_line!r})")
        jl.check('refused' in d.get('cands', ''),
                 f"swapped rails refused at the live-GND guard (got {vec_line!r})")
        # a top-half anchor is a bad argument, refused before touching rows
        fp_line = run_guarded(
            f"J.part_fingerprint({base - 30}, {w}, {gnd}, {vdd})", timeout=60)
        jl.check(kv(fp_line).get('status') == '-1',
                 f"top-half anchor refused (got {fp_line!r})")
    finally:
        restore_gpios(lifted)

    jl.finish("ic_identify")


if __name__ == '__main__':
    main()
