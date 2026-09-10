#!/usr/bin/env python3
"""bench.py - drive the Jumperless V5 on this Mac without hands.

You are talking to a real board. Three channels:
  terminal   CDC port 1  (what a user sees in the app / serial monitor)
  python     CDC port 5  (MicroPython raw REPL; used for hands simulation)
  read-only  CDC port 7  (screenshots and status; never changes anything)

COMMANDS
  bench.py term "<line>" [secs]      type a line on the terminal, return what it printed
  bench.py run <path> [secs]         run a script by its path and collect output. AFTER THIS, THE ONLY COMMAND TO SEND IS `stop`
                                     (any other terminal or python command while a script runs can hide the Ctrl+Q and wedge the board)
  bench.py stop                      stop a running script (a bare Ctrl+Q on the terminal, then a bare Ctrl+C on the Python port).
                                     ALWAYS use this to stop scripts; never `term` or `raw` while one runs (an Enter in front of Ctrl+Q hides it).
  bench.py raw "<text>" [secs]       send exact bytes, no newline, no priming. \\x11 = Ctrl+Q, \\x10 = Ctrl+P, \\x03 = Ctrl+C, \\r = Enter
  bench.py listen <secs>             just read the terminal for a while (running scripts print here)
  bench.py seq [--secs N] "<step>" ...   one terminal session, several steps in order. A step is a line to type,
                                     or a token: <ENTER> <CTRL-Q> <CTRL-P> <CTRL-C> <WAIT:3> <RAW:x>
  bench.py py "<code>"               run MicroPython on the board (import jumperless is implicit)
  bench.py tap <node>                simulate a probe tap on a row or pad. node = 1-60, or a name:
                                     gnd, top_rail, bottom_rail, dac0, dac1, adc0..adc4, gpio_1..gpio_8,
                                     logo_pad_top, logo_pad_bottom, gpio_pad, dac_pad, adc_pad,
                                     building_pad_top (I+), building_pad_bottom (I-), d0..d13, a0..a7
                                     (the probe tip is held there for ~1.2 s, like a real tap)
  bench.py wheel up|down|click       NOT AVAILABLE on this rig (see the message it prints): wheel steps need a hand
  bench.py wheel hold                NOT AVAILABLE on this rig (no debug probe on the V5). Use `bench.py term ""`
                                     (an Enter on the terminal) to leave a menu, and SAY that you did.
  bench.py switch select|measure     set the probe's slide switch position (simulated)
  bench.py oled                      the OLED contents as text art (what the screen shows right now)
  bench.py leds                      which rows / rails / pads are lit and in what color, as text
  bench.py shot <label>              save <label>_board.png and <label>_oled.png (+ raw dumps) to the shots dir
  bench.py logo                      the logo LED color (blue=connect, red=remove, etc.), pad LEDs, and rail strips - via the terminal
  bench.py status                    nets, rail voltages, gpio, active slot (JSON)
  bench.py ver                       firmware version
  bench.py reset                     put the board in the starting state the docs assume (harness step, not a doc step)

RULES
  - Never send U, u, Z, ~ or anything that reboots, deletes files, or formats; bench.py refuses them.
  - Never run `z <project> new` - it overwrites the owner's project run file. A bare `z <project>` is refused too (its main.py
    can only be stopped by a wheel hold on this rig): use `z <project> noscript`.
  - While a script runs: `listen`, `oled`, `leds`, `shot`, `status` are fine (read-only ports), then `stop`. Nothing else.
  - If the board stops answering (term/py/status all empty for 20 s), STOP: write your report saying the board wedged at that step.
    Do not retry; nothing you can send will recover it.
  - Report exactly what you saw. If a doc step needs a hand (a probe BUTTON press, the physical switch,
    pushing a part in), say "needs a hand" and do the closest thing you can, saying what you substituted.
"""
import sys, os, re, time, json, base64, glob, subprocess

HIL = "/Users/kevinsanto/Documents/GitHub/JumperlOS/test/hil"
sys.path.insert(0, HIL)
HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.environ.get("BENCH_SHOTS", os.path.join(HERE, "shots"))
DEFINES = "/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/JumperlessDefines.h"

