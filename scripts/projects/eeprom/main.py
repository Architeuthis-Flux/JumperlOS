"""EEPROM Dumper - companion script for /projects/eeprom/wiring.yaml

Reads a 24Cxx I2C EEPROM over machine.I2C(1) - the wiring file routes the
chip's SDA row to RP_GPIO_7 (RP pin 26) and its SCL row to RP_GPIO_8 (RP
pin 27) - and hex-dumps it to the terminal.

Write protect is not a jumper here. WP (pin 7, row 36) is wired to the top
rail, so the chip powers up read-only; the optional write test re-routes
that row to GND for the duration of one byte and puts it straight back.
That re-route is why the bus object is rebuilt afterwards: a routing change
runs refreshConnections(), which re-asserts the slot config onto GPIO 26/27.

Runs standalone from the Files browser too - the launcher injects
_jl_project, but nothing here depends on it.
"""

import time

# The launcher injects this global; default so the script also runs standalone.
_jl_project = globals().get("_jl_project", {})

SDA_PIN = 26        # RP GPIO 26 = node RP_GPIO_7 - wiring.yaml routes row 38 here
SCL_PIN = 27        # RP GPIO 27 = node RP_GPIO_8 - wiring.yaml routes row 37 here
I2C_BUS = 1         # pins 26/27 belong to i2c1; machine.I2C(0, ...) rejects them
I2C_HZ = 100000

ADDR = 0x50         # A0/A1/A2 are all grounded by the wiring
ADDR_BITS = 8       # 24C01..24C16 use an 8-bit word address; 24C32 and up use 16
WP_ROW = 36         # 24Cxx pin 7, held at the top rail = write protected
FIRST = 256         # bytes in the opening dump
TEST_AT = 0xFF      # the one address the optional write test touches

PRINTABLE = ("................................"
             " !\"#$%&'()*+,-./0123456789:;<=>?"
             "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
             "`abcdefghijklmnopqrstuvwxyz{|}~.")


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


def hexdump(data, base=0):
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join("%02X" % b for b in chunk)
        text = "".join(PRINTABLE[b] if b < 128 else "." for b in chunk)
        print("%04X  %-47s  |%s|" % (base + off, hexpart, text))


def read_block(i2c, start, count):
    """Sequential read. 32 bytes at a time keeps the buffers small."""
    out = bytearray()
    while count > 0:
        n = 32 if count > 32 else count
        out += i2c.readfrom_mem(ADDR, start, n, addrsize=ADDR_BITS)
        start += n
        count -= n
    return out


def ask(prompt):
    try:
        return input(prompt).strip()
    except EOFError:
        return ""


def write_test(i2c, original):
    """One byte written, read back, and put back. Returns the live bus."""
    probe = 0xA5 if original != 0xA5 else 0x5A
    moved = False
    try:
        disconnect(WP_ROW, TOP_RAIL)
        connect(WP_ROW, GND)
        moved = True
        time.sleep(0.05)
        # The re-route ran a refresh, which re-asserts the GPIO config onto
        # pins 26/27 - rebuild the bus before touching the chip again.
        i2c = open_bus()
        if i2c is None:
            raise OSError("bus gone after the WP re-route")

        i2c.writeto_mem(ADDR, TEST_AT, bytes((probe,)), addrsize=ADDR_BITS)
        time.sleep(0.01)                 # 24Cxx write cycle: 5 ms worst case
        back = i2c.readfrom_mem(ADDR, TEST_AT, 1, addrsize=ADDR_BITS)[0]
        if back == probe:
            print("wrote 0x%02X to 0x%02X and read it back - writes work."
                  % (probe, TEST_AT))
        else:
            print("wrote 0x%02X but read 0x%02X back - it did not stick."
                  % (probe, back))

        i2c.writeto_mem(ADDR, TEST_AT, bytes((original,)), addrsize=ADDR_BITS)
        time.sleep(0.01)
        print("restored 0x%02X at address 0x%02X." % (original, TEST_AT))
    except Exception as e:
        print("write test failed: " + str(e))
    finally:
        if moved:
            try:
                disconnect(WP_ROW, GND)
                connect(WP_ROW, TOP_RAIL)
                time.sleep(0.05)
                i2c = open_bus()
                print("write protect restored (row %d back on the top rail)."
                      % WP_ROW)
            except Exception as e:
                print("COULD NOT RESTORE WRITE PROTECT: " + str(e))
                print("re-load the project wiring before trusting the chip.")
    return i2c


def main():
    print("EEPROM Dumper")
    if _jl_project:
        print("project: " + str(_jl_project.get("dir", "eeprom")) +
              "  variant: " + str(_jl_project.get("variant", "default")))

    i2c = open_bus()
    if i2c is None:
        print("no bus - nothing to do.")
        return

    try:
        found = i2c.scan()
        print("i2c devices: " + str([hex(a) for a in found]))
        if ADDR not in found:
            print("no chip at " + hex(ADDR) + " - check the rail, the pull-ups "
                  "(rows 12-13 and 15-16) and that A0-A2 really reach GND.")
            return

        print("")
        try:
            data = read_block(i2c, 0x00, FIRST)
        except Exception as e:
            print("read failed: " + str(e))
            return

        hexdump(data, 0)
        print("")
        blank = True
        for b in data:
            if b != 0xFF:
                blank = False
                break
        print("%d bytes read." % len(data) +
              ("  (all 0xFF - an erased chip)" if blank else ""))

        try:
            oled_print("EE %d B" % len(data))
        except Exception:
            pass

        # Bigger part? Dump the rest on request.
        print("")
        more = ask("dump more - total size in bytes (blank = no): ")
        if more:
            try:
                total = int(more)
            except ValueError:
                total = 0
            if total > FIRST:
                if total > 256 and ADDR_BITS == 8:
                    print("note: ADDR_BITS is 8, so anything past 0xFF wraps. "
                          "Set ADDR_BITS = 16 for a 24C32 or larger.")
                try:
                    rest = read_block(i2c, FIRST, total - FIRST)
                    hexdump(rest, FIRST)
                    print("")
                    print("%d bytes read." % total)
                except Exception as e:
                    print("read failed: " + str(e))

        # --- the optional write test --------------------------------------
        # Default is no. WP is a wire the Jumperless owns, so "read-only"
        # here is real: nothing can write until that row is moved.
        print("")
        print("Write test: writes one byte to address 0x%02X, reads it back,"
              % TEST_AT)
        print("then puts the original byte back. WP moves to GND for that.")
        if ask("run the write test? [y/N] ").lower() == "y":
            original = data[TEST_AT] if len(data) > TEST_AT else 0xFF
            i2c = write_test(i2c, original)
        else:
            print("skipped - the chip stays read-only.")

    except KeyboardInterrupt:
        print("")
    finally:
        release_bus()
        print("bye")


main()
