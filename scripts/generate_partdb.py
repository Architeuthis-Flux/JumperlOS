#!/usr/bin/env python3
"""
Generate src/partdb/PartDbData.h from the data/partdb/*.yaml sources.

Companion to generate_projects.py: same discipline (deterministic output,
single-TU generated header, commit the output), different payload - the parts
database is rodata TABLES, not embedded files, so there is no hash history and
nothing is provisioned to FatFS. PartDb.cpp is the only translation unit that
may include the generated header (the projectFiles.h rule); everything else
includes PartDb.h.

Workflow for adding a part:
  1. Edit the family file in data/partdb/ (or add a new one and list it in
     SOURCE_FILES below).
  2. python3 scripts/generate_partdb.py
  3. Rebuild; commit the YAML AND src/partdb/PartDbData.h together.

The output is deterministic: running the generator twice with no source change
produces a byte-identical header (sorted iteration everywhere, no timestamps).

INPUT FORMAT - a constrained YAML subset, parsed line-by-line (stdlib only,
no pyyaml; the on-device /partdb reader (B-M7) will speak the same grammar):

  # comment lines are ignored; '# source:' licensing header is REQUIRED
  class: LOGIC              # file default, one of the 6 part classes
  subclass: s7400           # file default, may be overridden per record

  - id: 7400                # starts a record; <=15 chars of [A-Za-z0-9_-]
    name: 7400              # displayName <=15 (defaults to id)
    desc: Quad 2-input NAND # one-liner, required
    package: dip14          # sipN | dipN (even N) | axial2
    aliases: [74HC00, 74LS00]
    pop: 1                  # picker rank within (class,subclass); lowest first,
                            # unranked records follow, alphabetically
    pins: [1A, 1B, 1Y, 2A, 2B, 2Y, GND, 3Y, 3A, 3B, 4Y, 4A, 4B, VCC]
    # optional keys:
    subclass: opamp         # per-record subclass override
    power: VDD@1 VSS@8      # silence the corner-power check (verified against
                            # pins); 'power: none' = part really has no supply
    led: 40RGBX             # ledName override (<=7 breadboard glyphs)
    menu: 40RGBX|160        # menuName override; '|' = the two-line break
    i2c: [0x3C, 0x3D]       # ACK addresses; '0x20-0x27' = contiguous range
    whoami: 0x75=0x68&0x7E  # WHO_AM_I reg=value[&mask] (mask defaults 0xFF)
    whoami2: 0x6B=0x40&0x40 # optional second read - BOTH must match (the
                            # BMP3xx / SHT2x two-read idents, REF 6.5)
    probe_order: 50         # within one shared address, lower asks first
                            # (REF 6.3: WHO_AM_I holders before heuristics
                            # before registerless parts). Default 100.
    driver: ssd1306         # display driver key (agent C descriptor id)
    value: 10k              # defaultValue for label-only discretes
    fingerprint: GGGGGGG-GGGGGGG-   # Tier-1 unpowered clamp map, one char
                            # per PHYSICAL pin (PartDb.h alphabet: G/V/B =
                            # junction to gnd/vdd/both, N = open, T = hard
                            # tie, '-' = the supply pins themselves, '?' =
                            # don't care, 'C' = conducts somehow - for
                            # records whose aliases span TTL and CMOS)
    vec_supply: either      # either (default: 5V rail pass, 3.3V retry) |
                            # 5v (bipolar TTL only) | 3v3 (CMOS only)
    vec: 1=0 2=0 -> 3=1     # Tier-3 truth-table step, REPEATABLE - one
                            # line per step, run in order with no power
                            # cycle. Each side is pin=level pairs (1-based
                            # physical pins). An input omitted from a step
                            # keeps its previous level (clocking); every
                            # input must appear in the FIRST step. An
                            # omitted output is unchecked at that step.

  pins: is POSITIONAL - entry i is physical pin i+1, and every physical pin
  must be listed. Entries are NAME or NAME=ROLE. Role and pin class derive
  from the name (VCC/VDD/V+/VIN/5V/3V3/VS/VOUT -> power, GND/VSS/V-/AGND/VSSA
  -> gnd, NC -> nc, exact role names SDA/SCL/MOSI/... -> that role); =ROLE
  overrides the role only (e.g. SCL=SCK on an SPI display whose silkscreen
  says SCL).

NAMING for the breadboard LED matrix (Graphics.cpp printString): '\\31' (0x19)
is skipped zero-width; glyph positions 0-6 render on the top half, 7-13 on the
bottom. So a two-line menuName = first segment space-padded to EXACTLY 7
glyphs + '\\31' + <=7 more; a one-line menuName is just <=7 glyphs. The C
literal always encodes 0x19 as the 3-digit octal escape "\\031" - a 2-digit
"\\31" followed by a digit character would extend the escape ("\\311" = 0xC9).
"""

import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
REPO_ROOT = SCRIPT_DIR.parent
DATA_DIR = REPO_ROOT / "data" / "partdb"
OUTPUT_HEADER = REPO_ROOT / "src" / "partdb" / "PartDbData.h"

# Fixed processing order (also the banner's source list). New family files
# must be added here - nothing is globbed, so a stray file can't change the
# output silently.
SOURCE_FILES = [
    "logic_7400.yaml",
    "logic_4000.yaml",
    "analog.yaml",
    "discrete.yaml",
    "transistors.yaml",
    "displays.yaml",
    "modules.yaml",
]

LINE_BREAK = "\x19"  # '\31' - the breadboard/OLED two-line break character

# ---------------------------------------------------------------------------
# Enums - MUST match src/partdb/PartDb.h (the C side is hand-written; these
# are the generator's authoritative copies and the header mirrors them).
# ---------------------------------------------------------------------------

CLASSES = {  # name -> (index, {subclass name -> index})
    "LOGIC":      (0, {"s7400": 0, "s4000": 1, "other": 2}),
    "ANALOG":     (1, {"opamp": 0, "clock": 1, "audio": 2, "power": 3,
                       "other": 4}),
    "DISCRETE":   (2, {"resistor": 0, "capacitor": 1, "led": 2,
                       "inductor": 3, "diode": 4, "pot": 5}),
    "TRANSISTOR": (3, {"bjt": 0, "mosfet": 1, "other": 2}),
    "DISPLAY":    (4, {"oled": 0, "lcd": 1, "mip": 2, "led_direct": 3,
                       "led_driver": 4, "led_addr": 5}),
    "MODULE":     (5, {"sensor": 0, "io": 1, "memory": 2, "other": 3}),
}
NUM_CLASSES = 6
MAX_SUBCLASSES = 6  # rows in the flattened subclass-range table

