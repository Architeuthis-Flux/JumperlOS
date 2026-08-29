#!/usr/bin/env python3
"""Part-identification HIL regression (DESIGN_PART_ID_FOLLOWUP §12).

Runs against the standing bench rig: the 2N3906 on rows 17/18/19 and the
7400 on rows 10-16/40-46 (slot 3). Everything drives part_identify() over
the MicroPython port - the scan session lifts and restores user wiring
itself, and the tests assert that restoration bridge-for-bridge.

Fixture-free cases (EMPTY rows, arg refusals) run on any board.
"""
import sys, os, re, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jl


def parse_id(line):
    """'type=BJT_PNP conf=0.90 ... rows=17,18,19 roles=E,B,C ...' -> dict"""
    out = {}
    for tok in line.split():
        if '=' not in tok:
            continue
        k, v = tok.split('=', 1)
        out[k] = v
    return out


def device_id(rows, timeout=240):
    args = ", ".join(str(r) for r in rows)
    code = (
        "import jumperless as J\n"
        "before = sorted(str(J.get_bridge(i)) for i in range(J.get_num_bridges()))\n"
        f"r = J.part_identify({args})\n"
        "after = sorted(str(J.get_bridge(i)) for i in range(J.get_num_bridges()))\n"
        "print('RESULT', r)\n"
        "print('BRIDGES', 'same' if before == after else 'CHANGED')\n"
    )
    out = jl.jl_exec(code, timeout=timeout)
    res_line = None
    bridges = None
    for line in out.splitlines():
        if line.startswith('RESULT '):
            res_line = line[len('RESULT '):].strip()
        elif line.startswith('BRIDGES '):
            bridges = line.split()[1]
    jl.check(res_line is not None, "part_identify returned a result line")
    jl.check(bridges == 'same', "bridge set identical before and after (lift restored)")
    return parse_id(res_line)


def main():
    # ---- the placed 2N3906, all three tap orders --------------------------
    # The part's physical orientation is the user's business (it has been
    # reseated on this bench) - what the firmware must promise is: PNP, base
    # on row 18, E and C on rows 17/19, and the SAME physical assignment
    # whatever order the rows are tapped in.
    first_map = None
    for order in ((17, 18, 19), (19, 17, 18), (18, 19, 17)):
        r = device_id(order)
        jl.check(r.get('type') == 'BJT_PNP',
                 f"{order}: BJT_PNP (got {r.get('type')})")
        jl.check(float(r.get('conf', 0)) >= 0.8,
                 f"{order}: confidence >= 0.8 (got {r.get('conf')})")
        rows = [int(x) for x in r.get('rows', '').split(',')]
        roles = r.get('roles', '').split(',')
        byrow = dict(zip(rows, roles))
        jl.check(byrow.get(18) == 'B', f"{order}: base on row 18 (got {byrow})")
        jl.check(sorted((byrow.get(17), byrow.get(19))) == ['C', 'E'],
                 f"{order}: E and C on rows 17/19 (got {byrow})")
        if first_map is None:
            first_map = byrow
        else:
            jl.check(byrow == first_map,
                     f"{order}: assignment consistent across tap orders")
        vbe = float(r.get('value', 0))
        jl.check(0.45 <= vbe <= 0.85, f"{order}: Vbe plausible (got {vbe})")
        hfe = float(r.get('value2', 0))
        jl.check(50 <= hfe <= 2000, f"{order}: hFE plausible (got {hfe})")

    # ---- empty rows -------------------------------------------------------
    r = device_id((25, 26))
    jl.check(r.get('type') == 'EMPTY', f"empty pair: EMPTY (got {r.get('type')})")
    r = device_id((24, 25, 26))
    jl.check(r.get('type') == 'EMPTY', f"empty trio: EMPTY (got {r.get('type')})")

    # ---- the 7400's ESD network (gnd row 46, input row 40, vcc row 10) ----
    r = device_id((46, 40, 10))
    jmap = [float(x) for x in r.get('map', '0').split(',')]
    jl.check(len(jmap) == 9, "ESD run returned a 3x3 map")
    if len(jmap) == 9:
        jl.check(jmap[1] < 1.1, f"GND->input ESD diode conducts (got {jmap[1]})")
        jl.check(jmap[2] < 1.1, f"GND->VCC substrate diode conducts (got {jmap[2]})")
        jl.check(jmap[3] > 3.0, f"input->GND blocked (got {jmap[3]})")

    # ---- argument refusals ------------------------------------------------
    out = jl.jl_exec(
        "import jumperless as J\nprint('R', J.part_identify(29, 30))", timeout=60)
    m = re.search(r'status=(-?\d+)', out)
    jl.check(m is not None and int(m.group(1)) < 0,
             "x-pin-only rows refuse with a negative status")

    jl.finish("part_id")


if __name__ == '__main__':
    main()
