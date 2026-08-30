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


# kRailVfLo / kRailVfHi in PartsApp.cpp: one silicon junction at 1mA. The
# rail resolver keeps the (gnd, vdd) pair whose GND side lands in this band.
JUNCTION_LO, JUNCTION_HI = 0.40, 1.10


def pin_drops(line):
    """pins=row:<gndCode><vddCode>:vfGnd:vfVdd,... -> [(row, code, vf), ...]
    for the GND side only (codes: o open, j junction, r resistive tie)."""
    m = re.search(r'pins=(\S+)', line)
    out = []
    if not m:
        return out
    for field in m.group(1).split(','):
        parts = field.split(':')
        if len(parts) != 4:
            continue
        out.append((int(parts[0]), parts[1][0], float(parts[2])))
    return out


def lift_rows(rows):
    """Briefly unwire everything touching these rows (the chip's supply
    feeds, so an unpowered measurement is actually unpowered). Returns the
    (node1, node2) pairs for restore."""
    code = ("import jumperless as J\n"
            f"rows = {list(rows)}\n"
            "lift = []\n"
            "for i in range(J.get_num_bridges()):\n"
            "    b = J.get_bridge(i)\n"
            "    if b and (b[0] in rows or b[1] in rows):\n"
            "        lift.append((b[0], b[1]))\n"
            "for a, c in lift:\n"
            "    J.disconnect(a, c)\n"
            "print('LIFT', lift)\n")
    out = jl.jl_exec(code, timeout=60)
    m = re.search(r'LIFT (\[.*\])', out)
    return eval(m.group(1)) if m else []


def restore_bridges(pairs):
    if not pairs:
        return
    code = "import jumperless as J\n" + "".join(
        f"J.connect({a}, {b})\n" for a, b in pairs)
    jl.jl_exec(code, timeout=60)


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


