#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The on-hardware half of test/test_routing_og: drive random netlists onto a live
OG over serial (port 1: `x`, `+ a-b, ...`, `b`) and check the crosspoints the firmware
ACTUALLY sent (port 7 `:crossbar` = lastChipXY) against the same wire model of the
OG fabric the host fuzzer uses - SHORT / STRAY on copper, OPEN for unrouted bridges.

    python3 test/test_routing_og/bench_og.py [iters seed maxBridges]
    env: ROWBIAS=65 (percent of endpoints that are rows), LOG=file (board output per
    netlist), DRY=1 (print the netlists only)

Needs pyserial and an OG enumerated as JLOGport1/7. Per the project rule there is no
retry or reconnection logic: if the board goes away it says which netlist was in
flight and stops. Leaves the board cleared (`x`).

2026-09-23: 438 bridges over 59 netlists on Kevin's OG, 0 short/stray, 0 open. Do NOT
run it with a debug probe session attached - a core left halted pauses the RP2040 timer
and main()'s delay(1) never returns, which looks exactly like a firmware hang."""
import random, re, serial, sys, time, os

P1 = "/dev/cu.usbmodemJLOGport1"
P7 = "/dev/cu.usbmodemJLOGport7"
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")

# ---- node ids (JumperlessDefines.h) ----
GND, V33, V5, DAC0, DAC1, ISM, ISP = 100, 103, 105, 106, 107, 109, 108
ADC0, ADC1, ADC2, ADC3, GPIO0, UTX, URX = 110, 111, 112, 113, 114, 116, 117
ND = lambda n: 70 + n          # NANO_D0..D13
NA = lambda n: 86 + n          # NANO_A0..A7
RST, AREF = 84, 85
CHIP = {c: i for i, c in enumerate("ABCDEFGHIJKL")}
A,B,C,D,E,F,G,H,I,J,K,L = range(12)

XMAP = [
 [I,J,B,B,C,C,D,D,E,K,F,F,G,G,H,H],
 [A,A,I,J,C,C,D,D,E,E,F,K,G,G,H,H],
 [A,A,B,B,I,J,D,D,E,E,F,F,G,K,H,H],
 [A,A,B,B,C,C,I,J,E,E,F,F,G,G,H,K],
 [A,K,B,B,C,C,D,D,I,J,F,F,G,G,H,H],
 [A,A,B,K,C,C,D,D,E,E,I,J,G,G,H,H],
 [A,A,B,B,C,K,D,D,E,E,F,F,I,J,H,H],
 [A,A,B,B,C,C,D,K,E,E,F,F,G,G,I,J],
 [NA(0),ND(1),NA(2),ND(3),NA(4),ND(5),NA(6),ND(7),ND(11),ND(9),ND(13),RST,DAC0,ADC0,V33,GND],
 [ND(0),NA(1),ND(2),NA(3),ND(4),NA(5),ND(6),NA(7),ND(8),ND(10),ND(12),AREF,DAC1,ADC1,V5,GND],
 [NA(0),NA(1),NA(2),NA(3),ND(2),ND(3),ND(4),ND(5),ND(6),ND(7),ND(8),ND(9),ND(10),ND(11),ND(12),ADC2],
 [ISM,ISP,ADC0,ADC1,ADC2,ADC3,DAC1,DAC0,1,30,31,60,UTX,URX,V5,GPIO0],
]
YMAP = [[L]+list(range(2,9)), [L]+list(range(9,16)), [L]+list(range(16,23)), [L]+list(range(23,30)),
        [L]+list(range(32,39)), [L]+list(range(39,46)), [L]+list(range(46,53)), [L]+list(range(53,60)),
        list(range(8)), list(range(8)), list(range(8)), list(range(8))]

# ---- wire model ----
wires = {}
def wire(key):
    return wires.setdefault(key, len(wires))
