#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════╗
║  JUMPERLESS FLASH TERMINAL                                   ║
║  plug it in. it flashes. unplug. repeat.                     ║
╚══════════════════════════════════════════════════════════════╝

Loop-flasher GUI for Jumperless V5 / OG boards.

Autodetects boards by their JumperlOS CDC0 port (/dev/cu.usbmodem*JLV5port1),
then for each one runs

    pio run -e jumperless_v5_erase -t upload --upload-port <port>

[env:jumperless_v5_erase] already owns the whole flash sequence: its
pre-upload hook does the 1200 bps BOOTSEL touch, wipes the 4 MB FatFS
partition with an 0xFF blob, then `picotool load -v -x` writes the firmware
and reboots. Nothing here reimplements any of that.

Usage:
    python3 autoUploader/flash_loop_gui.py [env] [--port PORT ...]

Auto-creates a venv on first run — no manual setup needed.
"""

# ── Auto-bootstrap venv (before any third-party imports) ──────────────────────
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
VENV_DIR = os.path.join(SCRIPT_DIR, ".venv-flashgui")
VENV_PYTHON = os.path.join(VENV_DIR, "bin", "python3")
# pygame-ce, not pygame: upstream pygame has no wheels past 3.13, and pip
# happily source-builds it WITHOUT the font module when SDL2_ttf is absent,
# which fails much later at the first render. --only-binary turns a missing
# wheel into a loud install error instead. pygame-ce is import-compatible.
_REQUIRED_PKGS = ["pygame-ce", "pyserial"]
# Attribute access, not just import: a crippled pygame installs a stub module
# that imports fine and raises NotImplementedError on first use.
_IMPORT_CHECK = "import serial.tools.list_ports, pygame; pygame.font.init()"


def _pip_install():
    subprocess.check_call([VENV_PYTHON, "-m", "pip", "install", "--upgrade",
                           "pip"])
    # Plain pygame and pygame-ce both own the `pygame` name; clear it out in
    # case this venv predates the switch.
    subprocess.call([VENV_PYTHON, "-m", "pip", "uninstall", "-y", "pygame"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.check_call([VENV_PYTHON, "-m", "pip", "install",
                           "--only-binary", ":all:", *_REQUIRED_PKGS])


def _ensure_venv():
    # Test for THIS venv, not "any venv" — conda/micromamba base environments
    # pass the usual sys.prefix != sys.base_prefix check. sys.prefix (not
    # sys.executable) because bin/python3 is a symlink to the real interpreter.
    if os.path.realpath(sys.prefix) == os.path.realpath(VENV_DIR):
        return
    if os.environ.get("_JLFLASHGUI_BOOTSTRAPPED"):
        return

    if not os.path.isfile(VENV_PYTHON):
        print(f"Creating venv at {VENV_DIR} ...")
        subprocess.check_call([sys.executable, "-m", "venv", VENV_DIR])
        _pip_install()
    elif subprocess.call([VENV_PYTHON, "-c", _IMPORT_CHECK],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL) != 0:
        print("Venv packages missing or broken, reinstalling ...")
        _pip_install()

    os.environ["_JLFLASHGUI_BOOTSTRAPPED"] = "1"
    os.execv(VENV_PYTHON, [VENV_PYTHON] + sys.argv)


_ensure_venv()

# ── Third-party imports (safe after venv bootstrap) ───────────────────────────
import argparse
import atexit
import colorsys
import json
import random
import re
import threading
import time

import pygame
import serial
import serial.tools.list_ports

# ── Paths ─────────────────────────────────────────────────────────────────────
PIO = os.path.expanduser("~/.platformio/penv/bin/pio")
PICOTOOL = os.path.expanduser(
    "~/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool")

# ponytail: the BOOTSEL/erase/load phase is serialized across all boards by one
# global lock. picotool addresses devices by USB bus/address, and two boards
# sitting in BOOTSEL at once are indistinguishable to a bare `picotool load`;
# concurrent `pio run` on one build dir would also race SCons. Ceiling: boards
# flash one at a time (~10 s each). Upgrade path: snapshot `picotool info -d`
# before the touch, claim the newly-appeared bus/address, and pass
# --bus/--address so stations can run in parallel.
UPLOAD_LOCK = threading.Lock()

# ── HSV Color Engine ──────────────────────────────────────────────────────────


def hsv_rgb(h: float, s: float = 1.0, v: float = 1.0) -> tuple:
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, max(0.0, min(1.0, s)),
                                  max(0.0, min(1.0, v)))
    return (int(r * 255), int(g * 255), int(b * 255))


def port_base_hue(idx: int, total_ports: int) -> float:
    if total_ports <= 1:
        return 0.47
    return (0.47 + idx / total_ports) % 1.0


STATE_HSV_MAP: dict[str, tuple] = {}


def state_color(base_hue: float, state: str, tick: int) -> tuple:
    params = STATE_HSV_MAP.get(state)
    if params is None:
        return hsv_rgb(base_hue, 0.4, 0.5)
    h, s, v = params
    if h is None:
        h = base_hue
    if state == S_WAITING:
        v = 0.3 + 0.2 * (0.5 + 0.5 * ((tick % 60) / 60.0))
    return hsv_rgb(h, s, v)


def rainbow_line_color(line_idx: int, n_hues: int, tick: int) -> tuple:
    hue = ((line_idx / max(1, n_hues * 4)) + tick * 0.0005) % 1.0
    return hsv_rgb(hue, 0.7, 0.9)


# ── Theme ─────────────────────────────────────────────────────────────────────
BG = (0, 0, 0)
HEADER_BG = (3, 18, 20)
PANEL_BG = (2, 10, 11)
CYAN = (90, 230, 220)
DIM_CYAN = (50, 150, 150)
DARK_CYAN = (12, 55, 55)
WHITE = (225, 245, 250)
RED = (255, 60, 60)
DIM_RED = (150, 35, 35)
AMBER = (255, 176, 0)
DIM_AMBER = (170, 120, 10)
GREEN = (90, 240, 140)

FPS = 30
FONT_SIZE = 14
LINE_H = 18

# ── State machine ─────────────────────────────────────────────────────────────
S_BUILDING = "BUILDING"
S_WAITING = "AWAITING BOARD"
S_BOOTSEL = "ENTERING BOOTSEL"
S_ERASING = "ERASING FATFS"
S_FLASHING = "WRITING FIRMWARE"
S_SUCCESS = "COMPLETE"
S_FAILED = "FAILED"
S_DONE = "REMOVE OR [F]"
S_INACTIVE = "IDLE (other env)"
S_QUIT = "OFFLINE"

PROBE_KEY = "debug probe"

STATE_HSV_MAP.update({
    S_WAITING:  (None, 0.3, 0.5),
    S_BUILDING: (0.08, 1.0, 0.9),
    S_BOOTSEL:  (0.08, 1.0, 0.9),
    S_ERASING:  (0.08, 1.0, 0.9),
    S_FLASHING: (None, 1.0, 0.9),
    S_SUCCESS:  (0.35, 0.8, 1.0),
    S_FAILED:   (0.0, 1.0, 1.0),
    S_DONE:     (None, 0.2, 0.45),
    S_INACTIVE: (None, 0.1, 0.3),
    S_QUIT:     (None, 0.1, 0.3),
})

BUSY_STATES = (S_BOOTSEL, S_ERASING, S_FLASHING)

# JumperlOS (V5 and OG) and the OG factory firmware, from usb_descriptors.cpp
# and boards/jumperless_og.json "hwids". The CMSIS-DAP debug probe (2E8A:000C)
# is deliberately absent so a connected probe is never mistaken for a board.
JL_USB_IDS = {
    (0x1D50, 0xACAB),  # JumperlOS
    (0xACAB, 0x1312),  # OG factory firmware
}
JL_SERIAL_PREFIXES = ("JLV5port", "JLOGport")

ANSI_RE = re.compile(r'\x1b\[[0-9;]*[A-Za-z]|\[[\dA-Z;]*[mKHJ]')
# picotool draws "Loading into Flash: [=====]  73%" and redraws it with \r.
PICOTOOL_PCT_RE = re.compile(r'(\d+)\s*%')
# One compile line per translation unit drowns the flash log; ~600 of them.
BUILD_NOISE_RE = re.compile(r'^(Compiling|Indexing|Archiving|Generating)\b')
BUILD_KEEP_RE = re.compile(
    r'error|warning|Linking|Building|Creating|RAM:|Flash:|SUCCESS|FAILED'
    r'|Environment|took ',
    re.IGNORECASE)


def parse_upload_line(clean: str, state: str, progress: int
                      ) -> tuple[str, int, bool]:
    """Map one line of `pio run -t upload` output to (state, progress, keep).

    keep=False means the line is redundant — a picotool progress redraw or
    per-translation-unit compile spam — and should not reach the log.
    """
    if clean.startswith("[erase_fs]"):
        if "Continuing with firmware upload" in clean:
            return S_FLASHING, 0, True
        if "BOOTSEL" in clean:
            return S_BOOTSEL, progress, True
        if "Hooked" in clean:
            # Printed at SCons script-load, long before any erase happens.
            return state, progress, True
        return S_ERASING, progress, True
    if "Loading into" in clean:
        # Only picotool's own bar carries the flash percentage. Matching bare
        # "%" anywhere would also swallow the RAM:/Flash: usage summary.
        m = PICOTOOL_PCT_RE.search(clean)
        return state, (min(100, int(m.group(1))) if m else progress), False
    if "Uploading" in clean and ".uf2" in clean:
        return S_FLASHING, 0, True

    keep = not (BUILD_NOISE_RE.match(clean) and not BUILD_KEEP_RE.search(clean))
    if clean.startswith(("Compiling", "Linking", "Building")):
        return S_BUILDING, progress, keep
    return state, progress, keep


def spinner_char(tick: int) -> str:
    return ["◐", "◓", "◑", "◒"][(tick // 8) % 4]


def clipboard_copy(text: str):
    try:
        subprocess.run(["pbcopy"], input=text.encode("utf-8"),
                       check=False, timeout=2)
    except Exception:
        pass


def pio_envs() -> dict[str, str]:
    """{env name: resolved upload_protocol} straight from PlatformIO.

    Asking pio rather than reading platformio.ini ourselves means `extends`
    inheritance is resolved by the thing that owns those semantics — the
    _debug envs only get their cmsis-dap protocol through an extends chain.
    """
    try:
        out = subprocess.run([PIO, "project", "config", "--json-output"],
                             capture_output=True, text=True, timeout=120,
                             cwd=PROJECT_DIR)
        sections = json.loads(out.stdout)
    except Exception:
        return {}
    return {name[4:]: dict(opts).get("upload_protocol", "picotool")
            for name, opts in sections if name.startswith("env:")}


def is_probe_env(protocol: str) -> bool:
    """True for envs that flash over a debug probe instead of USB BOOTSEL.

    Those uploads have no serial port and never enter BOOTSEL, so the 1200 bps
    touch, the tty bookkeeping and the stray-BOOTSEL guard all have to be
    skipped. Everything that is not a USB mass-storage style upload is a probe.
    """
    return protocol not in ("picotool", "mbed")


def _tty_sort_key(device: str):
    """Sort /dev/cu.usbmodem646 after 618, not before it."""
    m = re.search(r'(\d+)$', device)
    return (int(m.group(1)) if m else 0, device)


def group_boards(entries) -> dict[str, str]:
    """(device, vid, pid, serial, location) tuples -> {board key: upload tty}.

    Every Jumperless reports the SAME USB serial string ("JLV5port"), so as
    soon as two are plugged in macOS abandons the /dev/cu.usbmodemJLV5portN
    naming and hands out opaque numbers instead — four boards look like
    sixteen anonymous ttys. The USB location (the hub-port topology path) is
    the only field that actually separates one board from another, and unlike
    the tty name it survives both a replug into the same port and the
    re-enumeration that follows every flash.

    Each board exposes several CDC interfaces; JumperlOS enters BOOTSEL on a
    1200 bps touch of ANY of them, so the lowest-numbered tty is a fine upload
    port. (OG *factory* firmware only resets on CDC0 — flash it once with
    JumperlOS and this holds thereafter.)
    """
    boards: dict[str, list[str]] = {}
    for device, vid, pid, serial_number, location in entries:
        sn = serial_number or ""
        if (vid, pid) not in JL_USB_IDS and not sn.startswith(JL_SERIAL_PREFIXES):
            continue
        # Linux appends ":<config>.<interface>"; the board is the part before.
        key = location.split(":")[0] if location else device
        boards.setdefault(key, []).append(device)
    return {key: sorted(ttys, key=_tty_sort_key)[0]
            for key, ttys in sorted(boards.items())}


def discover_boards() -> dict[str, str]:
    return group_boards(
        (p.device, p.vid, p.pid, p.serial_number, p.location)
        for p in serial.tools.list_ports.comports())


def bootsel_device_count() -> int:
    """Mirrors platform-raspberrypi's get_num_rpxxxx_devs()."""
    try:
        out = subprocess.run([PICOTOOL, "info", "-d"], capture_output=True,
                             timeout=5).stdout
        return out.count(b"type:")
    except Exception:
        return 0