ROLES = ["NONE", "VCC", "GND", "SDA", "SCL", "MOSI", "MISO", "SCK", "CS",
         "DC", "RST", "BL", "DIN", "DOUT", "CLK", "EN", "INT", "ADDR"]
ROLE_INDEX = {r: i for i, r in enumerate(ROLES)}

PINCLASS_SIGNAL, PINCLASS_POWER, PINCLASS_GND, PINCLASS_NC = 0, 1, 2, 3

FOOT_SIP, FOOT_DIP, FOOT_AXIAL2 = 0, 1, 2
FOOT_NAMES = {FOOT_SIP: "sip", FOOT_DIP: "dip", FOOT_AXIAL2: "axial2"}

# typeStr feeds PartDefinition.typeStr at instantiation (B-M3). Only the
# authored (class, subclass) pairs are mapped; an unmapped pair is a hard
# error so a new family can't pick up a wrong type string silently.
# NOTE: "potentiometer" (13 chars) will NOT fit PartDefinition.typeStr[12] -
# flagged for the B-M3 instantiation commit to resolve (grow the field or
# shorten the string), tracked here so neither side forgets.
TYPESTR_MAP = {
    ("LOGIC", None):      "ic",
    ("ANALOG", None):     "ic",
    ("DISPLAY", None):    "ic",
    ("MODULE", None):     "ic",
    ("DISCRETE", "resistor"):  "resistor",
    ("DISCRETE", "capacitor"): "capacitor",
    ("DISCRETE", "led"):       "led",
    ("DISCRETE", "inductor"):  "inductor",
    ("DISCRETE", "diode"):     "diode",
    ("DISCRETE", "pot"):       "potentiometer",
    ("TRANSISTOR", "bjt"):     "bjt",
    ("TRANSISTOR", "mosfet"):  "fet",
}

# Name -> (pinClass, role) derivation. Exact case-insensitive match.
NAME_DERIVE = {
    "VCC": (PINCLASS_POWER, "VCC"), "VDD": (PINCLASS_POWER, "VCC"),
    "V+": (PINCLASS_POWER, "VCC"), "VIN": (PINCLASS_POWER, "VCC"),
    "5V": (PINCLASS_POWER, "VCC"), "3V3": (PINCLASS_POWER, "VCC"),
    "VS": (PINCLASS_POWER, "VCC"), "VDDA": (PINCLASS_POWER, "VCC"),
    "VOUT": (PINCLASS_POWER, "NONE"), "VEE": (PINCLASS_POWER, "NONE"),
    "GND": (PINCLASS_GND, "GND"), "VSS": (PINCLASS_GND, "GND"),
    "V-": (PINCLASS_GND, "GND"), "AGND": (PINCLASS_GND, "GND"),
    "VSSA": (PINCLASS_GND, "GND"),
    "NC": (PINCLASS_NC, "NONE"),
}
for _r in ROLES[3:]:  # SDA..ADDR: a pin named exactly after a role gets it
    NAME_DERIVE.setdefault(_r, (PINCLASS_SIGNAL, _r))

# Glyphs the breadboard font can render (ASCII members of Graphics.cpp's
# fontMap[120] plus space, which printChar skips while the position still
# advances - i.e. renders blank). ledName/menuName are validated against this.
LED_GLYPHS = set(
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "!$%^*_-+=?<>~',./\\()[]{}|;:\" "
)

ID_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
               "0123456789_-")

# I2C ident flags (mirror PartDb.h)
I2C_FLAG_WHOAMI = 0x01
I2C_FLAG_RANGE = 0x02    # addrs[0] = base of a contiguous run of numAddrs
I2C_FLAG_WHOAMI2 = 0x04  # the second (reg,value,mask) must ALSO match

warnings = []


def warn(msg):
    warnings.append(msg)
    print(f"  WARN: {msg}")


class SrcError(Exception):
    pass


# ---------------------------------------------------------------------------
# Case fold - MUST match pdbFold() in PartDb.cpp exactly (ASCII a-z -> A-Z,
# everything else unchanged) so the generator's sort order and the firmware's
# binary-search compare agree.
# ---------------------------------------------------------------------------

def fold(s):
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)


