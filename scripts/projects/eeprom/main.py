"""EEPROM Dumper - companion for /projects/eeprom/wiring.yaml.

SDA row -> RP_GPIO_7 (RP pin 26), SCL row -> RP_GPIO_8 (RP pin 27), so
machine.I2C(1) reaches the chip. WP (row 6) sits on the top rail, so the
chip is read-only until the write test re-routes that row. See README.md.
"""

import time

_jl_project = globals().get("_jl_project", {})

SDA_PIN = 26        # node RP_GPIO_7 - the wiring routes row 8 here
SCL_PIN = 27        # node RP_GPIO_8 - the wiring routes row 7 here
I2C_BUS = 1         # 26/27 are i2c1; machine.I2C(0, ...) rejects them
I2C_HZ = 100000
ADDR = 0x50         # A0/A1/A2 grounded by the wiring
ADDR_BITS = 8       # 24C01..24C16; 24C32 and larger need 16
WP_ROW = 6          # 24Cxx pin 7 = write protect, held high
FIRST = 256         # bytes in the opening dump
TEST_AT = 0xFF      # the one address the write test touches


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


def release_bus():
    """Give the two pins back. Called on EVERY exit from main(), including
    the one where open_bus() claimed them and then failed to build the I2C
    object - that path used to return past the finally and leak the claim."""
    for n in (GPIO_7, GPIO_8):
        try:
            gpio_release_pin(n)
        except Exception:
            pass


def hexdump(data, base=0):
    for off in range(0, len(data), 16):
        c = data[off:off + 16]
        print("%04X  %-47s  |%s|"
              % (base + off, " ".join("%02X" % b for b in c),
                 "".join(chr(b) if 32 <= b < 127 else "." for b in c)))


def read_block(i2c, start, count):
    """Sequential read, 32 bytes at a time to keep buffers small."""
    out = bytearray()
    while count > 0:
        n = 32 if count > 32 else count
        out += i2c.readfrom_mem(ADDR, start, n, addrsize=ADDR_BITS)
        start += n
        count -= n
    return out


def ask(p):
    try:
        return input(p).strip()
    except EOFError:
        return ""


def write_test(i2c, original):
    """One byte written, read back, put back. Returns the live bus."""
    probe = 0xA5 if original != 0xA5 else 0x5A
    moved = False
    try:
        disconnect(WP_ROW, TOP_RAIL)
        # Never add GND while the rail is still on that row: the two together
        # are a short across the supply, through the crossbar. If the
        # disconnect did not take, stop here - protected is the safe way out.
        if is_connected(WP_ROW, TOP_RAIL):
            print("WP row %d is still on the top rail - refusing to add GND "
                  "(that would short the rail). Write test aborted." % WP_ROW)
            return i2c
        connect(WP_ROW, GND)
        moved = True
        time.sleep(0.05)
        i2c = open_bus()      # the re-route's refresh re-muxed 26/27
        if i2c is None:
            raise OSError("bus gone after the WP re-route")
        i2c.writeto_mem(ADDR, TEST_AT, bytes((probe,)), addrsize=ADDR_BITS)
        time.sleep(0.01)      # 24Cxx write cycle: 5 ms worst case
        back = i2c.readfrom_mem(ADDR, TEST_AT, 1, addrsize=ADDR_BITS)[0]
        print("wrote 0x%02X, read 0x%02X back - %s"
              % (probe, back, "writes work" if back == probe else "no stick"))
        i2c.writeto_mem(ADDR, TEST_AT, bytes((original,)), addrsize=ADDR_BITS)
        time.sleep(0.01)
        print("restored 0x%02X at 0x%02X." % (original, TEST_AT))
    except Exception as e:
        print("write test failed: " + str(e))
    finally:
        if moved:
            try:
                disconnect(WP_ROW, GND)
                connect(WP_ROW, TOP_RAIL)
                time.sleep(0.05)
                i2c = open_bus()
                print("write protect restored (row %d on the rail)." % WP_ROW)
            except Exception as e:
                print("COULD NOT RESTORE WRITE PROTECT: " + str(e))
                print("re-load the wiring before trusting the chip.")
    return i2c


def main():
    print("EEPROM Dumper " + str(_jl_project.get("dir", "")))
    i2c = open_bus()
    try:
        if i2c is None:
            return                  # inside the try: the finally still runs
        found = i2c.scan()
        print("i2c devices: " + str([hex(a) for a in found]))
        if ADDR not in found:
            print("no chip at " + hex(ADDR) + " - check the rail, the "
                  "pull-ups (rows 12-13, 15-16) and A0-A2 to GND.")
            return
        data = read_block(i2c, 0x00, FIRST)
        hexdump(data, 0)
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
        more = ask("dump more - total size in bytes (blank = no): ")
        if more:
            total = int(more) if more.isdigit() else 0
            if total > FIRST:
                if ADDR_BITS == 8 and total > 256:
                    print("note: ADDR_BITS is 8, so past 0xFF wraps.")
                hexdump(read_block(i2c, FIRST, total - FIRST), FIRST)
                print("%d bytes read." % total)
        # WP is a wire the board owns, so read-only is topology here, not
        # convention. Default is no.
        print("Write test: one byte at 0x%02X, read back, original put "
              "back. WP moves to GND only for that." % TEST_AT)
        if ask("run the write test? [y/N] ").lower() == "y":
            i2c = write_test(i2c, data[TEST_AT] if len(data) > TEST_AT else 0xFF)
        else:
            print("skipped - the chip stays read-only.")
    except KeyboardInterrupt:
        print("")
    except Exception as e:
        print("failed: " + str(e))
    finally:
        release_bus()
        print("bye")


main()
