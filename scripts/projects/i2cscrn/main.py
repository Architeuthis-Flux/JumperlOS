"""Type to Screen - companion for /projects/i2cscrn/wiring.yaml.

Bring up an I2C OLED that is wired ANYWHERE on the breadboard, then type at it.

Four things happen, in order:

  1. TAP-TO-ASSIGN. For each of GND / VCC / SCL / SDA the script asks which
     hole it is in. Tap the hole with the probe, or type the row number - both
     work at the same prompt, and pressing enter keeps the default (the guide's
     header rows 5-8). Every probe gesture here has a typed twin; that is what
     makes the flow drivable from a terminal, a script or the HIL.
  2. DRIVER / SIZE. A three-item menu: SSD1306 128x32, SSD1306 128x64,
     SH1106 128x64. The SH1106 is the same framebuffer with a different way of
     pushing it out - see Screen.show().
  3. THE WIRING BEACON. The script then loops: scan the bus, and the moment
     something answers, initialise it and start drawing an animation. So you
     can wire the panel up WHILE this is running and watch it spring to life
     the instant the last wire is right. The terminal shows a quiet heartbeat,
     not an error per attempt.
  4. TYPE TO SCREEN, as before - and if the panel falls off the bus mid-session
     the script drops back to the beacon instead of dying.

On the way out - a typed `q`, Ctrl-C, or the script ending - it blanks the
panel, releases the GPIO claims and REMOVES EXACTLY THE BRIDGES IT MADE. A
route that was already on the board when the script started (a guided build's,
or one you made yourself) is left alone: this script only cleans up after
itself. See README.md.

SCL rides RP_GPIO_8 (RP pin 27) and SDA rides RP_GPIO_7 (RP pin 26), so
machine.I2C(1) reaches the panel. Those are fixed by the hardware; only the
BREADBOARD ROW is yours to choose. framebuf owns the pixels and the 8x8 font;
Screen only adds the command set.
"""

import sys
import time

_jl_project = globals().get("_jl_project", {})

# SCRIPTED INPUT. Set _i2cscrn = {"feed": "5\n6\n7\n8\n1\nq\n"} before exec'ing
# this file and poll_line() serves those lines before it looks at the terminal.
# It is the SAME reader the human path uses, so a scripted run walks the real
# prompts, the real parser and the real assignment - there is no bypass branch
# to rot. That is how test_projects drives this script with no panel attached.
_i2cscrn = globals().get("_i2cscrn", {})

SDA_PIN = 26        # node RP_GPIO_7 - fixed by the hardware
SCL_PIN = 27        # node RP_GPIO_8 - fixed by the hardware
I2C_BUS = 1         # 26/27 are i2c1; machine.I2C(0, ...) rejects them
I2C_HZ = 100000     # kind to breadboard capacitance
ADDRS = (0x3C, 0x3D)   # 0x3D on a few 128x64 boards

# (label, kind, width, height). The first is the default.
DRIVERS = (("SSD1306 128x32", "ssd1306", 128, 32),
           ("SSD1306 128x64", "ssd1306", 128, 64),
           ("SH1106  128x64", "sh1106", 128, 64))

# (signal, default row, the node it has to reach). The default rows are the
# guide's placement - wiring.yaml puts the module header in rows 5-8.
SIGNALS = (("GND", 5, "GND"),
           ("VCC", 6, "TOP_RAIL"),
           ("SCL", 7, "RP_GPIO_8"),
           ("SDA", 8, "RP_GPIO_7"))

RETRY_MS = 300      # beacon: how often to re-scan a bus with nothing on it
FRAME_MS = 80       # beacon: animation frame interval once the panel answers
SPLASH_MS = 2500    # how long to animate before dropping into the type loop


class Abort(Exception):
    """A typed `q`. Handled exactly like Ctrl-C."""


# ---------------------------------------------------------------------------
# One input reader for everything: scripted feed, then the terminal.
# ---------------------------------------------------------------------------

_feed = _i2cscrn.get("feed", "")
try:
    import select
    _poll = select.poll()
    _poll.register(sys.stdin, select.POLLIN)
except Exception:
    _poll = None
_inbuf = ""

# With no poller and no feed there is no way to watch the probe and the
# terminal at once, so the prompts fall back to a blocking input(). The typed
# twin still works; only the tap does not.
TYPED_ONLY = (_poll is None and not _feed)