def find_bench_capacitor():
    """-> (rowA, rowB) of a placed two-leg capacitor record, or None."""
    out = jl.jl_exec(
        "import jumperless as J\n"
        "for p in J.list_parts():\n"
        "    if p.get('type') != 'capacitor' or not p.get('placed'):\n"
        "        continue\n"
        "    nodes = sorted(q['node'] for q in p.get('pins', {}).values())\n"
        "    if len(nodes) == 2 and all(1 <= n <= 60 for n in nodes):\n"
        "        print('CAP', nodes[0], nodes[1])\n"
        "        break\n",
        timeout=60)
    m = re.search(r'CAP (\d+) (\d+)', out)
    return (int(m.group(1)), int(m.group(2))) if m else None


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

    # ---- a blocked measurement lane still measures -----------------------
    # FIRST, on the fully-wired board: ADC0-3, both DACs, GND and both rails
    # all hang off crossbar chip K, each breadboard chip has ONE direct lane
    # to it, and the bounce routes go through chip L - which the 7-seg's
    # eight GPIO bridges fill. Lifting those (free_routable_gpios below)
    # opens the bounce and the conflict disappears, so this check only means
    # anything before it. Bench, 2026-08-28: C12 on rows 12/42 refused
    # ("no path for 42 to ADC_1") beside the 7447's row-40 rail feed, and
    # the session reported "busy" for a board that would never get less so.
    near = vdd + 2 if vdd + 2 <= 58 else vdd - 2
    if 31 <= near <= 58 and near not in (gnd, vdd):
        id_line = run_guarded(f"J.part_identify({near - 30}, {near})",
                              timeout=120)
        st = kv(id_line).get('status', '0')
        jl.check(st != '-7',
                 f"rows {near - 30}/{near} measurable beside the row-{vdd} "
                 f"rail feed and a full GPIO bank (status={st})")

    # ---- a capacitor comes back with a VALUE ----------------------------
    # Kevin's session log, 2026-08-28: C12 identified as "CAPACITOR
    # conf=0.00" with no value - the fake-out branch never measured and the
    # no-conduct branch only ever watched the INA decay. partScanCapMeasure
    # now times a pull-down decay (or integrates the hard-loop charge) and
    # both branches report farads in value= with a real confidence.
    cap = find_bench_capacitor()
    if cap is None:
        print("  (no placed capacitor on this bench - value check skipped)")
    else:
        id_line = run_guarded(f"J.part_identify({cap[0]}, {cap[1]})",
                              timeout=120)
        d = kv(id_line)
        jl.check(d.get('type') == 'CAPACITOR',
                 f"rows {cap[0]}/{cap[1]} read CAPACITOR (got {id_line!r})")
        if d.get('type') == 'CAPACITOR':
            val = float(d.get('value', 0) or 0)
            jl.check(1e-9 < val < 1e-1,
                     f"capacitance measured, plausible farads (value={val})")
            jl.check(float(d.get('conf', 0) or 0) >= 0.5,
                     f"a measured cap is not conf=0.00 (conf={d.get('conf')})")

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
        # 2026-08-30 database: dozens of records carry generous 'C'
        # (conducts-somehow) maps that "match" an all-G chip at 0 raw
        # mismatches, and the all-B 74595 (14 real misses) legitimately
        # drops out of the top 3. The ranking is evidence-weighted (a
        # wildcard match costs a quarter of a real mismatch), so the
        # 7447's specific all-G map must LEAD the list even on a
        # die-quirk day.
        matches = d.get('match', '')
        m47 = re.match(r'7447:(\d+)r?', matches)
        jl.check(m47 is not None,
                 f"the 7447 leads the match list (got {matches!r})")
        if m47:
            jl.check(int(m47.group(1)) <= 1,
                     f"7447 within one quirk mismatch ({matches})")

        # ---- the rail resolver's premise --------------------------------
        # WHICH row is ground cannot come from the fingerprint STRING: on
        # this chip the correct pair and its swap print the same one. It
        # comes from the DROP - one substrate diode vs a junction chain -
        # and getting it wrong would have the vector runner drive GND onto
        # the supply pin. partsResolveChipRails scores exactly this band.
        conducting = [(r, vf) for r, code, vf in pin_drops(fp_line)
                      if code == 'j']
        jl.check(len(conducting) >= 2 * w - 4,
                 f"most pins conduct to the real ground row "
                 f"(got {len(conducting)}/{2 * w})")
        out_of_band = [(r, vf) for r, vf in conducting
                       if not (JUNCTION_LO <= vf <= JUNCTION_HI)]
        jl.check(len(out_of_band) <= 1,
                 f"real ground reads ONE junction per pin, "
                 f"{JUNCTION_LO}-{JUNCTION_HI}V (outliers: {out_of_band})")

        swap_line = run_guarded(
            f"J.part_fingerprint({base}, {w}, {vdd}, {gnd})", timeout=180)
        d = kv(swap_line)
        jl.check(d.get('status') == '0',
                 f"swapped-rail fingerprint runs (got {d.get('status')})")
        jl.check(d.get('fp') == fp,
                 f"the fp STRING cannot referee the swap: {d.get('fp')!r} "
                 f"vs {fp!r} - only the drop can")
        swap_conducting = [(r, vf) for r, code, vf in pin_drops(swap_line)
                           if code == 'j']
        in_band = [(r, vf) for r, vf in swap_conducting
                   if JUNCTION_LO <= vf <= JUNCTION_HI]
        jl.check(swap_conducting and not in_band,
                 f"the swapped pair reads a CHAIN, out of the junction band, "
                 f"so the resolver rejects it (in-band: {in_band})")

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
        bad_line = run_guarded(
            f"J.part_fingerprint({base - 30}, {w}, {gnd}, {vdd})", timeout=60)
        jl.check(kv(bad_line).get('status') == '-1',
                 f"top-half anchor refused (got {bad_line!r})")

        # ---- the chip's own pins are never a discrete resistor -----------
        # Two adjacent signal pins of an unpowered TTL chip either read
        # nothing or conduct ONE way through a junction chain (~1.5-2.2V at
        # 1mA - bench: pins on rows 6/7 and 7/8 of this chip). Only a
        # conducting pair proves anything, so walk until one conducts. The
        # 50uA leg of the junction-vs-resistor ratio law lives inside this
        # fabric's transient floor, so a conducting pair used to come back
        # "RESISTOR 2" (kohms printed into an ohms field) - a phantom
        # two-leg part INSIDE the chip, which the Auto scan then split out
        # and offered for placement, stealing two of the chip's legs.
        rows_free = sorted(r for r, _c, _v in pin_drops(fp_line)
                           if r not in (gnd, vdd) and r <= 30)
        rail_lift = lift_rows([gnd, vdd])   # an unpowered read, actually
        try:
            found = None
            for r in rows_free:
                if r + 1 not in rows_free:
                    continue
                d = kv(run_guarded(f"J.part_identify({r}, {r + 1})",
                                   timeout=120))
                if d.get('status', '0') != '0':
                    continue
                if d.get('type') in ('EMPTY', 'UNKNOWN') and \
                        float(d.get('value', 0) or 0) == 0.0:
                    continue        # nothing between these two pins
                found = ((r, r + 1), d)
                break
            if found is None:
                print("  (no conducting adjacent pin pair - nothing to prove)")
            else:
                pair, d = found
                print(f"  conducting pin pair {pair}: {d.get('type')} "
                      f"{d.get('value')}")
                jl.check(d.get('type') not in ('RESISTOR', 'SHORT'),
                         f"pins {pair} are not a discrete resistor "
                         f"(got type={d.get('type')} value={d.get('value')})")
        finally:
            restore_bridges(rail_lift)
    finally:
        restore_gpios(lifted)

    jl.finish("ic_identify")


if __name__ == '__main__':
    main()
