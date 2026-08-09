"""
PlatformIO Pre-Upload Hook: erase the FatFS partition before flashing.

Wired into [env:jumperless_v5_erase]. Runs immediately before the
platform's normal upload sequence so flashing this env both wipes user
data AND installs a fresh firmware in one step. The firmware itself is
unchanged - if you want to preserve user data, just flash
[env:jumperless_v5] instead.

picotool v2.0.0 (the version bundled with the earlephilhower platform)
does NOT expose an `erase` subcommand. We achieve the same effect by
`picotool load`-ing a temp file full of 0xFF bytes - the same value an
actual flash erase produces - at FS_START, sized to FS_END - FS_START.

Flow when `pio run -e jumperless_v5_erase -t upload` runs:

  1. SCons runs this pre-action on the `upload` alias.
     a. Detect whether a Pico is already in BOOTSEL via
        `picotool info -d` (zero exit + "type:" line means yes).
     b. If not, do the standard 1200 bps DTR=0 touch and poll picotool
        until BOOTSEL appears (or give up). The tty comes from the USB
        descriptors rather than $UPLOAD_PORT when the two disagree - see
        _choose_port() for why $UPLOAD_PORT goes stale.
     c. Generate a $BUILD_DIR/fs_erase.bin blob of 0xFF bytes sized to
        the FatFS partition pinned by scripts/lock_fs_partition.py
        (FS_END - FS_START), then run
        `picotool load fs_erase.bin -t bin -o $FS_START -v
            --ignore-partitions`.
        The erase touches ONLY the 4 MB FatFS partition - it cannot
        reach the sketch region or the EEPROM page because we read
        FS_START / FS_END back from the env that lock_fs_partition.py
        just set, never hardcoding addresses here.
     d. We do NOT pass `-x`, so picotool leaves the device in BOOTSEL
        ready for the platform's normal load step.

  2. The platform's existing upload_actions then run:
     - BeforeUpload: fastpath detects "already in BOOTSEL", skips
       its own 1200 bps touch.
     - $UPLOADCMD: `picotool load -v -x $SOURCES` -> firmware
       written + execute (-x reboots into the new sketch).

On the first boot of the freshly-flashed firmware, FatFS sees a blank
partition (all 0xFF), can't mount it, and falls back to the existing
_autoFormat=true path in FatFSImpl::begin() to create a fresh empty
FAT volume. SPIFTL likewise sees no valid metadata and re-initialises.
"""

import os
import re
import subprocess
import sys
import time
from os.path import join

try:
    Import("env")  # noqa: F821 - provided by SCons / PlatformIO
except NameError:
    # Imported outside SCons (scripts/test_erase_fs_port_choice.py).
    env = None

# Jumperless USB IDs / serial prefixes, mirroring
# autoUploader/flash_loop_gui.py.
JL_USB_IDS = {
    (0x1D50, 0xACAB),  # JumperlOS
    (0xACAB, 0x1312),  # OG factory firmware
}
JL_SERIAL_PREFIXES = ("JLV5port", "JLOGport")


def _picotool_path(env):
    return join(
        env.PioPlatform().get_package_dir("tool-picotool-rp2040-earlephilhower") or "",
        "picotool",
    )


def _bootsel_device_count(picotool):
    """Mirror platform-raspberrypi/main.py:get_num_rpxxxx_devs()."""
    try:
        out = subprocess.run(
            '"%s" info -d' % picotool,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            shell=True,
            timeout=5,
        ).stdout
        return out.count(b"type:")
    except Exception:
        return 0


def _tty_sort_key(device):
    """Sort /dev/cu.usbmodem646 after 618, not before it."""
    m = re.search(r"(\d+)$", device)
    return (int(m.group(1)) if m else 0, device)


def _jumperless_ttys():
    """{USB location -> lowest-numbered tty} for each connected Jumperless.

    A board exposes several CDCs and enters BOOTSEL on a 1200 bps touch of any
    of them, so one tty per board is enough. Keyed by USB location because all
    Jumperlesses report the same serial string, which makes macOS fall back to
    anonymous /dev/cu.usbmodemNNN names once two are plugged in.
    """
    try:
        import serial.tools.list_ports  # PIO ships pyserial
    except Exception:  # pylint: disable=broad-except
        return {}
    boards = {}
    for p in serial.tools.list_ports.comports():
        sn = p.serial_number or ""
        if (p.vid, p.pid) not in JL_USB_IDS and not sn.startswith(JL_SERIAL_PREFIXES):
            continue
        boards.setdefault((p.location or p.device).split(":")[0], []).append(p.device)
    return {k: sorted(v, key=_tty_sort_key)[0] for k, v in boards.items()}