# ---------------------------------------------------------------- node names
def node_table():
    t = {}
    try:
        for m in re.finditer(r"^#define\s+([A-Z0-9_]+)\s+(\d+)\b", open(DEFINES).read(), re.M):
            t.setdefault(m.group(1).lower(), int(m.group(2)))
    except Exception:
        pass
    # friendly aliases
    ali = {"gnd": "gnd", "top_rail": "top_rail", "bottom_rail": "bottom_rail", "dac0": "dac0", "dac1": "dac1",
           "adc0": "adc0", "adc1": "adc1", "adc2": "adc2", "adc3": "adc3", "adc4": "adc4",
           "i+": "isense_plus", "i-": "isense_minus", "isense+": "isense_plus", "isense-": "isense_minus",
           "top_rail_plus": "top_rail", "bottom_rail_plus": "bottom_rail", "top+": "top_rail", "bottom+": "bottom_rail",
           "top-": "top_rail_gnd", "bottom-": "bottom_rail_gnd", "top_gnd": "top_rail_gnd", "bottom_gnd": "bottom_rail_gnd",
           "top_minus": "top_rail_gnd", "bottom_minus": "bottom_rail_gnd",
           "top_guy": "logo_pad_top", "bottom_guy": "logo_pad_bottom",
           "i+_pad": "building_pad_top", "i-_pad": "building_pad_bottom", "building_top": "building_pad_top", "building_bottom": "building_pad_bottom"}
    for k, v in ali.items():
        if v in t: t.setdefault(k, t[v])
    for i in range(14):
        if f"nano_d{i}" in t: t.setdefault(f"d{i}", t[f"nano_d{i}"])
    for i in range(8):
        if f"nano_a{i}" in t: t.setdefault(f"a{i}", t[f"nano_a{i}"])
        if f"gpio_{i+1}" not in t and f"gpio{i+1}" in t: t[f"gpio_{i+1}"] = t[f"gpio{i+1}"]
    return t

NODES = node_table()

def node_num(s):
    s = str(s).strip().lower().replace(" ", "_")
    if re.fullmatch(r"-?\d+", s):
        return int(s)
    if s in NODES: return NODES[s]
    for k in (s.replace("-", "_"), s.replace("+", "_plus").replace("-", "_minus")):
        if k in NODES: return NODES[k]
    sys.exit(f"bench: unknown node '{s}'. Rows are 1-60; see the names in `bench.py --help`.")

# ---------------------------------------------------------------- safety
DENY_TERM = [r"^\s*[Uu]\s*$", r"^\s*Z\s*$", r"^\s*~", r"^\s*z\s+\S+\s+.*\bnew\b", r"^\s*!", r"^\s*R\s*$", r"^\s*z\s+(555|i2cscrn|nand00|eeprom)\s*$"]
DENY_PY = [r"jfs\.remove|jfs\.rmdir|os\.remove|os\.rmdir|machine\.reset|reboot|format\(|nodes_save\s*\(\s*\d*\s*\)\s*$|remove_part|fs_remove|jfs\.rename"]
def guard(kind, text):
    pats = DENY_TERM if kind == "term" else DENY_PY
    for p in pats:
        if re.search(p, text):
            sys.exit(f"bench: REFUSED ({kind}) - '{text.strip()[:40]}' is on the deny list (mass storage, USB debug menu, config editor, reboots, deletes, `z <proj> new`, and a bare `z <project>`: it runs the project's main.py, which on this rig can only be stopped by a wheel HOLD (needs a hand) - use `z <project> noscript`, and record the bare form as needs_hand).")

# ---------------------------------------------------------------- channels
def _jl():
    import jl
    return jl

def py(code, timeout=20):
    guard("py", code)
    return _jl().jl_exec(code, timeout=timeout)