# ── Shared log ────────────────────────────────────────────────────────────────

class SharedLog:
    def __init__(self):
        self.lines: list[tuple[str, tuple, int | None]] = []
        self.lock = threading.Lock()

    def add(self, text: str, colour=CYAN, port_idx: int | None = None):
        with self.lock:
            for line in text.splitlines():
                self.lines.append((line.replace('\x00', ''), colour, port_idx))

    def get_lines(self, n: int) -> list[tuple[str, tuple, int | None]]:
        with self.lock:
            return list(self.lines[-n:])

    @property
    def total(self) -> int:
        with self.lock:
            return len(self.lines)


# ── Flash station (one per physical board) ───────────────────────────────────

class FlashStation:
    """One station per USB location, i.e. per physical board.

    The tty is looked up fresh from `board_index` every time instead of being
    stored, because a board's tty name changes whenever it re-enumerates —
    which happens on every single flash (BOOTSEL drops the CDCs, and the
    reboot afterwards comes back under a new number). The location does not
    change, so that is what a station is bound to.
    """

    def __init__(self, key: str, idx: int, shared_log: SharedLog, cfg: dict,
                 board_index, is_probe: bool = False):
        self.key = key
        self.idx = idx
        self.short = key.rsplit("/", 1)[-1] if key.startswith("/") else key
        self.shared_log = shared_log
        self.cfg = cfg
        self.board_index = board_index
        self.is_probe = is_probe
        self.state = S_WAITING
        self.count = 0
        self.running = True
        self.enabled = True
        self.worker: threading.Thread | None = None
        self.rearm = threading.Event()
        self.progress = 0
        self.flash_start_time: float | None = None
        self.last_flash_duration: float | None = None
        self._serial: serial.Serial | None = None
        self._mon_stop = threading.Event()

    @property
    def tag(self) -> str:
        return f"[{self.short}]"

    @property
    def port(self) -> str | None:
        return None if self.is_probe else self.board_index().get(self.key)

    def log(self, text: str, colour=None):
        self.shared_log.add(f"{self.tag} {text}", colour or CYAN, self.idx)

    def port_exists(self) -> bool:
        return self.port is not None

    def applies(self) -> bool:
        """Is this station the right kind for the env currently selected?"""
        return self.is_probe == self.cfg["is_probe_env"]

    def ready(self) -> bool:
        """A USB board flashes on plug-in; the probe has nothing to detect, so
        it flashes on demand ([R] / [F], and once when the build finishes)."""
        return self.rearm.is_set() if self.is_probe else self.port_exists()

    def close(self):
        s, self._serial = self._serial, None
        if s is not None:
            try:
                s.close()
            except Exception:
                pass

    def stop(self):
        self.running = False
        self._mon_stop.set()
        self.close()

    def free_port(self, port: str):
        """Drop anything holding this tty so the 1200 bps touch can land."""
        self._mon_stop.set()
        self.close()
        try:
            result = subprocess.run(["lsof", "-t", port],
                                    capture_output=True, text=True, timeout=5)
            for pid in result.stdout.split():
                if pid and int(pid) != os.getpid():
                    self.log(f"  killing pid {pid} holding {port}", DIM_AMBER)
                    subprocess.run(["kill", pid], capture_output=True,
                                   timeout=5)
        except Exception:
            pass

    def _wait_for_port(self, timeout_s: float) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline and self.running:
            if self.port_exists():
                return True
            time.sleep(0.4)
        return self.port_exists()

    def run_upload(self, port: str | None) -> bool:
        """One `pio run -t upload`. The env's own hooks do BOOTSEL + erase."""
        cmd = [PIO, "run", "-e", self.cfg["env"], "-d", PROJECT_DIR,
               "-t", "upload"]
        if port:  # probe uploads have no port to pass
            cmd += ["--upload-port", port]
        self.log(f"> {' '.join(cmd)}", DIM_CYAN)
        self.progress = 0
        self.state = S_BOOTSEL
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1, cwd=PROJECT_DIR)
            assert proc.stdout is not None
            for raw in proc.stdout:
                clean = ANSI_RE.sub('', raw.rstrip().replace('\x00', '')).strip()
                if not clean:
                    continue
                self.state, self.progress, keep = parse_upload_line(
                    clean, self.state, self.progress)
                if keep:
                    self.log(f"  {clean}", DIM_CYAN)
            proc.wait()
            if proc.returncode != 0:
                self.log(f"✗ upload exited {proc.returncode}", RED)
                return False
            self.progress = 100
            return True
        except FileNotFoundError:
            self.log(f"✗ pio not found at {PIO}", RED)
            return False
        except Exception as e:
            self.log(f"✗ upload: {e}", RED)
            return False

    def _serial_monitor(self):
        """Stream the board's first CDC while idling, to see boot output."""
        self._mon_stop.clear()
        port = self.port
        if port is None:
            return
        try:
            s = serial.Serial(port, 115200, timeout=0.5)
            self._serial = s
        except Exception as e:
            self.log(f"  monitor open failed: {e}", DIM_AMBER)
            return
        try:
            while (self.running and not self._mon_stop.is_set()
                   and self.port_exists()):
                try:
                    raw = s.readline()
                except (serial.SerialException, OSError):
                    break
                if not raw:
                    continue
                line = (raw.decode(errors="replace").rstrip()
                        .replace('\x00', '').replace('\ufffd', ''))
                if line:
                    self.log(f"  {ANSI_RE.sub('', line)}", DIM_CYAN)
        finally:
            # Close only our own handle — a later cycle may already have put a
            # fresh one in self._serial.
            try:
                s.close()
            except Exception:
                pass
            if self._serial is s:
                self._serial = None

    def start(self):
        self.worker = threading.Thread(target=self._loop, daemon=True)
        self.worker.start()

    def toggle(self):
        self.enabled = not self.enabled
        label = "ENABLED" if self.enabled else "DISABLED"
        self.log(f"PORT {label}", AMBER if self.enabled else DIM_RED)

    def _loop(self):
        while self.running:
            self.progress = 0
            while self.running:
                if not self.applies():
                    self.state = S_INACTIVE
                elif self.enabled and self.ready():
                    break
                else:
                    self.state = S_WAITING
                time.sleep(0.4)
            if not self.running:
                break
            self.rearm.clear()
            try:
                self._flash_cycle()
            finally:
                self.close()
        self.state = S_QUIT

    def _flash_cycle(self):
        port = None
        if not self.is_probe:
            time.sleep(1.0)  # let the CDCs settle before touching them
            port = self.port
            if port is None:
                return

        self.count += 1
        if self.is_probe:
            self.log(f"■ FLASH #{self.count} over {self.cfg['protocol']}",
                     WHITE)
        else:
            self.log(f"■ BOARD #{self.count} DETECTED on {port}", WHITE)
            self.free_port(port)
            time.sleep(0.4)

        self.flash_start_time = time.time()
        with UPLOAD_LOCK:
            if not self.running:
                return
            # picotool addresses BOOTSEL devices by USB bus/address, and
            # PlatformIO's BeforeUpload skips its 1200 bps touch entirely when
            # it already sees one — so a board stranded in BOOTSEL (usually
            # from an earlier failed flash) would silently receive THIS
            # station's firmware. With several boards on the bench that is a
            # wrong-board flash, so refuse instead. Probe uploads never touch
            # BOOTSEL, so the guard does not apply to them.
            stray = 0 if self.is_probe else bootsel_device_count()
            if stray:
                self.log(f"✗ {stray} board(s) ALREADY IN BOOTSEL — refusing, "
                         "picotool cannot tell which one is mine. Unplug the "
                         "stranded board, then [F] to retry.", RED)
                ok = False
            else:
                ok = self.run_upload(port)
        elapsed = time.time() - self.flash_start_time
        self.last_flash_duration = elapsed

        if ok:
            self.state = S_SUCCESS
            self.log(f"✓ #{self.count} FLASHED  [{elapsed:.1f}s]", GREEN)
            if not self.is_probe:
                if self._wait_for_port(15.0):
                    self.log(f"  BOARD REBOOTED as {self.port}", DIM_CYAN)
                else:
                    self.log("  PORT DID NOT COME BACK — check the board",
                             AMBER)
        else:
            self.state = S_FAILED
            self.log(f"✗ #{self.count} FLASH FAILED  [{elapsed:.1f}s]", RED)

        self.state = S_DONE
        if self.is_probe:
            # Nothing to unplug: the probe stays put, so only [R] / [F] can
            # start the next flash.
            self.log("  [R] OR [F] TO FLASH AGAIN", DIM_AMBER)
            while self.running and not self.rearm.is_set():
                time.sleep(0.4)
            return

        # Idle until the board is unplugged or someone re-arms this station.
        self.log("  UNPLUG, OR [F] TO RE-FLASH IN PLACE", DIM_AMBER)
        if self.cfg["monitor"] and ok and self.port_exists():
            threading.Thread(target=self._serial_monitor, daemon=True).start()

        while self.running:
            if not self.port_exists():
                self.log("  BOARD REMOVED.", DIM_CYAN)
                break
            if self.rearm.is_set():
                self.log("  RE-ARMED", AMBER)
                break
            time.sleep(0.4)
        self._mon_stop.set()


