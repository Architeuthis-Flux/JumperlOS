"""Type to Screen - companion for /projects/i2cscrn/wiring.yaml, see README.md.

An I2C OLED wired anywhere on the breadboard, typed at. Every prompt answers to
probe, wheel AND terminal, and only the bridges made here are removed.

KEEP UNDER ~13 KB - it is compiled on-device from one never-resetting heap, and
15 KB raises MemoryError even on a fresh boot. Prose goes in README.md.
"""

import sys
import time

_jl_project = globals().get("_jl_project", {})

SDA_PIN = 26           # RP_GPIO_7 \ fixed by the hardware; only the
SCL_PIN = 27           # RP_GPIO_8 / breadboard row is yours to choose
I2C_HZ = 100000
# Must clear a full frame: 513 B is ~46 ms at 100 kHz vs a 50 ms default -
# that 4 ms of margin was the ETIMEDOUT "panel went away" loop. Sum in README.
I2C_TIMEOUT_US = 400000
ADDRS = (0x3C, 0x3D)

# label, kind, width, height. First is the default.
DRIVERS = (("SSD1306 128x32", "ssd1306", 128, 32),
           ("SSD1306 128x64", "ssd1306", 128, 64),
           ("SH1106  128x64", "sh1106", 128, 64))

# signal, default row (the guide's placement), node it must reach.
SIGNALS = (("GND", 5, "GND"), ("VCC", 6, "TOP_RAIL"),
           ("SCL", 7, "RP_GPIO_8"), ("SDA", 8, "RP_GPIO_7"))

RETRY_MS = 300         # beacon rescan interval
SPLASH_MS = 2500       # animate this long before the type loop


class Abort(Exception):
    pass


try:
    import select
    _poll = select.poll()
    _poll.register(sys.stdin, select.POLLIN)
except Exception:
    _poll = None
_inbuf = ""
_lastcr = False


def ask():
    """A typed line once enter arrives, else None. Never blocks."""
    global _inbuf, _lastcr
    if _poll is None:
        return input()
    try:
        if not _poll.poll(0):
            return None
        ch = sys.stdin.read(1)
    except Exception:
        return None
    if not ch:
        return None
    if ch == "\x03":
        raise KeyboardInterrupt
    if ch in ("\n", "\r"):
        # A terminal that sends CRLF must not count as two Enters - the second
        # one would silently take the default for the NEXT signal.
        if ch == "\n" and _lastcr:
            _lastcr = False
            return None
        _lastcr = (ch == "\r")
        line, _inbuf = _inbuf, ""
        print()
        return line
    _lastcr = False
    if ch in ("\x08", "\x7f"):
        if _inbuf:
            _inbuf = _inbuf[:-1]
            print("\b \b", end="")
        return None
    _inbuf += ch
    print(ch, end="")
    return None


def drain():
    """Swallow buffered input - the launching newline would answer prompt 1."""
    global _inbuf, _lastcr
    if _poll is None:
        return
    end = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), end) < 200:
        try:
            if _poll.poll(0):
                sys.stdin.read(1)
                continue
        except Exception:
            break
        time.sleep(0.01)
    _inbuf, _lastcr = "", False


def say(text):
    try:
        oled_print(text)
    except Exception:
        pass


_wbtn = 0


HELDS = (2, 5, 6)      # HELD, LONG_HELD, MEDIUM_HELD - all mean "quit"


def wheel():
    """'up'|'down'|'click'|'hold'|None. up=-1, down=+1, click takes, hold quits.

    Click is edge-triggered on ENTERING released, not on having seen PRESSED:
    the injected press writes RELEASED straight out. Ending a hold is no click.
    """
    global _wbtn
    try:
        d = clickwheel_get_direction()
        b = clickwheel_get_button()
    except Exception:
        return None
    if d == 1:
        return "up"
    if d == 2:
        return "down"
    was, _wbtn = _wbtn, b
    if b in HELDS:
        return "hold"
    if b == 3 and was != 3:
        return None if was in HELDS else "click"
    return None