def term(line, secs=2.5):
    """Type a line on the terminal. Sends CR only: the "\\r\\n" the HIL helper uses can leave the LF parked at the
    head of port 1's input, and the firmware's interrupt scanner then never sees a Ctrl+Q behind it."""
    guard("term", line)
    import serial
    ser = serial.Serial(_jl().port1_path(), 115200, timeout=0.05)
    try:
        ser.write(b"\r"); ser.flush()
        _drain(ser, 1.5, 0.3)
        ser.reset_input_buffer()
        ser.write(line.encode() + b"\r"); ser.flush()
        return _drain(ser, float(secs), min(3.0, max(0.5, float(secs) * 0.25)))
    finally:
        ser.close()

def run(path, secs=4.0):
    """Run a script by typing its path (CR only), collect `secs` of output. After this, the ONLY thing to send is `stop`."""
    return term(path, secs)

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")
def _open_port1():
    import serial
    ser = serial.Serial(_jl().port1_path(), 115200, timeout=0.05)
    ser.write(b"\r\n"); ser.flush()
    _drain(ser, 4.0, 0.3)
    ser.reset_input_buffer()
    return ser

def _drain(ser, secs, quiet):
    deadline = time.time() + secs; buf = b""; last = time.time()
    while time.time() < deadline:
        c = ser.read(4096)
        if c:
            buf += c; last = time.time()
        elif buf and time.time() - last > quiet:
            break
    return ANSI.sub("", buf.decode(errors="replace"))

def raw(text, secs=2.5):
    """Exact bytes, no newline and NO priming - a leading CR/LF sits in front of a control byte and the firmware's
    interrupt scanner only looks at the first byte, so Ctrl+Q behind an Enter is never seen."""
    guard("term", text)
    data = bytes(text, "utf-8").decode("unicode_escape").encode("latin-1")
    import serial
    ser = serial.Serial(_jl().port1_path(), 115200, timeout=0.05)
    try:
        ser.write(data); ser.flush()
        return _drain(ser, float(secs), 0.5)
    finally:
        ser.close()

def stop(secs=3.0):
    """Stop a running script the clean way: a bare Ctrl+Q (0x11) on the terminal, then a bare Ctrl+C (0x03) on the
    MicroPython port, no other bytes. Use this INSTEAD of `raw "\\x11"` or `term` while a script is running."""
    import serial
    out = []
    for port, byte, name in ((_jl().port1_path(), b"\x11", "port1 Ctrl+Q"), (_jl().port5_path(), b"\x03", "port5 Ctrl+C")):
        try:
            ser = serial.Serial(port, 115200, timeout=0.05)
            ser.write(byte); ser.flush()
            got = _drain(ser, float(secs), 0.6)
            ser.close()
            out.append(f"[{name}] " + got[-400:])
            if "KeyboardInterrupt" in got or "script finished" in got or "finished" in got:
                break
        except Exception as e:
            out.append(f"[{name}] ERR {e}")
    return "\n".join(out)

def listen(secs):
    ser = _open_port1()
    try:
        deadline = time.time() + float(secs); buf = b""
        while time.time() < deadline:
            c = ser.read(4096)
            if c: buf += c
        return ANSI.sub("", buf.decode(errors="replace"))
    finally:
        ser.close()

TOKENS = {"<ENTER>": b"\r", "<CTRL-Q>": b"\x11", "<CTRL-P>": b"\x10", "<CTRL-C>": b"\x03", "<CTRL-A>": b"\x01", "<ESC>": b"\x1b",
          "<UP>": b"\x1b[A", "<DOWN>": b"\x1b[B", "<LEFT>": b"\x1b[D", "<RIGHT>": b"\x1b[C"}
def seq(steps, secs=2.5):
    for s in steps:
        if not s.startswith("<"): guard("term", s)
    ser = _open_port1()
    out = []
    try:
        for s in steps:
            if s.startswith("<WAIT:"):
                time.sleep(float(s[6:-1])); out.append(_drain(ser, 0.2, 0.1)); continue
            if s.startswith("<RAW:"):
                ser.write(bytes(s[5:-1], "utf-8").decode("unicode_escape").encode("latin-1")); ser.flush()
            elif s in TOKENS:
                ser.write(TOKENS[s]); ser.flush()
            else:
                ser.write(s.encode() + b"\r"); ser.flush()
            out.append(f"\n>>> {s}\n" + _drain(ser, float(secs), 0.5))
        return "".join(out)
    finally:
        ser.close()