def poll_line():
    """A typed line (no newline) once enter arrives, else None. Never blocks."""
    global _inbuf, _feed
    if _feed:
        nl = _feed.find("\n")
        if nl < 0:
            line, _feed = _feed, ""
        else:
            line, _feed = _feed[:nl], _feed[nl + 1:]
        line = line.replace("\r", "")
        print(line)
        return line
    if _poll is None:
        return None
    try:
        if not _poll.poll(0):
            return None
        ch = sys.stdin.read(1)
    except Exception:
        return None
    if not ch:
        return None
    if ch == "\x03":                 # Ctrl-C
        raise KeyboardInterrupt
    if ch in ("\n", "\r"):
        line, _inbuf = _inbuf, ""
        print()
        return line
    if ch in ("\x08", "\x7f"):       # backspace
        if _inbuf:
            _inbuf = _inbuf[:-1]
            print("\b \b", end="")
        return None
    _inbuf += ch
    print(ch, end="")                # echo
    return None


def read_line_blocking(prompt):
    """The TYPED_ONLY fallback."""
    try:
        return input(prompt)
    except EOFError:
        raise Abort()


def tapped_row():
    """The row the probe just touched, or None. Never blocks."""
    if TYPED_ONLY:
        return None
    try:
        pad = probe_read(False)
        if pad == NO_PAD:
            return None
        p = int(pad)
    except Exception:
        return None
    return p if 1 <= p <= 60 else -1     # -1 = touched, but not a row


def oled_say(text):
    """Mirror onto the board's own OLED if it has one. Never fatal."""
    try:
        oled_print(text)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# 1. Tap-to-assign
# ---------------------------------------------------------------------------

def ask_row(name, default):
    print("  %-3s  tap its hole with the probe, or type a row 1-60 "
          "(enter = %d, q = quit)" % (name, default))
    oled_say("Tap\n%s" % name)
    while True:
        line = (read_line_blocking("  %s> " % name) if TYPED_ONLY
                else poll_line())
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
            print("       ? %r is not a row 1-60" % line)
            continue
        p = tapped_row()
        if p is not None:
            if p < 0:
                print("       that pad is not a breadboard hole - tap 1-60")
            else:
                print("       %s = row %d (tapped)" % (name, p))
                return p
        time.sleep(0.02)


def assign():
    print("Where is the panel? Enter alone keeps the guide's header rows.")
    rows = {}
    for name, default, _node in SIGNALS:
        rows[name] = ask_row(name, default)
    print("assignment: " + "  ".join("%s %d" % (n, rows[n])
                                     for n, _d, _x in SIGNALS))
    return rows


# ---------------------------------------------------------------------------
# 2. Driver / size
# ---------------------------------------------------------------------------

def ask_driver():
    print("Panel type:")
    for i, d in enumerate(DRIVERS):
        print("  %d) %s%s" % (i + 1, d[0], "   (default)" if i == 0 else ""))
    oled_say("Panel\ntype?")
    while True:
        line = (read_line_blocking("  driver> ") if TYPED_ONLY
                else poll_line())
        if line is None:
            time.sleep(0.02)
            continue
        s = line.strip().lower()
        if s == "":
            s = "1"
        if s in ("q", "quit"):
            raise Abort()
        if s.isdigit() and 1 <= int(s) <= len(DRIVERS):
            d = DRIVERS[int(s) - 1]
            print("driver: %s (%s %dx%d)" % (d[0], d[1], d[2], d[3]))
            return d
        print("  ? pick 1-%d" % len(DRIVERS))


# ---------------------------------------------------------------------------
# Routing - and the record of what WE made, so the teardown cannot take
# anything that was already there (a guided build's bridges are the user's
# saved circuit; they survive).
# ---------------------------------------------------------------------------

made = []


def route(rows):
    for name, _default, node in SIGNALS:
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
            print("  could not route %s row %d -> %s: %s"
                  % (name, row, node, str(e)))
    print("routed=%d" % len(made))


def rail_note():
    """Report the top rail, never set it. Writing a rail here would dirty the
    active context and rewrite the user's file - the defect class W3-T5 just
    closed. If the rail is wrong, that is the user's call to make."""
    try:
        v = dac_get("TOP_RAIL")
    except Exception:
        return
    print("top rail: %.2f V" % v)
    if v < 3.0 or v > 5.5:
        print("  WARNING: outside 3.0-5.5 V - most panels want 3.3 V or 5 V. "
              "This script never changes a rail; set it yourself if it is wrong.")