def _choose_port(upload_port, ttys):
    """Decide which tty to touch, trusting USB descriptors over $UPLOAD_PORT.

    $UPLOAD_PORT is not reliable: the PlatformIO IDE keeps a per-project
    "customPort" in its extension global state and appends it as
    --upload-port, which overrides platformio.ini. A port picked once (e.g. an
    anonymous /dev/cu.usbmodemNNN handed out while several boards shared a hub)
    keeps being passed long after that name is gone.

    Erasing the wrong board's filesystem is unrecoverable, so guessing is only
    allowed when exactly one Jumperless is present.
    """
    if not ttys:
        return upload_port
    if upload_port in ttys:
        return upload_port
    if len(ttys) == 1:
        if upload_port:
            print("[erase_fs] $UPLOAD_PORT %s is not a Jumperless; using %s instead"
                  % (upload_port, ttys[0]))
        return ttys[0]
    sys.stderr.write(
        "[erase_fs] %d Jumperless boards connected (%s) and $UPLOAD_PORT (%s) is "
        "not one of them. Refusing to guess whose filesystem to erase - pass "
        "--upload-port explicitly.\n"
        % (len(ttys), ", ".join(ttys), upload_port or "unset")
    )
    sys.exit(1)


def _resolve_upload_port(upload_port):
    return _choose_port(upload_port, sorted(_jumperless_ttys().values()))


def _trigger_bootsel(upload_port):
    """1200 bps DTR-low touch - same trick the Arduino IDE uses."""
    if not upload_port:
        return
    try:
        import serial  # PIO ships pyserial

        s = serial.Serial(upload_port, 1200)
        s.dtr = False
        s.close()
    except Exception as e:  # pylint: disable=broad-except
        # Often the port has already vanished because the device rebooted
        # itself into BOOTSEL on a previous touch. That's fine - we still
        # poll for BOOTSEL below.
        print("[erase_fs] 1200bps touch on %s failed (often benign): %s" % (upload_port, e))


def _wait_for_bootsel(picotool, timeout_s=8.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if _bootsel_device_count(picotool) > 0:
            return True
        time.sleep(0.25)
    return False


def _erase_fs(target, source, env):  # pylint: disable=W0613,W0621
    fs_start = env.get("FS_START")
    fs_end = env.get("FS_END")
    if fs_start is None or fs_end is None:
        sys.stderr.write(
            "[erase_fs] FS_START / FS_END not set - is lock_fs_partition.py "
            "running before this script? Aborting upload to avoid a "
            "guess-and-erase.\n"
        )
        sys.exit(1)

    fs_start = int(fs_start)
    fs_end = int(fs_end)
    size = fs_end - fs_start

    picotool = _picotool_path(env)
    if not picotool:
        sys.stderr.write("[erase_fs] picotool package path not found\n")
        sys.exit(1)

    if _bootsel_device_count(picotool) == 0:
        upload_port = _resolve_upload_port(env.subst("$UPLOAD_PORT"))
        print("[erase_fs] Triggering BOOTSEL via 1200bps touch on %s" % upload_port)
        _trigger_bootsel(upload_port)
        if not _wait_for_bootsel(picotool):
            sys.stderr.write(
                "[erase_fs] No RPxxxx device entered BOOTSEL within timeout. "
                "Hold BOOTSEL while plugging in and re-run, or flash "
                "[env:jumperless_v5] first to land a known-good firmware.\n"
            )
            sys.exit(1)
    else:
        print("[erase_fs] Pico already in BOOTSEL, skipping 1200bps touch.")

    build_dir = env.subst("$BUILD_DIR")
    os.makedirs(build_dir, exist_ok=True)
    blob_path = join(build_dir, "fs_erase.bin")
    print(
        "[erase_fs] Generating %d-byte 0xFF blob -> %s (%.2f MB FatFS partition)"
        % (size, blob_path, size / 1024.0 / 1024.0)
    )
    with open(blob_path, "wb") as f:
        # 64 KB chunks to keep peak Python alloc bounded on small hosts.
        chunk = b"\xff" * (64 * 1024)
        remaining = size
        while remaining > 0:
            n = min(remaining, len(chunk))
            f.write(chunk[:n])
            remaining -= n

    cmd = [
        picotool,
        "load",
        blob_path,
        "-t", "bin",
        "-o", "0x%08X" % fs_start,
        "-v",
        "--ignore-partitions",
    ]
    print("[erase_fs] %s" % " ".join('"%s"' % a if " " in a else a for a in cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.stderr.write("[erase_fs] picotool load (FF blob) failed (rc=%d); aborting\n" % rc)
        sys.exit(rc)

    print("[erase_fs] FatFS partition erased. Continuing with firmware upload...")


if env is not None:
    env.AddPreAction("upload", _erase_fs)  # noqa: F821
    print("[erase_fs] Hooked pre-upload erase for [env:%s]" % env["PIOENV"])
