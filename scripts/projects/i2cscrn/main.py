"""Type to Screen - companion for /projects/i2cscrn/wiring.yaml.

Bring up an I2C OLED wired ANYWHERE on the breadboard, then type at it: tap
each signal (or type its row), pick a driver/size, then a beacon that rescans
until the panel answers - wire it up while it runs. On the way out it removes
exactly the bridges it made. Walkthrough in README.md.

KEEP COMPANION SCRIPTS LEAN. MicroPython compiles them on the device out of a
heap whose size depends on how long the session has been running: measured,
~12 KB of source compiles on a freshly booted board (~39 KB free) and raises
MemoryError on the ~25 KB left deep into a long one. This file is about 12 KB,
so it is at the edge of the good case - prose belongs in README.md, not here.
"""

import sys
import time

_jl_project = globals().get("_jl_project", {})

SDA_PIN = 26           # RP_GPIO_7 \ fixed by the hardware; only the
SCL_PIN = 27           # RP_GPIO_8 / breadboard row is yours to choose
I2C_HZ = 100000
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
    """A typed line once enter arrives, else None. Never blocks, so the probe
    can be watched at the same prompt."""
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
    """Swallow whatever is already buffered before the first prompt. The
    newline that launched this script is still in the stream, and it would
    otherwise answer the GND prompt with "keep the default"."""
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


def ask_row(name, default):
    print("  %-3s  tap its hole, or type a row 1-60 (enter=%d, q=quit)"
          % (name, default))
    say("Tap\n%s" % name)
    while True:
        line = ask()
        if line is not None:
            s = line.strip().lower()
            if s == "":
                print("       %s = row %d (default)" % (name, default))
                return default
            if s in ("q", "quit"):
                raise Abort()
            if s.isdigit() and 1 <= int(s) <= 60:
                print("       %s = row %s (typed)" % (name, s))
                return int(s)
            print("       ? not a row 1-60: " + s)
            continue
        try:
            pad = probe_read(False)
            p = -1 if pad == NO_PAD else int(pad)
        except Exception:
            p = -1
        if 1 <= p <= 60:
            print("       %s = row %d (tapped)" % (name, p))
            return p
        time.sleep(0.02)


def ask_driver():
    print("Panel type:")
    for i, d in enumerate(DRIVERS):
        print("  %d) %s%s" % (i + 1, d[0], "   (default)" if i == 0 else ""))
    say("Panel\ntype?")
    while True:
        line = ask()
        if line is None:
            time.sleep(0.02)
            continue
        s = line.strip().lower() or "1"
        if s in ("q", "quit"):
            raise Abort()
        if s.isdigit() and 1 <= int(s) <= len(DRIVERS):
            d = DRIVERS[int(s) - 1]
            print("driver: %s (%s %dx%d)" % d)
            return d
        print("  ? pick 1-%d" % len(DRIVERS))


# Only what WE made: after a guided build those bridges are the user's circuit.
made = []


def route(rows):
    for name, _d, node in SIGNALS:
        row = rows[name]
        try:
            if is_connected(row, node):
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
            print("  WARNING: outside 3.0-5.5 V. This never changes a rail.")
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
    # Claim, or refreshConnections() re-asserts the slot config's pulls onto
    # GPIO 26/27 underneath the I2C peripheral.
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
        # SH1106 shim 1/2: its pump is 0xAD/0x8B, and it has no 0x20 memory
        # addressing mode command at all.
        pump = (0xAD, 0x8B) if kind == "sh1106" else (0x8D, 0x14, 0x20, 0x00)
        for c in ((0xAE, 0xD5, 0x80, 0xA8, h - 1, 0xD3, 0x00, 0x40) + pump +
                  (0xA1, 0xC8, 0xDA, 0x02 if h == 32 else 0x12, 0x81, 0xCF,
                   0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF)):
            self.cmd(c)

    def cmd(self, c):
        self.i2c.writeto(self.addr, bytes((0x00, c)))

    def show(self):
        if self.kind == "sh1106":
            # Shim 2/2: set the page pointer per page, starting at column 2 -
            # its RAM is 132 wide with the glass wired to 2..129.
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
    """Rescan until something answers, then animate. Returns a Screen.

    The quit check is at the BOTTOM: one real scan always happens before the
    first chance to bail out.
    """
    print("waiting for the panel - wire it up now ('q' + enter to give up)")
    say("Waiting\nfor panel")
    scr = None
    frame = dots = splash_at = 0
    last = None
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
                        print("  (nothing yet - check GND/VCC first, then the"
                              " SDA/SCL order)")
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
    print("Where is the panel? Enter alone keeps the guide's header rows.")
    rows = {}
    for _nm, _df, _nd in SIGNALS:
        rows[_nm] = ask_row(_nm, _df)
    print("assignment: " + "  ".join("%s %d" % (n, rows[n])
                                     for n, _d, _x in SIGNALS))
    label, kind, w, h = ask_driver()
    pins(True)
    route(rows)
    import machine
    i2c = machine.I2C(1, scl=SCL_PIN, sda=SDA_PIN, freq=I2C_HZ)
    scr = beacon(i2c, kind, w, h)

    print("Type a line and press enter. Empty line clears. 'q' quits.")
    history = []
    print("> ", end="")
    while True:
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
    # finally, not just the except arms: q, Ctrl-C, an exception and a clean
    # fall-through all have to leave the board the way we found it.
    if scr is not None:
        try:
            scr.lines([])
        except Exception:
            pass
    unroute()
    pins(False)
    print("bye")