WX = [[None]*16 for _ in range(12)]; WY = [[None]*8 for _ in range(12)]
for c in range(8):
    for x in range(16):
        d = XMAP[c][x]
        lane = sum(1 for px in range(x) if XMAP[c][px] == d) if d < 8 else 0
        WX[c][x] = wire(("lane", min(c,d), max(c,d), lane))
    WY[c][0] = wire(("hub", c))
    for y in range(1,8): WY[c][y] = wire(("node", YMAP[c][y]))
for c in range(8,12):
    for x in range(16): WX[c][x] = wire(("node", XMAP[c][x]))
    for y in range(8): WY[c][y] = wire(("hub", y)) if c == L else wire(("lane", y, c, 0))
NODE_WIRE = {k[1]: w for k, w in wires.items() if k[0] == "node"}
WIRE_NODE = {w: n for n, w in NODE_WIRE.items()}

DRIVEN = {GND, V33, V5, DAC0, DAC1, UTX, GPIO0}
HIGHZ = {ADC0, ADC1, ADC2, ADC3, ISM, ISP, URX, AREF}
def is_nano(n): return 70 <= n <= 93
NAMES = {GND:"GND", V33:"3V3", V5:"5V", DAC0:"DAC0", DAC1:"DAC1", ADC0:"ADC0", ADC1:"ADC1", ADC2:"ADC2",
         ADC3:"ADC3", UTX:"UART_TX", AREF:"AREF", URX:"UART_RX", GPIO0:"GPIO_0", ISP:"I_POS", ISM:"I_NEG", RST:"RST"}
for i in range(14): NAMES[ND(i)] = f"D{i}"
for i in range(8): NAMES[NA(i)] = f"A{i}"
def name(n): return NAMES.get(n, str(n))

# names the OG parser is known to accept (UART_RX / GPIO_0 / I_POS / RST mis-parse - separate issue)
POOL = [GND, V5, V33, DAC0, DAC1, ADC0, ADC1, ADC2, ADC3, UTX, AREF] + [ND(i) for i in range(14)] + [NA(i) for i in range(8)]
SUPPLY = {GND, V5, V33, DAC0, DAC1}

class UF:
    def __init__(s, n): s.p = list(range(n))
    def f(s, a):
        while s.p[a] != a: s.p[a] = s.p[s.p[a]]; a = s.p[a]
        return a
    def u(s, a, b): a, b = s.f(a), s.f(b); s.p[a] = b if a != b else s.p[a]

def nets_of(bridges):
    """Same grouping NetManager does: union nodes; GND->1, DAC0->4, DAC1->5, else 6+."""
    uf = {}
    def f(n):
        uf.setdefault(n, n)
        while uf[n] != n: n = uf[n]
        return n
    for a, b in bridges:
        ra, rb = f(a), f(b)
        if ra != rb: uf[ra] = rb
    groups = {}
    for n in uf: groups.setdefault(f(n), []).append(n)
    node_net = {}; nxt = 6
    for g in groups.values():
        if sum(1 for n in g if n in SUPPLY) > 1: return None
        net = 1 if GND in g else 4 if DAC0 in g else 5 if DAC1 in g else None
        if net is None: net = nxt; nxt += 1
        for n in g: node_net[n] = net
    return node_net

def check(bridges, xbar):
    node_net = nets_of(bridges)
    uf = UF(len(wires))
    for c in range(12):
        for y in range(8):
            bits = xbar[c][y]
            for x in range(16):
                if bits & (1 << x): uf.u(WX[c][x], WY[c][y])
    comp_nets, comp_nodes = {}, {}
    for n, w in NODE_WIRE.items():
        r = uf.f(w); comp_nodes.setdefault(r, set()).add(n)
        if n in node_net: comp_nets.setdefault(r, set()).add(node_net[n])
    fails = []
    for r, nets in comp_nets.items():
        nodes = comp_nodes[r]
        if len(nets) > 1:
            fails.append("SHORT nets %s nodes %s" % (sorted(nets), sorted(name(n) for n in nodes)))
        else:
            net = next(iter(nets))
            for n in nodes:
                if n in node_net: continue
                if n in DRIVEN or (1 <= n <= 60):
                    fails.append("STRAY %s (no net) on net %d" % (name(n), net))
    opens = [(a, b) for a, b in bridges if uf.f(NODE_WIRE[a]) != uf.f(NODE_WIRE[b])]
    return fails, opens