# ── Port manager ──────────────────────────────────────────────────────────────

class PortManager:
    """Owns the live {board key -> tty} index and one station per board."""

    def __init__(self, shared_log: SharedLog, build_done: threading.Event,
                 build_ok: list, cfg: dict, opts: argparse.Namespace):
        self.shared_log = shared_log
        self.build_done = build_done
        self.build_ok = build_ok
        self.cfg = cfg
        self.opts = opts
        self.stations: list[FlashStation] = []
        self.lock = threading.Lock()
        # Separate short-held lock: every station polls the index constantly,
        # and the render loop reads it once per station per frame.
        self.index_lock = threading.Lock()
        self._boards: dict[str, str] = {}
        self.running = True
        # Explicit --port means the user picked the boards; don't second-guess.
        self.scanning = not opts.port
        # One permanent station for probe uploads. It sits IDLE while a USB env
        # is selected and takes over when [E] lands on a cmsis-dap env, so
        # switching envs at runtime needs no restructuring.
        self._add_station(PROBE_KEY, is_probe=True)

    def boards(self) -> dict[str, str]:
        with self.index_lock:
            return dict(self._boards)

    def refresh(self):
        """Re-read the USB tree, then adopt any board we have not seen yet."""
        if self.opts.port:
            found = {p: p for p in self.opts.port if os.path.exists(p)}
        else:
            found = discover_boards()
        with self.index_lock:
            self._boards = found
        if not self.scanning:
            return
        known = {s.key for s in self.get_stations()}
        for key in sorted(found):
            if key not in known:
                self.shared_log.add(
                    f"▶ NEW BOARD at USB {key}  ({found[key]})", AMBER)
                self._add_station(key)

    def _add_station(self, key: str, is_probe: bool = False):
        with self.lock:
            if any(s.key == key for s in self.stations):
                return
            s = FlashStation(key, len(self.stations), self.shared_log,
                             self.cfg, self.boards, is_probe=is_probe)
            self.stations.append(s)
            if self.build_done.is_set() and self.build_ok[0]:
                s.start()

    def get_stations(self) -> list[FlashStation]:
        with self.lock:
            return list(self.stations)

    def start_all_ready(self):
        with self.lock:
            for s in self.stations:
                if s.worker is None:
                    s.start()

    def rearm_all(self):
        for s in self.get_stations():
            s.rearm.set()

    def scan_loop(self):
        while self.running:
            self.refresh()
            time.sleep(1.0)

    def start_scanner(self):
        threading.Thread(target=self.scan_loop, daemon=True).start()

    def shutdown(self):
        self.running = False
        with self.lock:
            for s in self.stations:
                s.stop()
            for s in self.stations:
                if s.worker and s.worker.is_alive():
                    s.worker.join(timeout=2.0)


