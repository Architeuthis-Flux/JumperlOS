#!/usr/bin/env python3
"""Collect Jumperless V5 hardware self-test results from many boards at once.

Every board enumerates 4 CDC interfaces with the same USB VID:PID (1D50:ACAB)
and the same iSerial string ("JLV5port"), so boards are grouped by USB
location ID and identified by the RP2350 unique chip id embedded in the
self-test JSON report.

Modes:
  --watch    Listen passively on every board's main (CDC0) port for the
             sentinel-framed report that freshly-flashed boards emit while
             running their first-start self test.
  --run      Send `self_test to every board (runs the suite now) and collect.
  --report   Send `self_test_report to every board (re-prints the stored
             report without re-running) and collect.

The report line looks like:
  ::SELFTEST::{"id":"E463...","fw":"5.x.x.x","pass":true,"results":{...},...}::END::

Exit code is nonzero if any board reported a failure (or never reported).

Requires: pyserial  (pip install pyserial)
"""

import argparse
import json
import re
import sys
import threading
import time

import serial
import serial.tools.list_ports

JUMPERLESS_VID = 0x1D50
JUMPERLESS_PID = 0xACAB

TEST_NAMES = ["probe_cable", "crossbar", "tip_voltage", "psram", "peripherals"]

SENTINEL_RE = re.compile(r"::SELFTEST::(.*?)::END::", re.DOTALL)


def scan_reports(buf):
    """Consume every complete sentinel report in buf.

    Returns (payloads, remaining_buf): raw payload strings in order of
    appearance, and the unconsumed tail (possibly a partial report).
    """
    payloads = []
    while True:
        m = SENTINEL_RE.search(buf)
        if not m:
            return payloads, buf
        payloads.append(m.group(1).strip())
        buf = buf[m.end():]

GREEN = "\033[92m"
RED = "\033[91m"
BLUE = "\033[94m"
DIM = "\033[2m"
RESET = "\033[0m"


def find_boards():
    """Return {location_id: cdc0_device} for every connected Jumperless V5.

    The 4 CDC interfaces of one board share a USB location; pyserial reports
    it as e.g. "0-1.4:1.0" where the suffix after ':' is interface.alt, so
    stripping it groups the ports per physical board. CDC0 (the main command
    port) is the lowest-numbered interface.
    """
    groups = {}
    for p in serial.tools.list_ports.comports():
        if p.vid != JUMPERLESS_VID or p.pid != JUMPERLESS_PID:
            continue
        loc = p.location or p.device  # fallback: treat each port as its own board
        base = loc.split(":")[0]
        iface = loc.split(":")[1] if ":" in loc else "0"
        groups.setdefault(base, []).append((iface, p.device))
    boards = {}
    for base, ports in groups.items():
        ports.sort()  # interface "1.0" (CDC0) sorts first
        boards[base] = ports[0][1]
    return boards


class BoardWorker(threading.Thread):
    def __init__(self, location, device, command, timeout):
        super().__init__(daemon=True)
        self.location = location
        self.device = device
        self.command = command  # bytes to send, or None for watch mode
        self.timeout = timeout
        self.report = None  # parsed JSON dict
        self.error = None

    def run(self):
        try:
            with serial.Serial(self.device, 115200, timeout=0.2) as ser:
                ser.reset_input_buffer()
                if self.command:
                    ser.write(self.command)
                    ser.flush()
                buf = ""
                deadline = time.time() + self.timeout
                while time.time() < deadline:
                    chunk = ser.read(4096)
                    if chunk:
                        buf += chunk.decode("utf-8", errors="ignore")
                        # The firmware retries failed tests and prints one
                        # report per round: keep the LATEST report and only
                        # stop early on a passing one (a fail may be
                        # superseded by the next round).
                        payloads, buf = scan_reports(buf)
                        for payload in payloads:
                            if payload == "none":
                                self.error = "no stored report"
                                return
                            try:
                                self.report = json.loads(payload)
                                self.error = None
                            except json.JSONDecodeError as e:
                                self.error = f"bad json: {e}"
                                continue
                            if self.report.get("pass"):
                                return
                        # keep the buffer bounded; a report line is < 1 KB
                        if len(buf) > 65536:
                            buf = buf[-4096:]
                if self.report is None:
                    self.error = "timeout"
        except (serial.SerialException, OSError) as e:
            self.error = str(e)


def colorize(word):
    return {
        "pass": f"{GREEN}PASS{RESET}",
        "fail": f"{RED}FAIL{RESET}",
        "skip": f"{BLUE}SKIP{RESET}",
        "notrun": f"{DIM}--{RESET}",
    }.get(word, word)


def print_table(workers):
    id_w = 18
    col_w = 12
    header = f"{'board id':<{id_w}}" + "".join(f"{n:<{col_w}}" for n in TEST_NAMES) + "overall"
    print(header)
    print("-" * len(header))
    for w in sorted(workers, key=lambda w: w.location):
        if w.report:
            bid = w.report.get("id", w.location)[:id_w - 1]
            results = w.report.get("results", {})
            cells = ""
            for n in TEST_NAMES:
                word = results.get(n, "notrun")
                # pad manually: ANSI codes break str formatting widths
                cells += colorize(word) + " " * (col_w - len(word.replace("notrun", "--")))
            overall = f"{GREEN}PASS{RESET}" if w.report.get("pass") else f"{RED}FAIL{RESET}"
            print(f"{bid:<{id_w}}{cells}{overall}")
        else:
            status = w.error or "waiting..."
            print(f"{w.location[:id_w - 1]:<{id_w}}{DIM}{status}{RESET}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--watch", action="store_true", help="listen for first-start reports")
    mode.add_argument("--run", action="store_true", help="trigger `self_test on every board")
    mode.add_argument("--report", action="store_true", help="collect stored reports (`self_test_report)")
    ap.add_argument("--timeout", type=float, default=None,
                    help="seconds to wait per board (default: 120 run, 600 watch, 10 report)")
    ap.add_argument("--json", metavar="FILE", help="write collected reports to FILE as JSON")
    args = ap.parse_args()

    if args.run:
        command, timeout = b"`self_test\r\n", args.timeout or 120
    elif args.report:
        command, timeout = b"`self_test_report\r\n", args.timeout or 10
    else:
        command, timeout = None, args.timeout or 600

    boards = find_boards()
    if not boards:
        print("No Jumperless V5 boards found (VID:PID 1D50:ACAB).", file=sys.stderr)
        return 2
    print(f"Found {len(boards)} board(s).\n")

    workers = [BoardWorker(loc, dev, command, timeout) for loc, dev in sorted(boards.items())]
    for w in workers:
        w.start()

    try:
        while any(w.is_alive() for w in workers):
            time.sleep(2)
            done = sum(1 for w in workers if w.report or w.error)
            print(f"... {done}/{len(workers)} boards reported", end="\r", flush=True)
    except KeyboardInterrupt:
        print("\nInterrupted; showing partial results.\n")

    print()
    print_table(workers)

    if args.json:
        out = {w.location: (w.report or {"error": w.error}) for w in workers}
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
        print(f"Wrote {args.json}")

    all_pass = all(w.report and w.report.get("pass") for w in workers)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