def unroute():
    n = 0
    while made:
        row, node = made.pop()
        try:
            disconnect(row, node)
            n += 1
        except Exception as e:
            print("  could not un-route %d -> %s: %s" % (row, node, str(e)))
    print("unrouted=%d" % n)


def claim_pins():
    # Claim the pins, or every refreshConnections() re-asserts the slot
    # config's pulls onto GPIO 26/27 underneath the I2C peripheral.
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_claim_pin(n)
        except Exception:
            pass


def release_pins():
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_release_pin(n)
        except Exception:
            pass


def open_bus():
    """An I2C object, or None with an explanation printed."""
    try:
        import machine
    except Exception as e:
        print("no machine module: " + str(e))
        return None
    try:
        return machine.I2C(I2C_BUS, scl=SCL_PIN, sda=SDA_PIN, freq=I2C_HZ)
    except Exception as e:
        print("no I2C on %d/%d: %s" % (SDA_PIN, SCL_PIN, str(e)))
        return None


# ---------------------------------------------------------------------------
# The panel
# ---------------------------------------------------------------------------

class Screen:
    def __init__(self, i2c, addr, kind, width, height):
        import framebuf
        self.i2c = i2c
        self.addr = addr
        self.kind = kind
        self.width = width
        self.height = height
        self.pages = height // 8
        self.buf = bytearray(self.pages * width)
        self.fb = framebuf.FrameBuffer(self.buf, width, height,
                                       framebuf.MONO_VLSB)
        self.tx = bytearray(1 + len(self.buf))
        self.tx[0] = 0x40             # "data follows"
        self.rows = self.pages        # 8 px per text row
        self.cols = width // 8        # 8 px per character
        for c in self._init_cmds():
            self.cmd(c)

    def _init_cmds(self):
        h = self.height
        # off, clkdiv, mux, offset, startline, pump, (mode), seg remap, com
        # scan dec, com pins, contrast, precharge, vcomh, RAM, normal, on
        if self.kind == "sh1106":
            # THE WHOLE SH1106 SHIM, half 1 of 2: its charge pump is 0xAD/0x8B
            # rather than 0x8D/0x14, and it has NO 0x20 memory-addressing-mode
            # command at all. Everything else is the same controller family.
            return (0xAE, 0xD5, 0x80, 0xA8, h - 1, 0xD3, 0x00, 0x40,
                    0xAD, 0x8B, 0xA1, 0xC8,
                    0xDA, 0x02 if h == 32 else 0x12,
                    0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF)
        return (0xAE, 0xD5, 0x80, 0xA8, h - 1, 0xD3, 0x00, 0x40,
                0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
                0xDA, 0x02 if h == 32 else 0x12,
                0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF)

    def cmd(self, c):
        self.i2c.writeto(self.addr, bytes((0x00, c)))   # 0x00 = command

    def show(self):
        if self.kind == "sh1106":
            # Half 2 of 2: with no addressing mode the pointer has to be set
            # per page, and the SH1106's RAM is 132 columns wide with the glass
            # wired to columns 2..129 - hence the column-low nibble of 2. Miss
            # that and the whole image sits two pixels to the left, wrapped.
            mv = memoryview(self.tx)
            for page in range(self.pages):
                self.cmd(0xB0 | page)     # page address
                self.cmd(0x02)            # column low nibble  = 2 (the offset)
                self.cmd(0x10)            # column high nibble = 0
                start = page * self.width
                self.tx[1:1 + self.width] = self.buf[start:start + self.width]
                self.i2c.writeto(self.addr, mv[:1 + self.width])
            return
        self.cmd(0x21); self.cmd(0); self.cmd(self.width - 1)   # columns
        self.cmd(0x22); self.cmd(0); self.cmd(self.pages - 1)   # pages
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
        """The wiring beacon's animation - a sweeping bar under the name."""
        w, h = self.width, self.height
        self.fb.fill(0)
        self.fb.text("JUMPERLESS", max(0, (w - 80) // 2), 0, 1)
        self.fb.text("type to screen", max(0, (w - 112) // 2), 10, 1)
        span = w + 32
        x = (frame * 8) % span - 32
        self.fb.fill_rect(x, h - 6, 32, 4, 1)
        self.fb.hline(0, h - 1, w, 1)
        self.show()


# ---------------------------------------------------------------------------
# 3. The wiring beacon
# ---------------------------------------------------------------------------

def try_open(i2c, kind, width, height):
    """One scan + init attempt. A Screen on success, None on any failure."""
    try:
        found = i2c.scan()
    except Exception:
        return None
    addr = None
    for a in ADDRS:
        if a in found:
            addr = a
            break
    if addr is None:
        return None
    try:
        scr = Screen(i2c, addr, kind, width, height)
    except Exception as e:
        print("\npanel answered at %s but would not init: %s"
              % (hex(addr), str(e)))
        return None
    print("\npanel up at %s: %dx%d, %d rows of %d chars"
          % (hex(addr), width, height, scr.rows, scr.cols))
    oled_say("Panel\nup!")
    return scr


def beacon(i2c, kind, width, height):
    """Loop until something answers on the bus, then animate. Returns a Screen.

    Wire the panel WHILE this runs. Every failure is a 300 ms retry and a dot,
    never a stack trace - the terminal stays readable while your hands are on
    the breadboard.

    The quit check is at the BOTTOM of the loop on purpose: one scan always
    happens before the first chance to bail, so a scripted run exercises the
    real scan path rather than short-circuiting past it.
    """
    print("waiting for the panel - wire it up now ('q' + enter to give up)")
    oled_say("Waiting\nfor panel")
    scr = None
    frame = 0
    dots = 0
    splash_started = 0
    last_try = None
    while True:
        now = time.ticks_ms()
        if scr is None:
            nap = 0.02
            if last_try is None or time.ticks_diff(now, last_try) >= RETRY_MS:
                last_try = now
                scr = try_open(i2c, kind, width, height)
                if scr is None:
                    dots += 1
                    print(".", end="")
                    if dots % 40 == 0:
                        print("  (still nothing at %s - check GND/VCC first, "
                              "then the SDA/SCL order)"
                              % "/".join(hex(a) for a in ADDRS))
                else:
                    splash_started = time.ticks_ms()
                    frame = 0
        else:
            try:
                scr.splash(frame)
            except Exception as e:
                print("panel went away (%s) - back to waiting" % str(e))
                scr = None
                dots = 0
                last_try = None
                nap = 0.02
            else:
                frame += 1
                if time.ticks_diff(time.ticks_ms(), splash_started) >= SPLASH_MS:
                    return scr
                nap = FRAME_MS / 1000.0

        line = poll_line()
        if line is not None and line.strip().lower() in ("q", "quit"):
            raise Abort()
        time.sleep(nap)


# ---------------------------------------------------------------------------
# 4. Type to screen
# ---------------------------------------------------------------------------

def type_loop(i2c, scr, kind, width, height):
    print("Type a line and press enter. Empty line clears. 'q' quits.")
    history = []
    if not TYPED_ONLY:
        print("> ", end="")
    while True:
        typed = (read_line_blocking("> ") if TYPED_ONLY else poll_line())
        if typed is None:
            time.sleep(0.02)
            continue
        if typed.strip().lower() in ("q", "quit"):
            return
        if typed == "":
            history = []
        else:
            history.append(typed)
            history = history[-scr.rows:]
        try:
            scr.lines(history)
        except Exception as e:
            print("write failed (%s) - back to the beacon" % str(e))
            scr = beacon(i2c, kind, width, height)
            try:
                scr.lines(history)
            except Exception:
                pass
        if not TYPED_ONLY:
            print("> ", end="")


# ---------------------------------------------------------------------------

print("Type to Screen")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "i2cscrn")))
if TYPED_ONLY:
    print("note: no non-blocking stdin here - prompts are type-only "
          "(the probe cannot be watched at the same time)")

scr = None
i2c = None
try:
    rows = assign()
    label, kind, width, height = ask_driver()
    claim_pins()
    route(rows)
    rail_note()
    i2c = open_bus()
    if i2c is None:
        print("terminal-only mode - no I2C peripheral to talk through.")
    else:
        scr = beacon(i2c, kind, width, height)
        type_loop(i2c, scr, kind, width, height)
except (KeyboardInterrupt, Abort):
    pass
finally:
    # finally, not just the except arm: a `q`, a Ctrl-C, an unexpected
    # exception and a clean fall-through all have to leave the board the way
    # we found it. Blank the panel, drop the GPIO claims, and remove EXACTLY
    # the bridges route() made - nothing that was already there.
    if scr is not None:
        try:
            scr.lines([])
        except Exception:
            pass
    unroute()
    release_pins()
    print("bye")
