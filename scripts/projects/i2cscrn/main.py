"""Type to Screen - companion script for /projects/i2cscrn/wiring.yaml

Drives an SSD1306 sitting on the breadboard. The wiring file routes the
panel's SCL row to RP_GPIO_8 (RP pin 27) and its SDA row to RP_GPIO_7
(RP pin 26) - the same pair the built-in I2C scanner uses - so MicroPython's
machine.I2C(1) reaches it directly. Type a line, it lands on the screen.

The panel driver here is deliberately small: framebuf does the pixels and
the 8x8 font, and this file only speaks the SSD1306 command set on top.

Runs standalone from the Files browser too - the launcher injects
_jl_project, but nothing here depends on it.
"""

import time

# The launcher injects this global; default so the script also runs standalone.
_jl_project = globals().get("_jl_project", {})

SDA_PIN = 26        # RP GPIO 26 = node RP_GPIO_7 - wiring.yaml routes row 8 here
SCL_PIN = 27        # RP GPIO 27 = node RP_GPIO_8 - wiring.yaml routes row 7 here
I2C_BUS = 1         # pins 26/27 belong to i2c1; machine.I2C(0, ...) rejects them
I2C_HZ = 100000     # the scanner's rate - kind to breadboard capacitance
ADDR = 0x3C         # 0x3D on a few 128x64 boards; check the module's silkscreen

WIDTH = 128
HEIGHT = 32         # <- set to 64 for a 128x64 panel


class Screen:
    """The smallest useful SSD1306: framebuf for the pixels, this class for
    the command set (SSD1306 datasheet section 9 / 10)."""

    def __init__(self, i2c, addr, width, height):
        import framebuf
        self.i2c = i2c
        self.addr = addr
        self.width = width
        self.height = height
        self.pages = height // 8
        self.buf = bytearray(self.pages * width)
        self.fb = framebuf.FrameBuffer(self.buf, width, height,
                                       framebuf.MONO_VLSB)
        # One pre-allocated transmit buffer: 0x40 ("data follows") + the frame.
        self.tx = bytearray(1 + len(self.buf))
        self.tx[0] = 0x40
        self.rows = self.pages          # 8 px per text row
        self.cols = width // 8          # 8 px per character
        self._init_panel()

    def cmd(self, c):
        # 0x00 = "a command stream follows"
        self.i2c.writeto(self.addr, bytes((0x00, c)))

    def _init_panel(self):
        com_pins = 0x02 if self.height == 32 else 0x12
        for c in (0xAE,                       # display off
                  0xD5, 0x80,                 # clock divide / osc freq
                  0xA8, self.height - 1,      # multiplex ratio
                  0xD3, 0x00,                 # display offset
                  0x40,                       # start line 0
                  0x8D, 0x14,                 # charge pump on
                  0x20, 0x00,                 # horizontal addressing mode
                  0xA1,                       # segment remap
                  0xC8,                       # COM scan direction: flipped
                  0xDA, com_pins,             # COM pin hardware config
                  0x81, 0xCF,                 # contrast
                  0xD9, 0xF1,                 # pre-charge period
                  0xDB, 0x40,                 # VCOMH deselect level
                  0xA4,                       # resume from RAM
                  0xA6,                       # normal (not inverted)
                  0xAF):                      # display on
            self.cmd(c)

    def show(self):
        self.cmd(0x21); self.cmd(0); self.cmd(self.width - 1)      # column range
        self.cmd(0x22); self.cmd(0); self.cmd(self.pages - 1)      # page range
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
    """Returns an I2C object, or None with an explanation printed."""
    try:
        import machine
    except Exception as e:
        print("no machine module in this build: " + str(e))
        return None
    # Claim the two pins first. Without this, every refreshConnections()
    # re-asserts the slot config's pull setting onto GPIO 26/27 and can pull
    # the bus down under the I2C peripheral.
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_claim_pin(n)
        except Exception:
            pass
    try:
        return machine.I2C(I2C_BUS, scl=SCL_PIN, sda=SDA_PIN, freq=I2C_HZ)
    except Exception as e:
        print("could not open I2C on pins %d/%d: %s" % (SDA_PIN, SCL_PIN, str(e)))
        return None


def release_bus():
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_release_pin(n)
        except Exception:
            pass


print("Type to Screen")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "i2cscrn")) +
          "  variant: " + str(_jl_project.get("variant", "default")))

i2c = open_bus()
scr = None
if i2c is not None:
    found = i2c.scan()
    print("i2c devices: " + str([hex(a) for a in found]))
    if ADDR not in found:
        print("no panel at " + hex(ADDR) + " - check power, SDA/SCL order, "
              "and the module's address jumper")
    else:
        try:
            scr = Screen(i2c, ADDR, WIDTH, HEIGHT)
            scr.lines(["Type to Screen", "ready."])
            print("panel up: %dx%d, %d rows of %d chars"
                  % (WIDTH, HEIGHT, scr.rows, scr.cols))
        except Exception as e:
            print("panel init failed: " + str(e))
            scr = None

if scr is None:
    print("running in terminal-only mode - fix the wiring and re-run.")

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
            if scr is not None:
                history = history[-scr.rows:]
            else:
                history = history[-4:]
        if scr is not None:
            try:
                scr.lines(history)
            except Exception as e:
                print("write failed: " + str(e))
        else:
            print("screen: " + str(history))

except KeyboardInterrupt:
    pass

if scr is not None:
    try:
        scr.lines([])
    except Exception:
        pass
release_bus()
print("bye")