def port7(cmd, secs=2.0):
    import port7 as p7
    return p7.port7_command(cmd, secs)

# ---------------------------------------------------------------- hands
def tap(node):
    n = node_num(node)
    out = py(f"probe_tap({n})", timeout=10)
    return (out.strip() + f"\nbench: simulated tap on node {n} (held ~1.2 s). It tells the firmware WHERE the tip is; it does not "
            "make an electrical contact, so anything that needs a real voltage at the tip (Measure readings) is not testable this way.")

WHEEL_NOTE = ("bench: NOT AVAILABLE on this rig - simulated click-wheel input (turn, click, hold) does not reach the "
              "firmware's menus or highlighter on this firmware build (the Python hand-back clears it before the "
              "board reads it), and the debug probe is on the other board. Treat any wheel step as NEEDS A HAND. "
              "For a menu that is already open, `bench.py term \"\"` (an Enter on the terminal) leaves it - a substitute, not a hold.")
def wheel(direction, n=1):
    py("clickwheel_press()" if direction in ("click", "press") else ("clickwheel_up(1)" if direction in ("up", "cw", "right") else "clickwheel_down(1)"), 10)
    return WHEEL_NOTE

def switch(pos):
    v = {"select": 1, "measure": 0}.get(pos)
    if v is None: sys.exit("bench: switch select|measure")
    out = py(f"set_switch_position({v})", 10)
    return (out.strip() + "\nbench: switch position SIMULATED (the firmware's variable was set). Two limits: the board re-reads the "
            "physical switch about every 0.5 s and may flip it back, and a Measure reading comes from the real probe tip, which is "
            "touching nothing - so voltages read in Measure are not evidence either way. Record Measure-mode claims as needs_hand "
            "unless you saw the effect.")

# ---------------------------------------------------------------- screens
def oled_text():
    o = port7(":oled\n", 2.5)
    o = o.replace("\r", "")
    i = o.find("oled_q{")
    return o[i:] if i >= 0 else o