def c_escape(s):
    """C string literal. 0x19 always becomes the 3-digit octal \\031 so a
    following digit character can never extend the escape."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == LINE_BREAK:
            out.append("\\031")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            raise SrcError(f"unencodable character {ord(ch):#x} in {s!r}")
    return '"' + "".join(out) + '"'


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def split_kv(s, ctx):
    if ":" not in s:
        raise SrcError(f"{ctx}: expected 'key: value', got {s!r}")
    k, v = s.split(":", 1)
    return k.strip(), v.strip()


def strip_comment(line):
    # A trailing comment starts at ' #'. Values must not contain ' #'.
    idx = line.find(" #")
    return line[:idx].rstrip() if idx != -1 else line.rstrip()


def parse_list(v, ctx):
    if v.startswith("[") and v.endswith("]"):
        inner = v[1:-1].strip()
        return [t.strip() for t in inner.split(",")] if inner else []
    raise SrcError(f"{ctx}: expected a [flow, list], got {v!r}")


def parse_file(path):
    """-> list of record dicts. Enforces the '# source:' licensing header."""
    records = []
    file_class = None
    file_sub = None
    cur = None
    saw_source = False
    text = path.read_text(encoding="utf-8")
    for lineno, raw in enumerate(text.splitlines(), 1):
        ctx = f"{path.name}:{lineno}"
        if not raw.strip():
            continue
        if raw.lstrip().startswith("#"):
            if raw.lstrip()[1:].strip().startswith("source:"):
                saw_source = True
            continue
        line = strip_comment(raw)
        if not line.strip():
            continue
        if line.startswith("- "):
            k, v = split_kv(line[2:], ctx)
            if k != "id":
                raise SrcError(f"{ctx}: a record must start with '- id:'")
            cur = {"id": v, "_ctx": ctx, "_class": file_class,
                   "subclass": file_sub, "_file": path.name}
            records.append(cur)
        elif line.startswith("  "):
            if cur is None:
                raise SrcError(f"{ctx}: indented line outside a record")
            k, v = split_kv(line.strip(), ctx)
            if k == "vec":
                # the one repeatable key: each line is one vector step
                cur.setdefault("_vecs", []).append((v, ctx))
                continue
            if k in cur and k not in ("subclass",):
                raise SrcError(f"{ctx}: duplicate key {k!r}")
            cur[k] = v
        else:
            k, v = split_kv(line, ctx)
            if k == "class":
                file_class = v
            elif k == "subclass":
                file_sub = v
            else:
                raise SrcError(f"{ctx}: unknown top-level key {k!r}")
    if not saw_source:
        raise SrcError(f"{path.name}: missing '# source:' licensing header")
    return records


def parse_package(v, ctx):
    if v == "axial2":
        return FOOT_AXIAL2, 2
    for prefix, foot in (("sip", FOOT_SIP), ("dip", FOOT_DIP)):
        if v.startswith(prefix):
            try:
                n = int(v[len(prefix):])
            except ValueError:
                raise SrcError(f"{ctx}: bad package {v!r}")
            if foot == FOOT_DIP and n % 2:
                raise SrcError(f"{ctx}: DIP pin count must be even ({v})")
            if foot == FOOT_DIP and not 4 <= n <= 60:
                raise SrcError(f"{ctx}: dip{n} out of range")
            if foot == FOOT_SIP and not 1 <= n <= 30:
                raise SrcError(f"{ctx}: sip{n} exceeds a 30-row half")
            return foot, n
    raise SrcError(f"{ctx}: bad package {v!r} (sipN | dipN | axial2)")


def parse_pin(tok, ctx):
    """NAME or NAME=ROLE -> (name, pinClass, roleIdx)."""
    if "=" in tok:
        name, role = tok.split("=", 1)
        name, role = name.strip(), role.strip().upper()
        if role not in ROLE_INDEX:
            raise SrcError(f"{ctx}: unknown role {role!r} on pin {name!r}")
        derived = NAME_DERIVE.get(name.upper())
        pcls = derived[0] if derived else PINCLASS_SIGNAL
    else:
        name = tok.strip()
        pcls, role = NAME_DERIVE.get(name.upper(),
                                     (PINCLASS_SIGNAL, "NONE"))
    if not name:
        raise SrcError(f"{ctx}: empty pin name")
    if len(name) > 11:
        raise SrcError(f"{ctx}: pin name {name!r} > 11 chars "
                       f"(PartPin.name[12])")
    if not all(32 <= ord(c) < 127 for c in name):
        raise SrcError(f"{ctx}: non-ASCII pin name {name!r}")
    return name, pcls, ROLE_INDEX[role]


def parse_i2c_addrs(v, ctx):
    """[0x3C, 0x3D] or [0x20-0x27] -> (addr_list, is_range)."""
    toks = parse_list(v, ctx)
    addrs = []
    for t in toks:
        if "-" in t:
            lo, hi = (int(x, 0) for x in t.split("-", 1))
            addrs.extend(range(lo, hi + 1))
        else:
            addrs.append(int(t, 0))
    for a in addrs:
        if not 0x03 <= a <= 0x77:
            raise SrcError(f"{ctx}: I2C address {a:#04x} outside 0x03-0x77")
    if len(set(addrs)) != len(addrs):
        raise SrcError(f"{ctx}: duplicate I2C address")
    is_range = (len(addrs) > 1 and
                addrs == list(range(addrs[0], addrs[0] + len(addrs))))
    if len(addrs) > 4 and not is_range:
        raise SrcError(f"{ctx}: >4 I2C addresses only as a contiguous range")
    return addrs, is_range


VEC_SUPPLY = {"either": 0, "5v": 1, "3v3": 2}  # mirror PartDbVecSupply


def parse_vectors(vec_lines, pin_count, pins, ctx):
    """[(line, ctx)] -> pools dict for one PartDbVectorSet, or None.

    Each line: 'pin=level ... -> pin=level ...'. Steps run in order with no
    power cycle; an input omitted from a step keeps its previous level, an
    omitted output is unchecked (outCare bit 0). Every input must appear in
    the first step - carry-forward needs a starting level.
    """
    if not vec_lines:
        return None
    rail = {i + 1 for i, (n, pcls, role) in enumerate(pins)
            if pcls in (PINCLASS_POWER, PINCLASS_GND)}
    in_pins, out_pins, steps = [], [], []
    for text, vctx in vec_lines:
        if "->" not in text:
            raise SrcError(f"{vctx}: vec needs 'in... -> out...'")
        ins_s, outs_s = text.split("->", 1)

        def side(s, vctx=vctx):
            d = {}
            for tok in s.split():
                if "=" not in tok:
                    raise SrcError(f"{vctx}: bad vec token {tok!r} "
                                   f"(pin=level)")
                p_s, l_s = tok.split("=", 1)
                try:
                    p, lvl = int(p_s), int(l_s)
                except ValueError:
                    raise SrcError(f"{vctx}: bad vec token {tok!r}")
                if not 1 <= p <= pin_count:
                    raise SrcError(f"{vctx}: vec pin {p} outside "
                                   f"1-{pin_count}")
                if p in rail:
                    raise SrcError(f"{vctx}: vec pin {p} is a supply pin")
                if lvl not in (0, 1):
                    raise SrcError(f"{vctx}: vec level for pin {p} "
                                   f"must be 0/1")
                if p in d:
                    raise SrcError(f"{vctx}: vec pin {p} repeated")
                d[p] = lvl
            return d

        ins, outs = side(ins_s), side(outs_s)
        for p in ins:
            if p in out_pins:
                raise SrcError(f"{vctx}: vec pin {p} both driven and read")
            if p not in in_pins:
                in_pins.append(p)
        for p in outs:
            if p in in_pins:
                raise SrcError(f"{vctx}: vec pin {p} both driven and read")
            if p not in out_pins:
                out_pins.append(p)
        steps.append((ins, outs, vctx))
    if len(in_pins) > 16 or len(out_pins) > 16:
        raise SrcError(f"{ctx}: vec set exceeds 16 in or out pins "
                       f"(uint16 bit fields)")
    if len(steps) > 255:
        raise SrcError(f"{ctx}: more than 255 vec steps")
    missing = [p for p in in_pins if p not in steps[0][0]]
    if missing:
        raise SrcError(f"{ctx}: vec inputs {missing} not set in the first "
                       f"step (carry-forward needs a starting level)")
    in_bits, out_bits, out_care = [], [], []
    cur = {}
    for ins, outs, vctx in steps:
        cur.update(ins)
        in_bits.append(sum((cur[p] & 1) << i for i, p in enumerate(in_pins)))
        out_bits.append(sum((outs.get(p, 0) & 1) << i
                            for i, p in enumerate(out_pins)))
        out_care.append(sum(1 << i for i, p in enumerate(out_pins)
                            if p in outs))
    return {
        "in_pins": in_pins, "out_pins": out_pins, "in_bits": in_bits,
        "out_bits": out_bits, "out_care": out_care,
    }


def compose_led(rec, display, ctx):
    led = rec.get("led", display)
    if len(led) > 7:
        raise SrcError(f"{ctx}: ledName {led!r} > 7 glyphs - add a 'led:' "
                       f"override")
    bad = set(led) - LED_GLYPHS
    if bad:
        raise SrcError(f"{ctx}: ledName glyphs not in the breadboard font: "
                       f"{sorted(bad)}")
    return led


def compose_menu(rec, display, ctx):
    override = rec.get("menu")
    if override is not None:
        if "|" in override:
            a, b = override.split("|", 1)
            if not (0 < len(a) <= 7 and 0 < len(b) <= 7):
                raise SrcError(f"{ctx}: menu segments must be 1-7 glyphs "
                               f"({override!r})")
            menu = a.ljust(7) + LINE_BREAK + b
        else:
            if len(override) > 7:
                raise SrcError(f"{ctx}: one-line menu {override!r} > 7 "
                               f"glyphs (use 'a|b' for two lines)")
            menu = override
    elif len(display) <= 7:
        menu = display
    else:
        menu = None
        for i, ch in enumerate(display):
            if ch == " " and 0 < i <= 7 and 0 < len(display) - i - 1 <= 7:
                menu = display[:i].ljust(7) + LINE_BREAK + display[i + 1:]
                break
        if menu is None:
            raise SrcError(f"{ctx}: {display!r} doesn't fit one line and "
                           f"can't auto-split - add a 'menu: a|b' override")
    for ch in menu:
        if ch != LINE_BREAK and ch not in LED_GLYPHS:
            raise SrcError(f"{ctx}: menuName glyph {ch!r} not in the "
                           f"breadboard font")
    if LINE_BREAK in menu:
        seg1 = menu.split(LINE_BREAK)[0]
        assert len(seg1) == 7, "two-line menuName first segment must pad to 7"
    return menu


# ---------------------------------------------------------------------------
# Record processing + validation
# ---------------------------------------------------------------------------

def process_record(rec):
    ctx = rec["_ctx"]
    rid = rec["id"]
    if not 1 <= len(rid) <= 15:
        raise SrcError(f"{ctx}: id {rid!r} must be 1-15 chars")
    if not set(rid) <= ID_CHARS:
        raise SrcError(f"{ctx}: id {rid!r} has chars outside [A-Za-z0-9_-]")

    cls_name = rec["_class"]
    if cls_name not in CLASSES:
        raise SrcError(f"{ctx}: unknown class {cls_name!r}")
    cls_idx, subs = CLASSES[cls_name]
    sub_name = rec.get("subclass")
    if sub_name not in subs:
        raise SrcError(f"{ctx}: unknown subclass {sub_name!r} for {cls_name}")
    sub_idx = subs[sub_name]

    display = rec.get("name", rid)
    if not 1 <= len(display) <= 15:
        raise SrcError(f"{ctx}: name {display!r} must be 1-15 chars")
    if not all(32 <= ord(c) < 127 for c in display):
        raise SrcError(f"{ctx}: non-ASCII name {display!r}")

    desc = rec.get("desc")
    if not desc:
        raise SrcError(f"{ctx}: missing desc")

    if "package" not in rec:
        raise SrcError(f"{ctx}: missing package")
    foot, pin_count = parse_package(rec["package"], ctx)

    if "pins" not in rec:
        raise SrcError(f"{ctx}: missing pins")
    pin_toks = parse_list(rec["pins"], ctx)
    if len(pin_toks) != pin_count:
        raise SrcError(f"{ctx}: {len(pin_toks)} pins listed but "
                       f"{rec['package']} has {pin_count}")
    pins = [parse_pin(t, ctx) for t in pin_toks]
    if len(pins) > 24:
        raise SrcError(f"{ctx}: {len(pins)} listed pins > 24 (MAX_PART_PINS)")
    if len(pins) > 16:
        warn(f"{ctx}: {rid} lists {len(pins)} pins - filtered out on OG at "
             f"runtime (MAX_PART_PINS 16)")

    roles_present = {p[2] for p in pins}
    if (ROLE_INDEX["SDA"] in roles_present
            and ROLE_INDEX["SCL"] not in roles_present):
        raise SrcError(f"{ctx}: an SDA-role pin without an SCL-role pin")

    classes_present = {p[1] for p in pins}
    power_key = rec.get("power")
    if power_key == "none":
        if PINCLASS_POWER in classes_present:
            raise SrcError(f"{ctx}: 'power: none' but a power-class pin "
                           f"exists")
    elif cls_name in ("LOGIC", "ANALOG", "DISPLAY", "MODULE"):
        if PINCLASS_POWER not in classes_present:
            warn(f"{ctx}: {rid} has no power-class pin")
        if PINCLASS_GND not in classes_present:
            warn(f"{ctx}: {rid} has no gnd-class pin")

    # Corner-power convention check: dip14/dip16 logic parts almost always
    # put GND at pin N/2 and VCC at pin N. Non-conformers (4049/4050, 7490)
    # declare 'power: NAME@pin NAME@pin' to prove the layout is deliberate.
    if (cls_name == "LOGIC" and foot == FOOT_DIP and pin_count in (14, 16)
            and power_key is None):
        half, last = pin_count // 2, pin_count
        if pins[half - 1][1] != PINCLASS_GND:
            raise SrcError(f"{ctx}: {rid} pin {half} is not gnd-class - "
                           f"corner-power violation; declare 'power:' if "
                           f"the layout really is nonstandard")
        if pins[last - 1][1] != PINCLASS_POWER:
            raise SrcError(f"{ctx}: {rid} pin {last} is not power-class - "
                           f"corner-power violation; declare 'power:' if "
                           f"the layout really is nonstandard")
    if power_key and power_key != "none":
        for tok in power_key.split():
            if "@" not in tok:
                raise SrcError(f"{ctx}: bad power token {tok!r} "
                               f"(NAME@pin)")
            pname, pnum = tok.split("@", 1)
            pnum = int(pnum)
            if not 1 <= pnum <= pin_count:
                raise SrcError(f"{ctx}: power pin {pnum} out of range")
            actual = pins[pnum - 1]
            if actual[0].upper() != pname.upper():
                raise SrcError(f"{ctx}: power override says pin {pnum} is "
                               f"{pname!r} but pins: lists {actual[0]!r}")
            if actual[1] not in (PINCLASS_POWER, PINCLASS_GND):
                raise SrcError(f"{ctx}: power override pin {pnum} "
                               f"({pname!r}) is not power/gnd class")

    led = compose_led(rec, display, ctx)
    menu = compose_menu(rec, display, ctx)

    aliases = []
    if "aliases" in rec:
        for a in parse_list(rec["aliases"], ctx):
            if not 1 <= len(a) <= 15 or not set(a) <= ID_CHARS:
                raise SrcError(f"{ctx}: bad alias {a!r}")
            aliases.append(a)

    def parse_whoami(w, key):
        if "=" not in w:
            raise SrcError(f"{ctx}: {key} needs reg=value[&mask]")
        reg_s, rest = w.split("=", 1)
        if "&" in rest:
            val_s, mask_s = rest.split("&", 1)
        else:
            val_s, mask_s = rest, "0xFF"
        return int(reg_s, 0), int(val_s, 0), int(mask_s, 0)

    ident = None
    if "i2c" in rec:
        addrs, is_range = parse_i2c_addrs(rec["i2c"], ctx)
        flags = I2C_FLAG_RANGE if is_range else 0
        reg = val = mask = 0
        reg2 = val2 = mask2 = 0
        if "whoami" in rec:
            reg, val, mask = parse_whoami(rec["whoami"], "whoami")
            flags |= I2C_FLAG_WHOAMI
        if "whoami2" in rec:
            if "whoami" not in rec:
                raise SrcError(f"{ctx}: whoami2 without whoami")
            reg2, val2, mask2 = parse_whoami(rec["whoami2"], "whoami2")
            flags |= I2C_FLAG_WHOAMI2
        order = int(rec["probe_order"]) if "probe_order" in rec else 100
        if not 0 <= order <= 255:
            raise SrcError(f"{ctx}: probe_order {order} outside 0-255")
        if is_range:
            stored = (addrs[0], 0, 0, 0)
        else:
            stored = tuple(addrs) + (0,) * (4 - len(addrs))
        ident = (len(addrs), stored, reg, val, mask, reg2, val2, mask2,
                 order, flags)
    elif "whoami" in rec or "whoami2" in rec or "probe_order" in rec:
        raise SrcError(f"{ctx}: whoami/whoami2/probe_order without i2c")

    type_key = (cls_name, None) if (cls_name, None) in TYPESTR_MAP \
        else (cls_name, sub_name)
    if type_key not in TYPESTR_MAP:
        raise SrcError(f"{ctx}: no typeStr mapping for "
                       f"{cls_name}/{sub_name} - add one to TYPESTR_MAP")
    type_str = TYPESTR_MAP[type_key]

    driver = rec.get("driver")
    if driver is not None and (not 1 <= len(driver) <= 15
                               or not set(driver) <= ID_CHARS):
        raise SrcError(f"{ctx}: bad driver key {driver!r}")

    value = rec.get("value")
    if value is not None and len(value) > 11:
        raise SrcError(f"{ctx}: value {value!r} > 11 chars "
                       f"(PartDefinition.value[12])")

    # Tier-1 clamp fingerprint (optional): one char per PHYSICAL pin. The
    # alphabet must stay in lockstep with PartDb.h and the measured fp=
    # string partScanClampFingerprint produces. Supply pins carry '-' (the
    # measured string marks the rails it anchored on the same way).
    fingerprint = rec.get("fingerprint")
    if fingerprint is not None:
        if len(fingerprint) != pin_count:
            raise SrcError(f"{ctx}: fingerprint is {len(fingerprint)} chars "
                           f"but {rec['package']} has {pin_count} pins")
        bad = set(fingerprint) - set("GVBNT-?C")
        if bad:
            raise SrcError(f"{ctx}: fingerprint chars {sorted(bad)} not in "
                           f"the G/V/B/N/T/-/?/C alphabet")
        for n, (pname, pcls, role) in enumerate(pins):
            is_rail = pcls in (PINCLASS_POWER, PINCLASS_GND)
            ch = fingerprint[n]
            if is_rail and ch not in "-?":
                raise SrcError(f"{ctx}: fingerprint pin {n + 1} ({pname}) is "
                               f"a supply pin - must be '-' (or '?')")
            if not is_rail and ch == "-":
                raise SrcError(f"{ctx}: fingerprint pin {n + 1} ({pname}) is "
                               f"'-' but not a supply pin")

    vectors = parse_vectors(rec.get("_vecs", []), pin_count, pins, ctx)
    supply_key = rec.get("vec_supply", "either")
    if supply_key not in VEC_SUPPLY:
        raise SrcError(f"{ctx}: vec_supply {supply_key!r} not in "
                       f"{sorted(VEC_SUPPLY)}")
    if vectors is not None:
        vectors["supply"] = VEC_SUPPLY[supply_key]
    elif "vec_supply" in rec:
        raise SrcError(f"{ctx}: vec_supply without any vec: lines")

    pop = int(rec["pop"]) if "pop" in rec else None

    return {
        "id": rid, "display": display, "led": led, "menu": menu,
        "desc": desc, "cls": cls_idx, "sub": sub_idx,
        "cls_name": cls_name, "sub_name": sub_name,
        "foot": foot, "pin_count": pin_count, "pins": tuple(pins),
        "aliases": aliases, "ident": ident, "type_str": type_str,
        "driver": driver, "value": value, "pop": pop,
        "fingerprint": fingerprint, "vectors": vectors, "ctx": ctx,
    }


# ---------------------------------------------------------------------------
# Table building
# ---------------------------------------------------------------------------

def build_tables(records):
    # Deterministic record order = the byClass order: class, subclass,
    # authored pop rank (unranked last), folded id. partdb_byClass is then
    # the identity permutation, emitted explicitly all the same.
    records.sort(key=lambda r: (r["cls"], r["sub"],
                                r["pop"] if r["pop"] is not None else 1 << 16,
                                fold(r["id"])))

    ids_seen = {}
    for r in records:
        if r["id"] in ids_seen:
            raise SrcError(f"{r['ctx']}: duplicate id {r['id']!r}")
        ids_seen[r["id"]] = True

    # Pinout dedup: canonical form = (footprint, pinCount, ordered pin
    # tuples). Identical layouts collapse to one pinout + one pins run.
    pinout_key_to_idx = {}
    pinouts = []          # (foot, pin_count, pins tuple)
    pinout_users = []     # [ids] per pinout, for the header comments
    for r in records:
        key = (r["foot"], r["pin_count"], r["pins"])
        if key not in pinout_key_to_idx:
            pinout_key_to_idx[key] = len(pinouts)
            pinouts.append(key)
            pinout_users.append([])
        r["pinout_idx"] = pinout_key_to_idx[key]
        pinout_users[r["pinout_idx"]].append(r["id"])

    # I2C ident dedup, same idea.
    ident_key_to_idx = {}
    idents = []
    for r in records:
        if r["ident"] is None:
            r["ident_idx"] = 0xFF
            continue
        if r["ident"] not in ident_key_to_idx:
            ident_key_to_idx[r["ident"]] = len(idents)
            idents.append(r["ident"])
        r["ident_idx"] = ident_key_to_idx[r["ident"]]

    # Fingerprint dedup (the 74xx gate quartets share "CCCCCC-CCCCCC-").
    fp_key_to_idx = {}
    fingerprints = []
    for r in records:
        if r["fingerprint"] is None:
            r["fp_idx"] = 0xFF
            continue
        if r["fingerprint"] not in fp_key_to_idx:
            fp_key_to_idx[r["fingerprint"]] = len(fingerprints)
            fingerprints.append(r["fingerprint"])
        r["fp_idx"] = fp_key_to_idx[r["fingerprint"]]

    # Vector sets: one per authored record, no dedup (bit patterns differ
    # even when the pin lists agree - not worth the bookkeeping).
    vector_sets = []
    for r in records:
        if r["vectors"] is None:
            r["vec_idx"] = 0xFFFF
            continue
        r["vec_idx"] = len(vector_sets)
        vector_sets.append((r["id"], r["vectors"]))

    # typeStr pool, ordered by first use (record order is deterministic).
    type_strs = []
    for r in records:
        if r["type_str"] not in type_strs:
            type_strs.append(r["type_str"])
        r["type_idx"] = type_strs.index(r["type_str"])

    # Name index: canonical ids + aliases, sorted by the case fold the C
    # binary search uses. Collisions after folding are errors.
    names = []
    for i, r in enumerate(records):
        names.append((r["id"], i))
        for a in r["aliases"]:
            names.append((a, i))
    by_fold = {}
    for n, i in names:
        f = fold(n)
        if f in by_fold:
            other = records[by_fold[f]]["id"]
            raise SrcError(f"name {n!r} collides with {other!r} after "
                           f"case-fold")
        by_fold[f] = i
    names.sort(key=lambda t: fold(t[0]))

    # Class/subclass ranges over byClass (= record order).
    by_class = list(range(len(records)))
    class_ranges = [[0, 0] for _ in range(NUM_CLASSES)]
    sub_ranges = [[0, 0] for _ in range(NUM_CLASSES * MAX_SUBCLASSES)]
    for i, r in enumerate(records):
        cr = class_ranges[r["cls"]]
        if cr[1] == 0:
            cr[0] = i
        cr[1] += 1
        sr = sub_ranges[r["cls"] * MAX_SUBCLASSES + r["sub"]]
        if sr[1] == 0:
            sr[0] = i
        sr[1] += 1

    # Index-width guarantees (the struct fields are this narrow).
    assert len(records) < 0xFFFF, "recIdx/pinout indices are uint16"
    assert len(pinouts) < 0xFFFF, "pinoutIdx is uint16 (0xFFFF reserved)"
    assert len(idents) < 0xFF, "i2cIdentIdx is uint8 (0xFF = none)"
    assert len(fingerprints) < 0xFF, "fingerprintIdx is uint8 (0xFF = none)"
    assert len(vector_sets) < 0xFFFF, "vectorSetIdx is uint16 (0xFFFF = none)"
    assert len(type_strs) < 256, "typeStrIdx is uint8"

    return {
        "records": records, "pinouts": pinouts, "pinout_users": pinout_users,
        "idents": idents, "fingerprints": fingerprints,
        "vector_sets": vector_sets,
        "type_strs": type_strs, "names": names,
        "by_class": by_class, "class_ranges": class_ranges,
        "sub_ranges": sub_ranges,
    }


# ---------------------------------------------------------------------------
# Size estimate (ARM 32-bit pointers; identical string literals merged by the
# compiler within the single including TU)
# ---------------------------------------------------------------------------

def estimate_rodata(t):
    n_pins = sum(len(p[2]) for p in t["pinouts"])
    strings = set()
    for foot, cnt, pins in t["pinouts"]:
        for name, _, _ in pins:
            strings.add(name)
    for r in t["records"]:
        strings.update([r["id"], r["display"], r["led"], r["menu"],
                        r["desc"]])
        if r["driver"]:
            strings.add(r["driver"])
        if r["value"]:
            strings.add(r["value"])
    for n, _ in t["names"]:
        strings.add(n)
    strings.update(t["type_strs"])
    strings.update(t["fingerprints"])
    str_bytes = sum(len(s) + 1 for s in strings)
    vec_bytes = sum(len(v["in_pins"]) + len(v["out_pins"]) +
                    6 * len(v["in_bits"]) + 24
                    for _, v in t["vector_sets"])
    sizes = {
        "pins": n_pins * 8,
        "pinouts": len(t["pinouts"]) * 8,
        "i2cIdents": len(t["idents"]) * 13,
        "fingerprints": len(t["fingerprints"]) * 4,
        "vectorSets": vec_bytes,
        "records": len(t["records"]) * 40,
        "names": len(t["names"]) * 8,
        "byClass": len(t["by_class"]) * 2,
        "ranges": (NUM_CLASSES + NUM_CLASSES * MAX_SUBCLASSES) * 4,
        "typeStrs": len(t["type_strs"]) * 4,
        "strings": str_bytes,
    }
    sizes["total"] = sum(sizes.values())
    return sizes


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

def emit_header(t, sizes):
    rec = t["records"]
    lines = [
        "#ifndef PARTDB_DATA_H",
        "#define PARTDB_DATA_H",
        "",
        '#include "PartDb.h"',
        "",
        "//==============================================================================",
        "// Jumperless Parts Database - generated rodata tables",
        "//==============================================================================",
        "// This file is AUTO-GENERATED by scripts/generate_partdb.py",
        "// Do not edit manually! Edit the sources in data/partdb/ instead.",
        "//",
        "// Sources: " + ", ".join(SOURCE_FILES),
        "//",
        "// These are DEFINITIONS (not extern declarations), so exactly ONE",
        "// translation unit may include this header: src/partdb/PartDb.cpp -",
        "// same rule as projectFiles.h. Everything else includes PartDb.h.",
        "//",
        f"// Dedup: {len(rec)} records -> {len(t['pinouts'])} pinouts, "
        f"{len(t['names'])} names, {len(t['idents'])} i2c idents, "
        f"{len(t['fingerprints'])} fingerprints",
        f"// Estimated rodata: ~{sizes['total']} bytes "
        f"(pins {sizes['pins']}, pinouts {sizes['pinouts']}, "
        f"records {sizes['records']}, names {sizes['names']},",
        f"//   i2cIdents {sizes['i2cIdents']}, "
        f"fingerprints {sizes['fingerprints']}, "
        f"byClass {sizes['byClass']}, "
        f"ranges {sizes['ranges']}, typeStrs {sizes['typeStrs']}, "
        f"strings ~{sizes['strings']})",
        "// (assumes 32-bit pointers and in-TU merging of identical string"
        " literals)",
        "//",
        "// partdb_records / partdb_byClass ordering: class, then subclass,",
        "// then authored 'pop:' rank (unranked last, alphabetically) - so a",
        "// class or subclass slice IS the picker's most-common-first list.",
        "// partdb_names is sorted by the PartDb.cpp case fold (a-z -> A-Z)",
        "// for binary search.",
        "//==============================================================================",
        "",
    ]

    # --- pins pool ---
    lines.append("const PartDbPin partdb_pins[] = {")
    pin_base = 0
    pinout_pin_base = []
    for pi, (foot, cnt, pins) in enumerate(t["pinouts"]):
        pinout_pin_base.append(pin_base)
        users = t["pinout_users"][pi]
        shown = " ".join(users[:6]) + (" ..." if len(users) > 6 else "")
        lines.append(f"  // pinout {pi}: {FOOT_NAMES[foot]}"
                     f"{cnt if foot != FOOT_AXIAL2 else ''} ({shown})")
        for n, (name, pcls, role) in enumerate(pins):
            lines.append(f"  {{ {c_escape(name)}, {n + 1}, -1, {pcls}, "
                         f"{role} }},")
        pin_base += len(pins)
    lines.append("};")
    lines.append("")

    # --- pinouts ---
    lines.append("const PartDbPinout partdb_pinouts[] = {")
    for pi, (foot, cnt, pins) in enumerate(t["pinouts"]):
        lines.append(f"  {{ {foot}, {cnt}, {len(pins)}, "
                     f"&partdb_pins[{pinout_pin_base[pi]}] }}, // {pi}")
    lines.append("};")
    lines.append("")

    # --- i2c idents ---
    lines.append("// { numAddrs, addrs, whoAmIReg, whoAmIValue, whoAmIMask,")
    lines.append("//   whoAmIReg2, whoAmIValue2, whoAmIMask2, probeOrder,"
                 " flags }")
    lines.append("const PartDbI2cIdent partdb_i2cIdents[] = {")
    for ii, (num, addrs, reg, val, mask, reg2, val2, mask2, order,
             flags) in enumerate(t["idents"]):
        astr = ", ".join(f"0x{a:02X}" for a in addrs)
        lines.append(f"  {{ {num}, {{ {astr} }}, 0x{reg:02X}, 0x{val:02X}, "
                     f"0x{mask:02X}, 0x{reg2:02X}, 0x{val2:02X}, "
                     f"0x{mask2:02X}, {order}, 0x{flags:02X} }}, // {ii}")
    lines.append("};")
    lines.append("")

    # --- fingerprints ---
    lines.append("// Tier-1 unpowered clamp fingerprints (PartDb.h alphabet)")
    lines.append("const char* const partdb_fingerprints[] = {")
    for fi, fp in enumerate(t["fingerprints"]):
        lines.append(f"  {c_escape(fp)}, // {fi}")
    lines.append("};")
    lines.append("")

    # --- vector sets (Tier-3 truth tables) ---
    lines.append("// Tier-3 vector pin lists (per set: inPins then outPins)")
    lines.append("static const uint8_t partdb_vecPins[] = {")
    vec_pin_base = []
    vp = 0
    for vid, v in t["vector_sets"]:
        vec_pin_base.append(vp)
        ins = ", ".join(str(p) for p in v["in_pins"])
        outs = ", ".join(str(p) for p in v["out_pins"])
        lines.append(f"  {ins},   // {vid} in")
        lines.append(f"  {outs},   // {vid} out")
        vp += len(v["in_pins"]) + len(v["out_pins"])
    if vp == 0:
        lines.append("  0, // no vector sets authored")
    lines.append("};")
    lines.append("")
    lines.append("// Tier-3 vector words (per set: inBits, outBits, outCare)")
    lines.append("static const uint16_t partdb_vecWords[] = {")
    vec_word_base = []
    vw = 0
    for vid, v in t["vector_sets"]:
        vec_word_base.append(vw)
        for label, arr in (("inBits", v["in_bits"]),
                           ("outBits", v["out_bits"]),
                           ("outCare", v["out_care"])):
            row = ", ".join(f"0x{b:04X}" for b in arr)
            lines.append(f"  {row},   // {vid} {label}")
        vw += 3 * len(v["in_bits"])
    if vw == 0:
        lines.append("  0, // no vector sets authored")
    lines.append("};")
    lines.append("")
    lines.append("// { supply, numIn, numOut, numSteps, inPins, outPins,")
    lines.append("//   inBits, outBits, outCare }")
    lines.append("const PartDbVectorSet partdb_vectorSets[] = {")
    for k, (vid, v) in enumerate(t["vector_sets"]):
        pb = vec_pin_base[k]
        wb = vec_word_base[k]
        n_in, n_out = len(v["in_pins"]), len(v["out_pins"])
        n_steps = len(v["in_bits"])
        lines.append(
            f"  {{ {v['supply']}, {n_in}, {n_out}, {n_steps}, "
            f"&partdb_vecPins[{pb}], &partdb_vecPins[{pb + n_in}],")
        lines.append(
            f"    &partdb_vecWords[{wb}], &partdb_vecWords[{wb + n_steps}], "
            f"&partdb_vecWords[{wb + 2 * n_steps}] }}, // {k}: {vid}")
    if not t["vector_sets"]:
        lines.append("  { 0, 0, 0, 0, partdb_vecPins, partdb_vecPins,")
        lines.append("    partdb_vecWords, partdb_vecWords, partdb_vecWords"
                     " }, // none authored")
    lines.append("};")
    lines.append("")

    # --- typeStrs ---
    lines.append("const char* const partdb_typeStrs[] = {")
    for ts in t["type_strs"]:
        lines.append(f"  {c_escape(ts)},")
    lines.append("};")
    lines.append("")

    # --- records ---
    lines.append("// { id, displayName, ledName, menuName, desc,")
    lines.append("//   partClass, subClass, typeStrIdx, i2cIdentIdx,"
                 " fingerprintIdx,")
    lines.append("//   pinoutIdx, altPinoutIdx, vectorSetIdx, driverKey,"
                 " defaultValue }")
    lines.append("const PartDbRecord partdb_records[] = {")
    for i, r in enumerate(rec):
        drv = c_escape(r["driver"]) if r["driver"] else "0"
        val = c_escape(r["value"]) if r["value"] else "0"
        lines.append(f"  // {i}: {r['cls_name']}/{r['sub_name']}")
        lines.append(f"  {{ {c_escape(r['id'])}, {c_escape(r['display'])}, "
                     f"{c_escape(r['led'])}, {c_escape(r['menu'])},")
        lines.append(f"    {c_escape(r['desc'])},")
        vec = "0xFFFF" if r["vec_idx"] == 0xFFFF else str(r["vec_idx"])
        lines.append(f"    {r['cls']}, {r['sub']}, {r['type_idx']}, "
                     f"0x{r['ident_idx']:02X}, 0x{r['fp_idx']:02X}, "
                     f"{r['pinout_idx']}, "
                     f"0xFFFF, {vec}, {drv}, {val} }},")
    lines.append("};")
    lines.append("")

    # --- names ---
    lines.append("const PartDbName partdb_names[] = {")
    for n, i in t["names"]:
        lines.append(f"  {{ {c_escape(n)}, {i} }},")
    lines.append("};")
    lines.append("")

    # --- byClass + ranges ---
    lines.append("const uint16_t partdb_byClass[] = {")
    row = []
    for i in t["by_class"]:
        row.append(str(i))
        if len(row) == 16:
            lines.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ", ".join(row) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const PartDbRange partdb_classRanges[PARTDB_NUM_CLASSES]"
                 " = {")
    for ci, (start, count) in enumerate(t["class_ranges"]):
        cname = [k for k, v in CLASSES.items() if v[0] == ci][0]
        lines.append(f"  {{ {start}, {count} }}, // {cname}")
    lines.append("};")
    lines.append("")
    lines.append("const PartDbRange partdb_subclassRanges[PARTDB_NUM_CLASSES"
                 " * PARTDB_MAX_SUBCLASSES] = {")
    for ci in range(NUM_CLASSES):
        cname = [k for k, v in CLASSES.items() if v[0] == ci][0]
        subs = CLASSES[cname][1]
        for si in range(MAX_SUBCLASSES):
            start, count = t["sub_ranges"][ci * MAX_SUBCLASSES + si]
            sname = next((k for k, v in subs.items() if v == si), "(unused)")
            lines.append(f"  {{ {start}, {count} }}, // {cname}/{sname}")
    lines.append("};")
    lines.append("")

    # --- counts ---
    n_pins = sum(len(p[2]) for p in t["pinouts"])
    lines.extend([
        f"const uint16_t partdb_numRecords = {len(rec)};",
        f"const uint16_t partdb_numPinouts = {len(t['pinouts'])};",
        f"const uint16_t partdb_numPins = {n_pins};",
        f"const uint16_t partdb_numNames = {len(t['names'])};",
        f"const uint16_t partdb_numI2cIdents = {len(t['idents'])};",
        f"const uint16_t partdb_numFingerprints = {len(t['fingerprints'])};",
        f"const uint16_t partdb_numVectorSets = {len(t['vector_sets'])};",
        f"const uint16_t partdb_numTypeStrs = {len(t['type_strs'])};",
        "",
        "#endif // PARTDB_DATA_H",
        "",
    ])
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Self-tests - cheap invariants that must hold before anything is written.
# ---------------------------------------------------------------------------

def self_test():
    # The octal-escape rule: a two-line menuName whose second segment starts
    # with a digit must emit \031 (3 digits), never \31 - "\31160" would
    # parse as '\311' + "60".
    assert c_escape("40RGBX " + LINE_BREAK + "160") == '"40RGBX \\031160"'
    assert "\\31" + "1" not in c_escape(LINE_BREAK + "1").replace("\\031", "")
    assert c_escape('a"b\\c') == '"a\\"b\\\\c"'
    # Fold matches the C fold (ASCII-only, a-z -> A-Z).
    assert fold("sn74hc00_x-9") == "SN74HC00_X-9"
    assert fold("µ") == "µ"  # non-ASCII untouched (never appears in ids)
    # Sorted-by-fold really is what a folded strcmp would produce.
    sample = ["b1", "A2", "a1", "B0", "_z", "40RGBX"]
    folded = sorted(sample, key=fold)
    assert folded == sorted(sample, key=lambda s: [ord(c) for c in fold(s)])
    # menuName composition: pad-to-exactly-7 + linebreak.
    m = compose_menu({"menu": "40RGBX|160"}, "40RGBX160", "test")
    assert m == "40RGBX " + LINE_BREAK + "160" and len(m.split(LINE_BREAK)[0]) == 7
    m = compose_menu({}, "HD44780 1602", "test")
    assert m == "HD44780" + LINE_BREAK + "1602"
    assert compose_menu({}, "7400", "test") == "7400"
    # vec parsing: carry-forward inputs, don't-care outputs, bit packing.
    pins14 = ([("1A", 0, 0), ("1B", 0, 0), ("1Y", 0, 0)] +
              [("x", 0, 0)] * 3 + [("GND", PINCLASS_GND, 2)] +
              [("x", 0, 0)] * 6 + [("VCC", PINCLASS_POWER, 1)])
    v = parse_vectors([("1=0 2=0 -> 3=1", "t1"), ("2=1 -> 3=1", "t2"),
                       ("1=1 ->", "t3")], 14, pins14, "t")
    assert v["in_pins"] == [1, 2] and v["out_pins"] == [3]
    assert v["in_bits"] == [0b00, 0b10, 0b11], v["in_bits"]
    assert v["out_bits"] == [1, 1, 0] and v["out_care"] == [1, 1, 0]
    try:
        parse_vectors([("2=1 -> 3=1", "t1"), ("1=0 ->", "t2")], 14,
                      pins14, "t")
        assert False, "late-appearing input must be rejected"
    except SrcError:
        pass
    try:
        parse_vectors([("7=0 -> 3=1", "t1")], 14, pins14, "t")
        assert False, "driving a supply pin must be rejected"
    except SrcError:
        pass


def main():
    self_test()
    print("=" * 70)
    print("Generating PartDbData.h")
    print("=" * 70)

    all_records = []
    for fname in SOURCE_FILES:
        path = DATA_DIR / fname
        if not path.exists():
            raise SrcError(f"missing source file {path}")
        recs = parse_file(path)
        print(f"  {fname}: {len(recs)} records")
        all_records.extend(process_record(r) for r in recs)

    tables = build_tables(all_records)
    sizes = estimate_rodata(tables)
    OUTPUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_HEADER.write_text(emit_header(tables, sizes), encoding="utf-8")

    print()
    print(f"Dedup: {len(tables['records'])} records -> "
          f"{len(tables['pinouts'])} pinouts")
    print(f"Names: {len(tables['names'])} "
          f"(ids + aliases, sorted for binary search)")
    print(f"I2C idents: {len(tables['idents'])}, "
          f"fingerprints: {len(tables['fingerprints'])}, "
          f"vector sets: {len(tables['vector_sets'])}, "
          f"typeStrs: {len(tables['type_strs'])}")
    print(f"Estimated rodata: ~{sizes['total']} bytes")
    if warnings:
        print(f"\n{len(warnings)} warning(s) - see above")
    print(f"\nWrote {OUTPUT_HEADER}")


if __name__ == "__main__":
    try:
        main()
    except SrcError as e:
        print(f"\nERROR: {e}")
        sys.exit(1)
