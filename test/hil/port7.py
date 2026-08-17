"""Port-7 (USBSer3, the machine backchannel) helper for the HIL scripts.

Use it when JLV5port1 is taken (Kevin's `jumperless` client) and you still need
a read-only status query. Only commands tagged SER3_ALLOWED run here - `X`
(resource census: MCP4728 write/skip counters, OLED status, IRQ slots, uptime),
`A`/`V`/`G`/`N`/`K`, and the `:verb` lines (`:oled:quarter`, `:json:power`, ...).
`i@`, `~` and anything that changes state are refused with an error line.

    python3 test/hil/port7.py X 3          # one command, collect for 3 s
    from port7 import port7_command        # from another script in test/hil
"""
import sys, glob, time
import serial  # pyserial


def port7_path():
    hits = [p for p in glob.glob("/dev/cu.usbmodem*JLV5port7") if p.endswith("port7")]
    if not hits:
        sys.exit("FAIL: machine backchannel (JLV5port7) not found. Is the board connected?")
    return hits[0]


def port7_command(cmd, collect_seconds=2.5):
    """Send `cmd` raw (single char, or ':verb\\n') and return everything received."""
    with serial.Serial(port7_path(), 115200, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(cmd.encode())
        ser.flush()
        t0 = time.time()
        buf = b""
        while time.time() - t0 < collect_seconds:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
        return buf.decode(errors="replace")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "X"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 2.5
    print(port7_command(cmd, secs))
