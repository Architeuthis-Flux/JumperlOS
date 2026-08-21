"""Type to Screen - companion for /projects/i2cscrn/wiring.yaml.

SCL row -> RP_GPIO_8 (RP pin 27), SDA row -> RP_GPIO_7 (RP pin 26), so
machine.I2C(1) reaches the panel. framebuf owns the pixels and the 8x8
font; Screen only adds the SSD1306 command set. See README.md.
"""

_jl_project = globals().get("_jl_project", {})

SDA_PIN = 26        # node RP_GPIO_7 - wiring.yaml routes row 8 here
SCL_PIN = 27        # node RP_GPIO_8 - wiring.yaml routes row 7 here
I2C_BUS = 1         # 26/27 are i2c1; machine.I2C(0, ...) rejects them
I2C_HZ = 100000     # the scanner's rate - kind to breadboard capacitance
ADDR = 0x3C         # 0x3D on a few 128x64 boards
WIDTH = 128
HEIGHT = 32         # <- set to 64 for a 128x64 panel


class Screen:
    def __init__(self, i2c, addr, width, height):
        import framebuf
        self.i2c = i2c
        self.addr = addr
        self.width = width
        self.pages = height // 8
        self.buf = bytearray(self.pages * width)
        self.fb = framebuf.FrameBuffer(self.buf, width, height,
                                       framebuf.MONO_VLSB)
        self.tx = bytearray(1 + len(self.buf))
        self.tx[0] = 0x40             # "data follows"
        self.rows = self.pages        # 8 px per text row
        self.cols = width // 8        # 8 px per character
        # off, clkdiv, mux, offset, startline, pump, horiz mode, seg remap,
        # com scan dec, com pins, contrast, precharge, vcomh, RAM, normal, on
        for c in (0xAE, 0xD5, 0x80, 0xA8, height - 1, 0xD3, 0x00, 0x40,
                  0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
                  0xDA, 0x02 if height == 32 else 0x12,
                  0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF):
            self.cmd(c)

    def cmd(self, c):
        self.i2c.writeto(self.addr, bytes((0x00, c)))   # 0x00 = command

    def show(self):
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


def open_bus():
    """An I2C object, or None with an explanation printed."""
    try:
        import machine
    except Exception as e:
        print("no machine module: " + str(e))
        return None
    # Claim the pins, or every refreshConnections() re-asserts the slot
    # config's pulls onto GPIO 26/27 underneath the I2C peripheral.
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_claim_pin(n)
        except Exception:
            pass
    try:
        return machine.I2C(I2C_BUS, scl=SCL_PIN, sda=SDA_PIN, freq=I2C_HZ)
    except Exception as e:
        print("no I2C on %d/%d: %s" % (SDA_PIN, SCL_PIN, str(e)))
        return None


print("Type to Screen")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "i2cscrn")))

scr = None
i2c = open_bus()
if i2c is not None:
    found = i2c.scan()
    print("i2c devices: " + str([hex(a) for a in found]))
    if ADDR not in found:
        print("no panel at " + hex(ADDR) + " - check power, the SDA/SCL "
              "order, and the address jumper")
    else:
        try:
            scr = Screen(i2c, ADDR, WIDTH, HEIGHT)
            scr.lines(["Type to Screen", "ready."])
            print("panel up: %dx%d, %d rows of %d chars"
                  % (WIDTH, HEIGHT, scr.rows, scr.cols))
        except Exception as e:
            print("panel init failed: " + str(e))
if scr is None:
    print("terminal-only mode - fix the wiring and re-run.")
print("Type a line and press enter. Empty line clears. 'q' quits.")

history = []
try:
    while True:
        try:
            typed = input("> ")
        except EOFError:
            break
        if typed == "q":
            break
        if typed == "":
            history = []
        else:
            history.append(typed)
            history = history[-(scr.rows if scr else 4):]
        if scr is None:
            print("screen: " + str(history))
        else:
            try:
                scr.lines(history)
            except Exception as e:
                print("write failed: " + str(e))
except KeyboardInterrupt:
    pass

if scr is not None:
    try:
        scr.lines([])
    except Exception:
        pass
for n in (GPIO_7, GPIO_8):
    try:
        gpio_release_pin(n)
    except Exception:
        pass
print("bye")