# ── Rendering helpers ─────────────────────────────────────────────────────────

def build_scanline_overlay(w: int, h: int) -> pygame.Surface:
    overlay = pygame.Surface((w, h), pygame.SRCALPHA)
    for y in range(0, h, 3):
        pygame.draw.line(overlay, (0, 0, 0, 18), (0, y), (w, y))
    return overlay


def _get_visible(shared_log: SharedLog, max_lines: int,
                 scroll_offset: int) -> list[tuple[str, tuple, int | None]]:
    all_lines = shared_log.get_lines(max_lines + scroll_offset)
    total = len(all_lines)
    return all_lines[max(0, total - max_lines - scroll_offset):
                     max(0, total - scroll_offset)]


def self_test() -> None:
    """Checks the two pieces of logic that are not obvious by inspection:
    board identification, and the pio/picotool output parser."""

    # Four V5 boards on one hub, verbatim from `list_ports.comports()`. All
    # share serial 'JLV5port', so macOS gave up on the JLV5portN names and
    # handed out opaque numbers — only the location separates the boards.
    ports = [(f"/dev/cu.usbmodem{n}", 0x1D50, 0xACAB, "JLV5port", loc)
             for loc, nums in {
                 "0-1.3.2.1": (618, 644, 645, 646),
                 "0-1.3.2.2": (647, 648, 649, 650),
                 "0-1.3.2.4": (651, 652, 653, 654),
                 "0-1.3.4.4.3": (655, 657, 658, 659),
             }.items() for n in nums]
    ports += [
        ("/dev/cu.Bluetooth-Incoming-Port", None, None, None, None),
        ("/dev/cu.debug-console", None, None, None, None),
        # Linux reports location with a ":<config>.<interface>" suffix, so the
        # two CDCs below are one board, not two.
        ("/dev/ttyACM0", 0x1D50, 0xACAB, "JLV5port", "1-1.4:1.0"),
        ("/dev/ttyACM1", 0x1D50, 0xACAB, "JLV5port", "1-1.4:1.2"),
    ]
    got = group_boards(ports)
    want = {
        "0-1.3.2.1": "/dev/cu.usbmodem618",
        "0-1.3.2.2": "/dev/cu.usbmodem647",
        "0-1.3.2.4": "/dev/cu.usbmodem651",
        "0-1.3.4.4.3": "/dev/cu.usbmodem655",
        "1-1.4": "/dev/ttyACM0",
    }
    assert got == want, f"group_boards -> {got}\n           want {want}"

    # Only USB uploads have a port and a BOOTSEL step to manage.
    assert not is_probe_env("picotool")
    assert is_probe_env("cmsis-dap") and is_probe_env("jlink")

    # Every line below is verbatim from a real `pio run -e jumperless_v5_erase
    # -t upload` run (format strings in scripts/erase_fs_partition.py).
    script = [
        # (line, expected state, expected progress, expected keep)
        ("[erase_fs] Hooked pre-upload erase for [env:jumperless_v5_erase]",
         S_BOOTSEL, 0, True),
        ("Compiling .pio/build/jumperless_v5_erase/src/Apps.cpp.o",
         S_BUILDING, 0, False),
        ("RAM:   [==        ]  24.1% (used 126536 bytes from 524288 bytes)",
         S_BUILDING, 0, True),
        ("[erase_fs] Triggering BOOTSEL via 1200bps touch on "
         "/dev/cu.usbmodemJLV5port1", S_BOOTSEL, 0, True),
        ("[erase_fs] Generating 4194304-byte 0xFF blob -> fs_erase.bin "
         "(4.00 MB FatFS partition)", S_ERASING, 0, True),
        ("Loading into Flash: [=========            ]  31%",
         S_ERASING, 31, False),
        ("[erase_fs] FatFS partition erased. Continuing with firmware "
         "upload...", S_FLASHING, 0, True),
        ("Uploading .pio/build/jumperless_v5_erase/firmware.uf2",
         S_FLASHING, 0, True),
        ("Loading into Flash: [=====================]  100%",
         S_FLASHING, 100, False),
    ]
    state, progress = S_BOOTSEL, 0
    for line, want_state, want_pct, want_keep in script:
        state, progress, keep = parse_upload_line(line, state, progress)
        got, want = (state, progress, keep), (want_state, want_pct, want_keep)
        assert got == want, f"parse_upload_line({line!r}) -> {got}, want {want}"

    print("self-test OK: 16 ttys grouped into 4 boards by USB location, "
          "and parse_upload_line tracks erase -> flash -> 100%")