def leds_raw():
    o = port7(":leds\n", 2.5)
    m = re.search(r"leds\{(\d+):([0-9a-f]+)\}", o)
    if not m: return None
    n = int(m.group(1)); h = m.group(2)
    return [tuple(int(h[i*6+j*2:i*6+j*2+2], 16) for j in range(3)) for i in range(min(n, len(h)//6))]

def oled_raw():
    o = port7(":oled:b64\n", 2.5)
    m = re.search(r"oled_b64\{(\d+)x(\d+):([A-Za-z0-9+/=]+)\}", o)
    if not m: return None
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, base64.b64decode(m.group(3))

PALETTE = {"red": (255, 0, 0), "orange": (255, 110, 0), "yellow": (255, 255, 0), "green": (0, 255, 0), "cyan": (0, 255, 255),
           "blue": (0, 0, 255), "purple": (150, 0, 255), "magenta": (255, 0, 255), "pink": (255, 40, 120), "white": (255, 255, 255)}
def color_name(rgb):
    r, g, b = rgb; mx = max(rgb)
    if mx < 4: return "off"
    v = tuple(x / mx for x in rgb)
    best, bd = None, 9
    for name, p in PALETTE.items():
        pm = max(p); pv = tuple(x / pm for x in p)
        d = sum((a - b) ** 2 for a, b in zip(v, pv))
        if d < bd: best, bd = name, d
    return best

def rail_volts():
    o = port7(":json:power\n", 2.0)
    def g(k):
        m = re.search(r'"%s"\s*:\s*(-?[0-9.]+)' % k, o)
        return float(m.group(1)) if m else None
    return {"top": g("top_rail"), "bottom": g("bottom_rail"), "dac0": g("dac0"), "dac1": g("dac1")}

def leds_text():
    L = leds_raw()
    if not L: return "bench: no LED snapshot"
    def hexs(c): return "%02x%02x%02x" % c
    lines = []
    lit = []
    for r in range(1, 61):
        px = L[(r-1)*5:(r-1)*5+5]
        on = [p for p in px if max(p) >= 4]
        if on:
            top = max(px, key=max)
            pattern = "".join("#" if max(p) >= 4 else "." for p in px)
            lit.append(f"row {r}: {color_name(top)} {hexs(top)} [{pattern}]")
    lines.append("ROWS LIT (5 LEDs per row, pattern is the 5 LEDs in strip order):" if lit else "ROWS LIT: none")
    lines += ["  " + x for x in lit]
    rv = rail_volts()
    lines.append(f"RAILS (from the board's reported voltages; the rail/logo/pad LEDs themselves are not readable on this firmware): top {rv.get('top')} V, bottom {rv.get('bottom')} V, DAC0 {rv.get('dac0')} V, DAC1 {rv.get('dac1')} V")
    lines.append("LOGO / pad LEDs: not readable on this firmware - use the terminal output and the OLED for mode.")
    return "\n".join(lines)

HEADER_LABELS = ["D1", "D0", "RST", "GND", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11", "D12", "D13",
                 "3V3", "AREF", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "5V", "RST", "GND", "VIN"]

def _boost(rgb, frame_max):
    # LEDs run dim on the board (brightness config). Inverse gamma, then scale the frame so its brightest LED is vivid.
    out = []
    for v in rgb:
        x = (v / 255.0) ** (1 / 2.2)
        out.append(x)
    scale = 0.92 / max(1e-6, (frame_max / 255.0) ** (1 / 2.2))
    return tuple(int(min(255, o * scale * 255)) for o in out)

def rail_color(v):
    if v is None or abs(v) < 0.05: return (46, 50, 56)
    if v > 0:
        t = min(1.0, v / 8.0)
        return (int(255), int(90 + 120 * (1 - t)), 20)      # warm: orange at low, red at high
    t = min(1.0, -v / 8.0)
    return (40, int(120 + 80 * (1 - t)), 255)               # cool for negative

def render_board(L, path, rails=None):
    from PIL import Image, ImageDraw, ImageFont
    W, H = 688, 416
    bg = (16, 18, 22); off = (46, 50, 56)
    im = Image.new("RGB", (W, H), bg); d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 10)
        font2 = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 11)
    except Exception:
        font = font2 = ImageFont.load_default()
    frame_max = max([max(p) for p in L[:300]] + [4])
    def col(p):
        return off if max(p) < 4 else _boost(p, frame_max)
    def sq(x, y, s, c):
        d.rounded_rectangle([x, y, x + s, y + s], radius=3, fill=c)
    x0, cw, s = 20, 22, 14
    rails = rails or {}
    def strip(y, v, label):
        c = rail_color(v)
        for i in range(25):
            sq(x0 + i * (cw * 30 / 25) + 4, y, s, c)
        d.text((x0 + 4, y - 12), label + ("" if v is None else f" {v:.1f} V"), fill=(170, 170, 170), font=font2)
    def gstrip(y, label):
        for i in range(25):
            sq(x0 + i * (cw * 30 / 25) + 4, y, s, (30, 110, 40))
        d.text((x0 + 4, y - 12), label, fill=(170, 170, 170), font=font2)
    strip(20, rails.get("top"), "top rail +"); gstrip(46, "top rail GND")
    for r in range(30):
        for k in range(5):
            sq(x0 + r * cw, 90 + k * 20, s, col(L[r * 5 + k]))
        if (r + 1) % 5 == 0 or r == 0:
            d.text((x0 + r * cw + 2, 74), str(r + 1), fill=(150, 150, 150), font=font)
    d.line([(10, 202), (W - 10, 202)], fill=(60, 64, 70), width=2)
    for r in range(30):
        for k in range(5):
            sq(x0 + r * cw, 214 + k * 20, s, col(L[(r + 30) * 5 + k]))
        if (r + 31) % 5 == 0 or r == 0:
            d.text((x0 + r * cw + 2, 318), str(r + 31), fill=(150, 150, 150), font=font)
    # Kevin (2026-09-09): the bottom rail (+) sits above GND; GND is the very bottom row of the board.
    strip(350, rails.get("bottom"), "bottom rail +"); gstrip(384, "bottom rail GND")
    im.save(path)


def render_oled(w, h, buf, path, scale=6):
    from PIL import Image, ImageDraw
    pad = 10
    im = Image.new("RGB", (w * scale + pad * 2, h * scale + pad * 2), (8, 8, 10)); d = ImageDraw.Draw(im)
    for y in range(h):
        for x in range(w):
            if (buf[(y // 8) * w + x] >> (y % 8)) & 1:
                d.rectangle([pad + x * scale, pad + y * scale, pad + x * scale + scale - 1, pad + y * scale + scale - 1], fill=(170, 215, 255))
    im.save(path)

def shot(label):
    os.makedirs(SHOTS, exist_ok=True)
    label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    L = leds_raw(); o = oled_raw()
    outs = []
    if L:
        p = os.path.join(SHOTS, f"{label}_board.png"); render_board(L, p, rail_volts()); outs.append(p)
        open(os.path.join(SHOTS, f"{label}_leds.txt"), "w").write(leds_text())
    if o:
        w, h, buf = o
        p = os.path.join(SHOTS, f"{label}_oled.png"); render_oled(w, h, buf, p); outs.append(p)
        open(os.path.join(SHOTS, f"{label}_oled.txt"), "w").write(oled_text())
    return "\n".join(outs) if outs else "bench: no screenshot data"


# ---------------------------------------------------------------- logo / rails via the terminal's own R! dump
X256 = None
def _x256(i):
    global X256
    if X256 is None:
        base = [(0,0,0),(128,0,0),(0,128,0),(128,128,0),(0,0,128),(128,0,128),(0,128,128),(192,192,192),(128,128,128),(255,0,0),(0,255,0),(255,255,0),(0,0,255),(255,0,255),(0,255,255),(255,255,255)]
        cube = []
        for r in (0,95,135,175,215,255):
            for g in (0,95,135,175,215,255):
                for b in (0,95,135,175,215,255):
                    cube.append((r,g,b))
        grey = [(8+10*i,)*3 for i in range(24)]
        X256 = base + cube + grey
    return X256[i] if 0 <= i < 256 else (0,0,0)

def r_dump():
    """The terminal's own board dump (R!), raw with ANSI colors."""
    import serial
    ser = serial.Serial(_jl().port1_path(), 115200, timeout=0.01)
    try:
        ser.write(b"\r\n"); ser.flush(); time.sleep(0.8); ser.reset_input_buffer()
        ser.write(b"R!\r\n"); ser.flush()
        buf = b""; t0 = time.time(); last = time.time()
        while time.time() - t0 < 6:
            c = ser.read(65536)
            if c: buf += c; last = time.time()
            elif buf and time.time() - last > 0.8: break
    finally:
        ser.close()
    return buf.decode(errors="replace").replace("\r", "")

def logo_text():
    """Logo LED color and the pad/rail colors, parsed from the R! dump (the only place the release firmware exposes them)."""
    s = r_dump()
    lines = s.split("\n")
    strip = lambda l: re.sub(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]", "", l)
    def cols(l):
        return [int(x) for x in re.findall(r"\x1b\[38;5;(\d+)m", l)]
    start = None
    for i, l in enumerate(lines[:60]):
        if "ADC" in strip(l) and "▐" in strip(l): start = i; break
    if start is None: return "bench: could not find the logo in the R! dump"
    from collections import Counter
    out = []
    logo_cols = []
    for l in lines[start+1:start+5]:
        # the logo glyphs sit left of the pad labels and the header box; cut the raw line at the first '│' or pad label
        raw = l
        v = strip(l)
        cut = len(raw)
        for marker in ("│", "DAC", "GPIO"):
            j = v.find(marker)
            if j > 0:
                # map the visible index back to the raw index by walking
                seen = 0; k = 0
                while k < len(raw) and seen < j:
                    m = re.match(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]", raw[k:])
                    if m: k += len(m.group(0)); continue
                    seen += 1; k += 1
                cut = min(cut, k)
        logo_cols += [c for c in cols(raw[:cut]) if c != 0]
    cnt = Counter(logo_cols)
    names = [f"{color_name(_x256(c))} (x256 {c}) x{n}" for c, n in cnt.most_common(4)]
    out.append("LOGO: " + (", ".join(names) if names else "off/black"))
    for label in ("ADC", "DAC", "GPIO"):
        for l in lines[start:start+6]:
            v = strip(l)
            if label in v and "▐" + label in v.replace(" ", ""):
                j = v.find(label)
                # colors before the label in visible order: take the last color escape before it
                seen = 0; k = 0; last = 0
                while k < len(l) and seen < j:
                    m = re.match(r"\x1b\[38;5;(\d+)m", l[k:])
                    if m: last = int(m.group(1)); k += len(m.group(0)); continue
                    m = re.match(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]", l[k:])
                    if m: k += len(m.group(0)); continue
                    seen += 1; k += 1
                out.append(f"{label} pad: {color_name(_x256(last))} (x256 {last})")
                break
    strips = [i for i, l in enumerate(lines[start:start+80]) if strip(l).count("▐█") >= 20]
    labels = ["top rail +", "top rail GND", "bottom rail +", "bottom rail GND"]
    for k, i in enumerate(strips[:4]):
        cc = [c for c in cols(lines[start+i]) if c != 0]
        cnt = Counter(cc)
        out.append(f"{labels[k]} strip: " + ", ".join(f"{color_name(_x256(c))} x{n}" for c, n in cnt.most_common(2)))
    return "\n".join(out)

def status():
    o = port7(":status\n", 3.0).replace("\r", "")
    i = o.find("{")
    return o[i:] if i >= 0 else o

def reset():
    out = []
    out.append(term("", 1.0)[:0])                 # an Enter leaves menus/pickers
    out.append(py("probe_tap(0)", 10)[:0])
    out.append(py("set_switch_position(1)", 10)[:0])
    out.append(term("<3", 4.0)[:0])                # back to the scratch slot (a project run file may be the active context)
    out.append(term("x", 2.0)[:0])                 # clear all connections
    out.append(py("dac_set(TOP_RAIL, 0.0); dac_set(BOTTOM_RAIL, 0.0)", 15)[:0])
    time.sleep(0.5)
    return "reset done\n" + status()[:400]

def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__); return
    c, a = argv[0], argv[1:]
    if c == "term": print(term(a[0] if a else "", a[1] if len(a) > 1 else 2.5))
    elif c == "run": print(run(a[0], a[1] if len(a) > 1 else 4.0))
    elif c == "raw": print(raw(a[0], a[1] if len(a) > 1 else 2.5))
    elif c == "stop": print(stop(a[0] if a else 3.0))
    elif c == "listen": print(listen(a[0] if a else 5))
    elif c == "seq":
        secs = 2.5
        if a and a[0] == "--secs": secs = float(a[1]); a = a[2:]
        if a and a[0] == "--": a = a[1:]
        print(seq(a, secs))
    elif c == "py": print(py(a[0]))
    elif c == "tap": print(tap(a[0]))
    elif c == "wheel": print(wheel(a[0], a[1] if len(a) > 1 else 1))
    elif c == "switch": print(switch(a[0]))
    elif c == "oled": print(oled_text())
    elif c == "leds": print(leds_text())
    elif c == "shot": print(shot(a[0] if a else "shot"))
    elif c == "status": print(status())
    elif c == "logo": print(logo_text())
    elif c == "ver": print(port7(":ver\n", 1.0).replace("\r", "").strip())
    elif c == "reset": print(reset())
    elif c == "nodes": print(json.dumps({k: v for k, v in sorted(NODES.items()) if not k.startswith("nano")}, indent=0)[:3000])
    else: sys.exit(f"bench: unknown command {c}; see --help")

if __name__ == "__main__":
    main(sys.argv[1:])