def choose(lo, hi, start, render, probe=False, dflt=None):
    """The loop both prompts share: wheel turns/clicks, probe taps (rows only),
    terminal types, enter takes the default. Wraps, never runs off the end.
    Returns (value, which surface answered) - the provenance is worth saying
    out loud, and it is what the HIL suite reads to know a typed answer landed
    as typed."""
    v = start
    render(v)
    while True:
        w = wheel()
        if w == "hold":
            raise Abort()
        if w == "up" or w == "down":
            v = lo + (v - lo + (-1 if w == "up" else 1)) % (hi - lo + 1)
            render(v)
            continue
        if w == "click":
            return v, "wheel"
        line = ask()
        if line is not None:
            t = line.strip().lower()
            if t in ("q", "quit"):
                raise Abort()
            if t == "":
                return (dflt if dflt is not None else v), "default"
            if t.isdigit() and lo <= int(t) <= hi:
                return int(t), "typed"
            print("  ? %d-%d, or use the wheel" % (lo, hi))
        if probe:
            try:
                pad = probe_read(False)
                pv = -1 if pad == NO_PAD else int(pad)
            except Exception:
                pv = -1
            if lo <= pv <= hi:
                return pv, "tapped"
        time.sleep(0.02)


def ask_row(name, default):
    print("  %-3s  tap it, turn+click the wheel, or type 1-60 (enter=%d, q)"
          % (name, default))

    def show(v):
        say("Tap %s\nor row %d" % (name, v))
        print("       %s -> row %-3d\r" % (name, v), end="")

    r, how = choose(1, 60, default, show, True, default)
    print("\n       %s = row %d (%s)" % (name, r, how))
    return r


def ask_driver():
    print("Panel type: turn the wheel and click, or type 1-%d" % len(DRIVERS))
    for n, d in enumerate(DRIVERS):
        print("  %d) %s%s" % (n + 1, d[0], "   (default)" if n == 0 else ""))

    def show(v):
        say("Panel?\n%s" % DRIVERS[v - 1][0])
        print("   > %-16s\r" % DRIVERS[v - 1][0], end="")

    d = DRIVERS[choose(1, len(DRIVERS), 1, show, False, 1)[0] - 1]
    print("\ndriver: %s (%s %dx%d)" % d)
    return d


# Only what WE made - after a guided build the rest is the user's circuit.
made = []


def route(rows):
    for name, _d, node in SIGNALS:
        row = rows[name]
        try:
            if is_connected(row, node):
                # Wording is a contract: test_projects reads this line, and
                # "left alone" is what tells the user their wiring survived.
                print("  %s row %d -> %s was already routed - left alone"
                      % (name, row, node))
                continue
            connect(row, node)
            made.append((row, node))
            print("  %s row %d -> %s" % (name, row, node))
        except Exception as e:
            print("  could not route %s: %s" % (name, str(e)))
    print("routed=%d" % len(made))
    try:
        v = dac_get("TOP_RAIL")
        print("top rail: %.2f V" % v)
        if v < 3.0 or v > 5.5:
            print("  WARNING: outside 3.0-5.5 V (this never sets a rail)")
    except Exception:
        pass


def unroute():
    n = 0
    while made:
        row, node = made.pop()
        try:
            disconnect(row, node)
            n += 1
        except Exception:
            pass
    print("unrouted=%d" % n)


def pins(claim):
    # Or refreshConnections() re-asserts config pulls under the peripheral.
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_claim_pin(n) if claim else gpio_release_pin(n)
        except Exception:
            pass