def _load_mono(size: int) -> pygame.font.Font:
    for name in ("SF Mono", "Menlo", "Monaco", "Courier New", "Courier"):
        path = pygame.font.match_font(name)
        if path:
            return pygame.font.Font(path, size)
    return pygame.font.SysFont("monospace", size)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Jumperless Flash Terminal — loop-flash boards as they "
                    "are plugged in.")
    parser.add_argument("env", nargs="?", default="jumperless_v5_erase",
                        help="PlatformIO env to build + upload "
                             "(default: jumperless_v5_erase, which wipes the "
                             "FatFS partition before writing firmware)")
    parser.add_argument("--no-erase", action="store_true",
                        help="Drop the _erase suffix from env, preserving "
                             "slots / scripts / config")
    parser.add_argument("--port", nargs="+", default=None,
                        help="Explicit tty(s) instead of USB autodetection")
    parser.add_argument("--no-build", action="store_true",
                        help="Skip the initial build, flash what is in .pio")
    parser.add_argument("--monitor", action="store_true",
                        help="Stream the board's first CDC after a flash")
    parser.add_argument("--hues", type=int, default=64,
                        help="HSV hue steps for the rainbow (16-360)")
    parser.add_argument("--self-test", action="store_true",
                        help="Check the pio output parser and exit")
    args = parser.parse_args()
    args.hues = max(16, min(360, args.hues))

    if args.self_test:
        self_test()
        return

    if not os.path.isfile(PIO):
        sys.exit(f"ERROR: pio not found at {PIO}")

    envs = pio_envs()
    env = args.env
    if args.no_erase and env.endswith("_erase"):
        env = env[:-len("_erase")]
    if envs and env not in envs:
        sys.exit(f"ERROR: [env:{env}] is not in platformio.ini "
                 f"(have: {', '.join(sorted(envs))})")
    # [E] cycles every env pio knows about. Each has its own build dir, so a
    # switch costs one compile the first time you land on it.
    env_cycle = sorted(envs)

    cfg = {"env": env, "monitor": args.monitor, "no_build": args.no_build,
           "protocol": "picotool", "is_probe_env": False}

    def set_env(name: str):
        cfg["env"] = name
        cfg["protocol"] = envs.get(name, "picotool")
        cfg["is_probe_env"] = is_probe_env(cfg["protocol"])

    set_env(env)
    n_hues = args.hues
    boards = ({p: p for p in args.port} if args.port else discover_boards())

    # ── Window layout ─────────────────────────────────────────────────
    header_h, footer_h, win_w, min_log_h = 55, 28, 1100, 420
    port_panel_h = max(60, (len(boards) + 1) * 32 + 12)  # +1 probe station
    win_h = header_h + port_panel_h + min_log_h + footer_h
    max_log_lines = (win_h - header_h - port_panel_h - footer_h - 10) // LINE_H

    shared_log = SharedLog()

    pygame.init()
    screen = pygame.display.set_mode((win_w, win_h), pygame.RESIZABLE)
    pygame.display.set_caption("JUMPERLESS FLASH TERMINAL")
    clock = pygame.time.Clock()
    font = _load_mono(FONT_SIZE)
    font_big = _load_mono(FONT_SIZE + 2)
    char_w = font.size("X")[0]
    scanlines = build_scanline_overlay(win_w, win_h)

    # ── Banner ────────────────────────────────────────────────────────
    shared_log.add("")
    for i, line in enumerate([
        "╔═══════════════════════════════════════════════════════════╗",
        "║   JUMPERLESS FLASH TERMINAL                               ║",
        "║   plug it in. it flashes. unplug. repeat.                  ║",
        "╚═══════════════════════════════════════════════════════════╝",
    ]):
        shared_log.add(line, rainbow_line_color(i, n_hues, 0))
    shared_log.add("")
    shared_log.add(f"PROJECT:  {PROJECT_DIR}", WHITE)
    shared_log.add(f"ENV:      {env}   (upload via {cfg['protocol']})", WHITE)
    shared_log.add(f"ENVS:     {', '.join(env_cycle) or '(pio query failed)'}",
                   DIM_CYAN)
    if cfg["is_probe_env"]:
        shared_log.add("MODE:     debug probe — flashes whatever the probe is "
                       "wired to, on [R]/[F]. USB boards stay idle.", AMBER)
    if boards:
        shared_log.add(f"BOARDS:   {len(boards)} found "
                       "(identified by USB location, since every board "
                       "reports the same USB serial)", WHITE)
        for key, tty in boards.items():
            shared_log.add(f"            USB {key:<14s} -> {tty}", WHITE)
    else:
        shared_log.add("BOARDS:   none yet — plug one in", WHITE)
    if env.endswith("_erase"):
        shared_log.add("ERASE:    FatFS partition wiped before every flash",
                       AMBER)
    else:
        shared_log.add("ERASE:    off — user data preserved", DIM_AMBER)
    shared_log.add("")

    # ── Build ─────────────────────────────────────────────────────────
    build_done = threading.Event()
    build_ok = [False]

    def do_build():
        build_done.clear()
        if cfg["no_build"]:
            shared_log.add("▶ SKIPPING BUILD (--no-build)", AMBER)
            build_ok[0] = True
            build_done.set()
            return
        shared_log.add(f"▶ COMPILING [{cfg['env']}] ...", AMBER)
        cmd = [PIO, "run", "-e", cfg["env"], "-d", PROJECT_DIR]
        shared_log.add(f"  > {' '.join(cmd)}", DIM_CYAN)
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1, cwd=PROJECT_DIR)
            assert proc.stdout is not None
            for raw in proc.stdout:
                clean = ANSI_RE.sub('', raw.rstrip()).strip()
                if not clean:
                    continue
                if BUILD_NOISE_RE.match(clean) and not BUILD_KEEP_RE.search(clean):
                    continue
                shared_log.add(f"    {clean}", DIM_CYAN)
            proc.wait()
            if proc.returncode != 0:
                shared_log.add(f"✗ BUILD FAILED (exit {proc.returncode})", RED)
                build_done.set()
                return
            shared_log.add("✓ BUILD COMPLETE — READY TO FLASH", GREEN)
            shared_log.add("")
            build_ok[0] = True
        except Exception as e:
            shared_log.add(f"✗ BUILD EXCEPTION: {e}", RED)
        build_done.set()

    threading.Thread(target=do_build, daemon=True).start()

    # ── Port manager ──────────────────────────────────────────────────
    port_mgr = PortManager(shared_log, build_done, build_ok, cfg, opts=args)
    atexit.register(port_mgr.shutdown)
    # The index must stay current from the very start: stations resolve their
    # tty through it, and a board's tty changes on every flash.
    port_mgr.refresh()
    port_mgr.start_scanner()

    def start_stations():
        build_done.wait()
        if build_ok[0]:
            n = len(port_mgr.get_stations())
            shared_log.add(f"▶ LAUNCHING {n} STATION(S) — "
                           "AUTODETECT ON FOR NEW BOARDS", AMBER)
            shared_log.add("━" * 58, DIM_CYAN)
            port_mgr.start_all_ready()
            if cfg["is_probe_env"]:
                port_mgr.rearm_all()  # nothing to plug in; fire once

    threading.Thread(target=start_stations, daemon=True).start()

    # ── Text selection state ──────────────────────────────────────────
    sel_anchor: tuple[int, int] | None = None
    sel_end: tuple[int, int] | None = None
    sel_dragging = False
    log_y_global = 0

    def pos_to_line_char(mx, my, log_y, n_visible):
        li = max(0, min((my - log_y) // LINE_H, n_visible - 1))
        return (li, max(0, (mx - 16) // char_w))

    def get_selected_text(visible_lines):
        if sel_anchor is None or sel_end is None:
            return ""
        a, b = sorted((sel_anchor, sel_end))
        parts = []
        for i in range(a[0], min(b[0] + 1, len(visible_lines))):
            text = visible_lines[i][0]
            if i == a[0] and i == b[0]:
                parts.append(text[a[1]:b[1]])
            elif i == a[0]:
                parts.append(text[a[1]:])
            elif i == b[0]:
                parts.append(text[:b[1]])
            else:
                parts.append(text)
        return "\n".join(parts)

    # ── Event loop ────────────────────────────────────────────────────
    tick = 0
    scroll_offset = 0
    auto_scroll = True
    prev_station_count = 0
    selected_port_idx = 0

    try:
        while True:
            stations = port_mgr.get_stations()
            n_stations = len(stations)

            if n_stations != prev_station_count:
                prev_station_count = n_stations
                port_panel_h = max(60, n_stations * 32 + 12)
                win_h = header_h + port_panel_h + min_log_h + footer_h
                max_log_lines = (win_h - header_h - port_panel_h
                                 - footer_h - 10) // LINE_H
                screen = pygame.display.set_mode((win_w, win_h),
                                                 pygame.RESIZABLE)
                scanlines = build_scanline_overlay(win_w, win_h)

            mods = pygame.key.get_mods()
            cmd_ctrl = bool(mods & (pygame.KMOD_META | pygame.KMOD_CTRL))

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    raise KeyboardInterrupt

                elif event.type == pygame.VIDEORESIZE:
                    win_w, win_h = event.w, event.h
                    max_log_lines = (win_h - header_h - port_panel_h
                                     - footer_h - 10) // LINE_H
                    screen = pygame.display.set_mode((win_w, win_h),
                                                     pygame.RESIZABLE)
                    scanlines = build_scanline_overlay(win_w, win_h)

                elif event.type == pygame.KEYDOWN:
                    k = event.key
                    if k in (pygame.K_q, pygame.K_ESCAPE):
                        raise KeyboardInterrupt
                    elif k == pygame.K_UP:
                        scroll_offset = min(
                            scroll_offset + 3,
                            max(0, shared_log.total - max_log_lines))
                        auto_scroll = False
                    elif k == pygame.K_DOWN:
                        scroll_offset = max(0, scroll_offset - 3)
                        auto_scroll = scroll_offset == 0
                    elif k == pygame.K_END:
                        scroll_offset, auto_scroll = 0, True
                    elif k == pygame.K_HOME:
                        scroll_offset = max(0, shared_log.total - max_log_lines)
                        auto_scroll = False
                    elif pygame.K_1 <= k <= pygame.K_9:
                        idx = k - pygame.K_1
                        if idx < n_stations:
                            stations[idx].toggle()
                            selected_port_idx = idx
                    elif k == pygame.K_c and cmd_ctrl:
                        text = get_selected_text(
                            _get_visible(shared_log, max_log_lines,
                                         scroll_offset))
                        if text:
                            clipboard_copy(text)
                            shared_log.add(f"  COPIED {len(text)} chars",
                                           DIM_CYAN)
                    elif k == pygame.K_a and cmd_ctrl:
                        vis = _get_visible(shared_log, max_log_lines,
                                           scroll_offset)
                        sel_anchor = (0, 0)
                        sel_end = (max(0, len(vis) - 1), 200)
                    elif k == pygame.K_r and not cmd_ctrl:
                        shared_log.add("▶ REBUILD + RE-FLASH ALL", AMBER)

                        def _rebuild():
                            do_build()
                            if build_ok[0]:
                                port_mgr.start_all_ready()
                                port_mgr.rearm_all()

                        threading.Thread(target=_rebuild, daemon=True).start()
                    elif k == pygame.K_f and not cmd_ctrl:
                        if selected_port_idx < n_stations:
                            s = stations[selected_port_idx]
                            shared_log.add(f"▶ RE-FLASH {s.short}", AMBER)
                            s.rearm.set()
                    elif k == pygame.K_b and not cmd_ctrl:
                        cfg["no_build"] = not cfg["no_build"]
                        shared_log.add(
                            f"  BUILD: {'SKIP' if cfg['no_build'] else 'ON'}",
                            AMBER)
                    elif k == pygame.K_m and not cmd_ctrl:
                        cfg["monitor"] = not cfg["monitor"]
                        shared_log.add(
                            f"  MONITOR: {'ON' if cfg['monitor'] else 'OFF'}",
                            AMBER)
                    elif k == pygame.K_e and not cmd_ctrl:
                        if len(env_cycle) < 2:
                            shared_log.add("  no other env to switch to",
                                           DIM_AMBER)
                        else:
                            nxt = (env_cycle.index(cfg["env"]) + 1) % len(
                                env_cycle)
                            set_env(env_cycle[nxt])
                            shared_log.add(
                                f"  ENV: {cfg['env']} (upload via "
                                f"{cfg['protocol']}) — press [R] to build "
                                "+ flash", AMBER)
                    elif k == pygame.K_p and not cmd_ctrl:
                        port_mgr.scanning = not port_mgr.scanning
                        shared_log.add(
                            "  AUTODETECT: "
                            f"{'ON' if port_mgr.scanning else 'PAUSED'}",
                            AMBER)

                elif event.type == pygame.MOUSEWHEEL:
                    if event.y > 0:
                        scroll_offset = min(
                            scroll_offset + 3,
                            max(0, shared_log.total - max_log_lines))
                        auto_scroll = False
                    elif event.y < 0:
                        scroll_offset = max(0, scroll_offset - 3)
                        auto_scroll = scroll_offset == 0

                elif (event.type == pygame.MOUSEBUTTONDOWN
                      and event.button == 1):
                    mx, my = event.pos
                    if my >= log_y_global:
                        vis = _get_visible(shared_log, max_log_lines,
                                           scroll_offset)
                        sel_anchor = pos_to_line_char(mx, my, log_y_global,
                                                      len(vis))
                        sel_end = sel_anchor
                        sel_dragging = True

                elif event.type == pygame.MOUSEMOTION and sel_dragging:
                    mx, my = event.pos
                    vis = _get_visible(shared_log, max_log_lines,
                                       scroll_offset)
                    sel_end = pos_to_line_char(mx, my, log_y_global, len(vis))

                elif (event.type == pygame.MOUSEBUTTONUP
                      and event.button == 1):
                    sel_dragging = False
                    if sel_anchor == sel_end:
                        sel_anchor = sel_end = None

            if auto_scroll:
                scroll_offset = 0

            screen.fill(BG)

            # ── HEADER ────────────────────────────────────────────────
            pygame.draw.rect(screen, HEADER_BG, (0, 0, win_w, header_h))
            pygame.draw.line(screen, DARK_CYAN, (0, header_h),
                             (win_w, header_h))
            screen.blit(font_big.render("JUMPERLESS FLASH TERMINAL", True,
                                        rainbow_line_color(tick // 4, n_hues,
                                                           tick)), (20, 6))
            clock_surf = font.render(time.strftime("%H:%M:%S"), True, DIM_CYAN)
            screen.blit(clock_surf, (win_w - clock_surf.get_width() - 20, 8))

            total_flashed = sum(s.count for s in stations)
            active = sum(1 for s in stations if s.state in BUSY_STATES)
            plugged = sum(1 for s in stations if s.port_exists())
            screen.blit(font.render(
                f"ENV: {cfg['env']}   PORTS: {n_stations}   "
                f"PLUGGED: {plugged}   ACTIVE: {active}   "
                f"TOTAL FLASHED: {total_flashed}", True, DIM_CYAN), (20, 34))

            # ── PORT STATUS PANEL ─────────────────────────────────────
            panel_y = header_h + 1
            pygame.draw.rect(screen, PANEL_BG,
                             (0, panel_y, win_w, port_panel_h))
            pygame.draw.line(screen, DARK_CYAN, (0, panel_y + port_panel_h),
                             (win_w, panel_y + port_panel_h))

            py = panel_y + 6
            for i, s in enumerate(stations):
                bhue = port_base_hue(i, max(n_stations, 1))
                if not s.enabled:
                    col, status_text, sp = DIM_RED, "DISABLED", ""
                else:
                    col = state_color(bhue, s.state, tick)
                    status_text = s.state
                    sp = (spinner_char(tick) + " "
                          if s.state in BUSY_STATES + (S_WAITING,) else "")

                lit = s.applies() if s.is_probe else s.port_exists()
                dot = hsv_rgb(bhue, 0.8, 1.0) if lit else DIM_RED
                if not s.enabled:
                    dot = (60, 60, 60)
                pygame.draw.circle(screen, dot, (30, py + 8), 5)
                if i == selected_port_idx:
                    pygame.draw.circle(screen, WHITE, (30, py + 8), 8, 1)

                time_str = ""
                if s.state in BUSY_STATES and s.flash_start_time:
                    time_str = f"  {time.time() - s.flash_start_time:.1f}s"
                elif s.last_flash_duration is not None:
                    time_str = f"  LAST: {s.last_flash_duration:.1f}s"

                if s.is_probe:
                    label = f"PROBE {cfg['protocol']}"
                else:
                    tty = s.port
                    label = f"USB {s.short}"
                    if tty:
                        label += f" ({tty.rsplit('/', 1)[-1]})"
                screen.blit(font.render(
                    f"[{i + 1}] {label:<32s}  {sp}{status_text:<18s}  "
                    f"FLASHED: {s.count}{time_str}", True, col), (44, py))

                if s.enabled and s.state in BUSY_STATES:
                    strip_w = 168
                    bar_x = win_w - strip_w - 8
                    pygame.draw.rect(screen, PANEL_BG,
                                     (bar_x - 4, py - 1, strip_w + 8,
                                      LINE_H + 2))
                    bar_w, bar_h, bar_y = 108, 11, py + 5
                    pygame.draw.rect(screen, (8, 40, 40),
                                     (bar_x, bar_y, bar_w, bar_h))
                    fill = max(0, min(100, s.progress))
                    if fill:
                        pygame.draw.rect(screen, hsv_rgb(bhue, 0.9, 0.9),
                                         (bar_x, bar_y,
                                          int(bar_w * fill / 100), bar_h))
                    screen.blit(font.render(f"{fill}%" if fill else "…", True,
                                            col), (bar_x + bar_w + 6, py + 2))
                py += 32

            # ── LOG AREA ──────────────────────────────────────────────
            log_y = log_y_global = panel_y + port_panel_h + 4
            visible = _get_visible(shared_log, max_log_lines, scroll_offset)

            if sel_anchor is not None and sel_end is not None:
                a, b = sorted((sel_anchor, sel_end))
                hl = pygame.Surface((win_w, LINE_H), pygame.SRCALPHA)
                hl.fill((20, 80, 80, 140))
                for li in range(a[0], min(b[0] + 1, len(visible))):
                    screen.blit(hl, (0, log_y + li * LINE_H))

            y = log_y
            for vi, (text, _colour, pidx) in enumerate(visible):
                if pidx is not None and pidx < n_stations:
                    rc = state_color(port_base_hue(pidx, max(n_stations, 1)),
                                     stations[pidx].state, tick)
                else:
                    rc = rainbow_line_color(vi, n_hues, tick)
                screen.blit(font.render(text, True, rc), (16, y))
                y += LINE_H

            # ── FOOTER ────────────────────────────────────────────────
            fy = win_h - footer_h
            pygame.draw.line(screen, DARK_CYAN, (0, fy), (win_w, fy))
            pygame.draw.rect(screen, HEADER_BG, (0, fy, win_w, footer_h))
            screen.blit(font.render(
                f"  [Q]uit [1-9]toggle [R]ebuild+reflash [F]reflash "
                f"[E]nv:{cfg['env']} "
                f"[B]uild:{'SKIP' if cfg['no_build'] else 'ON'} "
                f"[M]onitor:{'ON' if cfg['monitor'] else 'OFF'} "
                f"[P]scan:{'ON' if port_mgr.scanning else 'PAUSED'} "
                f"Ctrl+C:copy", True, DIM_CYAN), (10, fy + 5))

            for _ in range(6):
                rx = random.randint(0, win_w - 1)
                ry = random.randint(0, win_h - 1)
                screen.set_at((rx, ry),
                              rainbow_line_color(rx + ry, n_hues, tick))

            screen.blit(scanlines, (0, 0))
            pygame.display.flip()
            clock.tick(FPS)
            tick += 1

    except KeyboardInterrupt:
        pass
    finally:
        port_mgr.shutdown()
        pygame.quit()
        stations = port_mgr.get_stations()
        total = sum(s.count for s in stations)
        print(f"\nFlashed {total} board(s) across {len(stations)} port(s).")


if __name__ == "__main__":
    main()