# ---- serial ----
def p1(cmds, settle=2.5, quiet=0.4):
    out = []
    with serial.Serial(P1, 115200, timeout=0.05) as s:
        s.write(b"\r\n"); s.flush(); time.sleep(0.4); s.reset_input_buffer()
        for c in cmds:
            s.write(c.encode() + b"\r\n"); s.flush()
            buf = b""; last = time.time(); dl = time.time() + settle
            while time.time() < dl:
                ch = s.read(4096)
                if ch: buf += ch; last = time.time()
                elif buf and time.time() - last > quiet: break
            out.append(ANSI.sub("", buf.decode(errors="replace")))
    return out

def xbar():
    with serial.Serial(P7, 115200, timeout=0.1) as s:
        s.reset_input_buffer(); s.write(b":crossbar\n"); s.flush()
        buf = b""; t0 = time.time()
        while time.time() - t0 < 2 and b"}" not in buf: buf += s.read(1024)
    m = re.search(r"xbar\{12x8:([0-9A-Fa-f]{384})\}", buf.decode(errors="replace"))
    if not m: raise RuntimeError("no xbar reply: %r" % buf[:120])
    h = m.group(1)
    return [[int(h[(c*8+y)*4:(c*8+y)*4+4], 16) for y in range(8)] for c in range(12)]

def bridge_array(text):
    return re.findall(r"\[([^,\]]+),([^,\]]+),Net (\d+)\]", text)

def main():
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    maxb = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    rowbias = int(os.environ.get("ROWBIAS", "65"))
    random.seed(seed)
    tot_b = tot_open = tot_fail = ran = 0
    for it in range(iters):
        n = random.randint(1, maxb)
        bridges = []
        for _ in range(n):
            a = random.randint(1, 60) if random.random()*100 < rowbias else random.choice(POOL)
            b = random.randint(1, 60) if random.random()*100 < rowbias else random.choice(POOL)
            if a != b and (a, b) not in bridges and (b, a) not in bridges: bridges.append((a, b))
        if not bridges or nets_of(bridges) is None: continue
        cmd = "+ " + ", ".join(f"{name(a)}-{name(b)}" for a, b in bridges)
        print(f"     #{it:3d} sending {cmd}", flush=True)
        if os.environ.get("DRY"): continue
        try:
            outs = p1(["x", cmd, "b"])
        except Exception as e:
            print(f"BOARD GONE while applying #{it}: {cmd}\n  {e}", flush=True); return 2
        if os.environ.get("LOG"): open(os.environ["LOG"], "a").write(f"\n=== #{it} {cmd}\n" + "\n".join(outs))
        ba = bridge_array(outs[2])
        try:
            xb = xbar()
        except Exception as e:
            print(f"BOARD GONE after applying #{it}: {cmd}\n  {e}", flush=True); return 2
        fails, opens = check(bridges, xb)
        ran += 1; tot_b += len(bridges); tot_open += len(opens); tot_fail += len(fails)
        flag = "FAIL" if fails else ("open" if opens else "ok  ")
        print(f"{flag} #{it:3d} {cmd}   [board sees {len(ba)} bridges]")
        for f in fails: print("      ", f)
        for a, b in opens: print(f"       OPEN {name(a)}-{name(b)}")
        if len(ba) != len(bridges): print(f"       NOTE: board bridge array has {len(ba)} entries, sent {len(bridges)}: {ba}")
        if "Couldn't find a path" in outs[1] + outs[2]:
            print("       board: " + "; ".join(l.strip() for l in (outs[1]+outs[2]).splitlines() if "Couldn't" in l))
    p1(["x"])
    print(f"\n{ran} netlists, {tot_b} bridges: {tot_fail} short/stray, {tot_open} open ({100.0*tot_open/max(tot_b,1):.1f}%)")
    return 1 if tot_fail else 0

if __name__ == "__main__":
    sys.exit(main())