class Screen:
    def __init__(self, i2c, addr, kind, w, h):
        import framebuf
        self.i2c, self.addr, self.kind = i2c, addr, kind
        self.w, self.h, self.pages = w, h, h // 8
        self.buf = bytearray(self.pages * w)
        self.fb = framebuf.FrameBuffer(self.buf, w, h, framebuf.MONO_VLSB)
        self.tx = bytearray(1 + len(self.buf))
        self.tx[0] = 0x40                    # "data follows"
        self.rows, self.cols = self.pages, w // 8
        # SH1106 shim 1/2: pump is 0xAD/0x8B and it has no 0x20 command.
        pump = (0xAD, 0x8B) if kind == "sh1106" else (0x8D, 0x14, 0x20, 0x00)
        for c in ((0xAE, 0xD5, 0x80, 0xA8, h - 1, 0xD3, 0x00, 0x40) + pump +
                  (0xA1, 0xC8, 0xDA, 0x02 if h == 32 else 0x12, 0x81, 0xCF,
                   0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF)):
            self.cmd(c)

    def cmd(self, c):
        self.i2c.writeto(self.addr, bytes((0x00, c)))

    def show(self):
        if self.kind == "sh1106":
            # Shim 2/2: page pointer per page from column 2 (132-wide RAM).
            mv = memoryview(self.tx)
            for pg in range(self.pages):
                self.cmd(0xB0 | pg); self.cmd(0x02); self.cmd(0x10)
                s = pg * self.w
                self.tx[1:1 + self.w] = self.buf[s:s + self.w]
                self.i2c.writeto(self.addr, mv[:1 + self.w])
            return
        self.cmd(0x21); self.cmd(0); self.cmd(self.w - 1)
        self.cmd(0x22); self.cmd(0); self.cmd(self.pages - 1)
        self.tx[1:] = self.buf
        self.i2c.writeto(self.addr, self.tx)

    def lines(self, texts):
        self.fb.fill(0)
        y = 0
        for t in texts:
            self.fb.text(t[:self.cols], 0, y, 1)
            y += 8
        self.show()

    def splash(self, frame):
        self.fb.fill(0)
        self.fb.text("JUMPERLESS", max(0, (self.w - 80) // 2), 0, 1)
        self.fb.text("type to screen", max(0, (self.w - 112) // 2), 10, 1)
        self.fb.fill_rect((frame * 8) % (self.w + 32) - 32, self.h - 6, 32, 4, 1)
        self.show()


def beacon(i2c, kind, w, h):
    """Rescan until answered, then animate. Quit check is at the BOTTOM."""
    print("waiting for the panel - wire it up now (hold the wheel or 'q' to"
          " give up)")
    say("Waiting\nfor panel")
    scr = None
    frame = dots = splash_at = 0
    last = None
    stuck = False
    while True:
        nap = 0.02
        if scr is None:
            now = time.ticks_ms()
            if last is None or time.ticks_diff(now, last) >= RETRY_MS:
                last = now
                try:
                    found = i2c.scan()
                except Exception:
                    found = []
                if len(found) > 8:
                    # Not a bus full of chips - SDA cannot rise, so the master
                    # reads its own low back as an ACK from everyone.
                    if not stuck:
                        stuck = True
                        print("\nevery address answered - SDA stuck low, not"
                              " %d devices; check SDA's row and its pull-up"
                              % len(found))
                    found = []
                addr = None
                for a in ADDRS:
                    if a in found:
                        addr = a
                if addr is not None:
                    try:
                        scr = Screen(i2c, addr, kind, w, h)
                        print("\npanel up at %s: %dx%d, %d rows of %d chars"
                              % (hex(addr), w, h, scr.rows, scr.cols))
                        say("Panel\nup!")
                        splash_at, frame = time.ticks_ms(), 0
                    except Exception as e:
                        print("\n%s would not init: %s" % (hex(addr), str(e)))
                        scr = None
                if scr is None:
                    dots += 1
                    print(".", end="")
                    if dots % 40 == 0:
                        print("  (nothing yet - check GND/VCC, then SDA/SCL"
                              " order)")
        else:
            try:
                scr.splash(frame)
            except Exception as e:
                print("panel went away (%s) - back to waiting" % str(e))
                scr, dots, last = None, 0, None
            else:
                frame += 1
                if time.ticks_diff(time.ticks_ms(), splash_at) >= SPLASH_MS:
                    return scr
                nap = 0.08
        if wheel() == "hold":
            raise Abort()
        line = ask()
        if line is not None and line.strip().lower() in ("q", "quit"):
            raise Abort()
        time.sleep(nap)


print("Type to Screen")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "i2cscrn")))

scr = None
try:
    drain()
    print("Where is the panel? Enter alone keeps the guide's rows.")
    rows = {}
    for _nm, _df, _nd in SIGNALS:
        rows[_nm] = ask_row(_nm, _df)
    print("assignment: " + "  ".join("%s %d" % (n, rows[n])
                                     for n, _d, _x in SIGNALS))
    label, kind, w, h = ask_driver()
    pins(True)
    route(rows)
    import machine
    i2c = machine.I2C(1, scl=SCL_PIN, sda=SDA_PIN, freq=I2C_HZ,
                      timeout=I2C_TIMEOUT_US)
    scr = beacon(i2c, kind, w, h)

    print("Type a line + enter. Empty clears. 'q' or hold quits.")
    history = []
    print("> ", end="")
    while True:
        if wheel() == "hold":
            break
        typed = ask()
        if typed is None:
            time.sleep(0.02)
            continue
        if typed.strip().lower() in ("q", "quit"):
            break
        if typed == "":
            history = []
        else:
            history.append(typed)
            history = history[-scr.rows:]
        try:
            scr.lines(history)
        except Exception as e:
            print("write failed (%s) - back to the beacon" % str(e))
            scr = beacon(i2c, kind, w, h)
        print("> ", end="")
except (KeyboardInterrupt, Abort):
    pass
except Exception as e:
    print("stopped: " + str(e))
finally:
    # finally, not the except arms: every exit path restores the board.
    if scr is not None:
        try:
            scr.lines([])
        except Exception:
            pass
    unroute()
    pins(False)
    print("bye")
