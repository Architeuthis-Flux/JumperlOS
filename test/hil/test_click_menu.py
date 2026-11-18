#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The click menu on the terminal: its columns, its keys and the one line it
leaves behind, opened by the K command and by a click.

Drives port 1 the way a person at a terminal would - K, arrows, Enter, q -
and reads what MenuTermView painted: the lit row of each step is the cell it
drew in inverse video. Every move is checked before the next key goes out, so
a key the board did not take cannot turn the rest of the walk into something
else. The one pick it makes is Routing Options > Stack > Paths > <the value
already set>; the board's state is compared with a snapshot at the end.

Needs port 1 free (quit the jumperless client)."""

import re
import time

import serial  # pyserial

from jl import (board_state_capture, check, finish, port1_path, skip,
                state_fingerprint)

UP, DOWN, RIGHT, LEFT = b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D"
_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# A lit cell: inverse, bold when it is the wheel's column, its colour, the text.
_LIT = re.compile(r"\x1b\[7m(?:\x1b\[1m)?(?:\x1b\[38;5;\d+m)? ([^\x1b]*)\x1b\[0m")

before = board_state_capture()
if before is None:
    skip("no state snapshot from the board")
m = re.search(r"stackPaths: *(\d+)", before)
paths_now = m.group(1) if m else None

ser = serial.Serial(port1_path(), 115200, timeout=0.005)


waits = []  # seconds from each key to the first byte it painted


def drain(first=1.5, quiet=0.2):
    """What the board sends: up to `first` for it to start, then until it has
    been quiet. A fixed quiet window alone reads a slow repaint (the board
    busy with a flash save) as no repaint, and every later step one key off."""
    buf, t0 = b"", time.time()
    last = None
    while True:
        chunk = ser.read(4096)
        now = time.time()
        if chunk:
            if last is None:
                waits.append(now - t0)
            buf += chunk
            last = now
        elif last is None and now - t0 > first:
            break
        elif last is not None and now - last > quiet:
            break
    return buf.decode(errors="replace")


def key(data):
    """Send one key; what the board painted for it."""
    ser.write(data)
    ser.flush()
    return drain()


resent = 0  # keys the board painted nothing for, sent again


def press(data, want, tries=3):
    """Send a key and say whether it left `want` lit. Port 1 loses a key now
    and then (test_stats_pages.py's step() resends for the same reason), so a
    key the board painted NOTHING for is sent again; a key that lit some other
    row is not - that is a wrong move, and sending more would compound it."""
    global resent
    for attempt in range(tries):
        painted = key(data)
        if painted:
            return lit(painted) == want
        if attempt < tries - 1:
            resent += 1
    return False


def lit(painted):
    """The row the step left lit: the last inverse cell it drew, else None."""
    cells = _LIT.findall(painted)
    return cells[-1].rstrip(" ›").strip() if cells else None


def walk_to(label, data, limit=20):
    """Press `data` until `label` is the lit row. False if it never is."""
    for _ in range(limit):
        if press(data, label, tries=1):
            return True
    return False


def plain(painted):
    return [ln.strip() for ln in _ANSI.sub("", painted).replace("\r", "\n").split("\n") if ln.strip()]


try:
    # The first byte on a fresh connection triggers the connection-init, which
    # eats what follows it (see test_stats_pages.py): prime with a newline.
    ser.write(b"\n")
    ser.flush()
    time.sleep(1.0)
    drain()

    opened = key(b"K\r")
    text = _ANSI.sub("", opened)
    check("Rails" in text and "Routing Options" in text, "K draws the root list")
    check("q close" in text, "... with its key hint under it")
    check("\x1b[2J" not in opened and "\x1b[?1049h" not in opened,
          "... inline: no screen clear, no alternate screen")
    is_open = lit(opened) == "Rails"
    check(is_open, "the menu opens on Rails")

    if is_open:
        check(press(DOWN, "Connect"), "down is one detent: Connect")
        check(press(UP, "Rails"), "up is one back: Rails")
        at_connect = press(DOWN, "Connect")
        check(at_connect and press(RIGHT, "Add"), "right is the click: Connect's options open as a column")
        check(at_connect and press(DOWN, "Remove"), "... and down moves in it: Remove")
        check(at_connect and press(LEFT, "Connect"), "left is one step back, the menu still open")

        # Routing Options > Stack > Paths > the value already set: a pick that
        # changes nothing. Up from Rails wraps to the last root row. (The first
        # turn back after a Left paints nothing - the wheel's own behaviour -
        # so these are walks, not single presses.)
        on_path = (walk_to("Routing Options", UP) and press(RIGHT, "Stack")
                   and press(RIGHT, "ADCs") and walk_to("Paths", UP)
                   and press(RIGHT, "0"))
        check(on_path, "four columns deep: Routing Options > Stack > Paths > its options")
        if on_path and paths_now in ("0", "1", "2", "3", "4") and (paths_now == "0" or walk_to(paths_now, DOWN, 5)):
            picked = key(b"\r")
            lines = plain(picked)
            want = f"Routing Options > Stack > Paths > {paths_now}"
            check(want in lines, f"Enter picks: the columns fold to one line, '{want}'")
            check(picked.count("\x1b[2K") >= 14, "... and the block is erased, not left in the scrollback")
            is_open = False
        else:
            check(False, f"could not reach the option for stackPaths = {paths_now}; nothing picked")

    if is_open:
        key(b"q")
    answered = key(b"?")
    check("firmware version" in answered, "the terminal is the main menu's again afterwards")

    # A cancel leaves no line.
    reopened = key(b"K\r")
    if lit(reopened) == "Rails":
        closed = key(b"q")
        check(not plain(closed), "q closes it and leaves nothing behind")
        check("firmware version" in key(b"?"), "... with the main menu back")
    else:
        check(False, "K opens the menu a second time")

    # Ctrl-C is not a menu key: it ends the session and is left for the main
    # loop, so a script (or the flash recipe) can always get a board out of
    # a menu. Same for anything else that is not the menu's input.
    if lit(key(b"K\r")) == "Rails":
        check(not plain(key(b"\x03")), "Ctrl-C closes the menu and leaves nothing behind")
        check("firmware version" in key(b"?"), "... with the main menu back")
    else:
        check(False, "K opens the menu a third time")

    # A wheel click brings up the same columns. The terminal's own click is a
    # lone space on an empty line (line mode), which raises the same button
    # edge the wheel does - so this is the wheel's way in, minus the finger.
    clicked = key(b" ")
    if lit(clicked) == "Rails":
        check("q close" in _ANSI.sub("", clicked), "a click (not K) opens the same columns on the terminal")
        check(press(DOWN, "Connect"), "... and the keys drive that session too")
        check(not plain(key(b"q")), "... q closes it, nothing left behind")
    else:
        check(False, "a click (not K) opens the same columns on the terminal")
finally:
    # Whatever happened above, leave no menu open and no stray key on the line.
    ser.write(b"q")
    ser.flush()
    time.sleep(0.4)
    ser.write(b"\x15")
    ser.flush()
    drain()
    ser.close()

print(f"  info: {resent} key(s) sent again after the board painted nothing for them")
if waits:
    print(f"  info: key to first paint - median {sorted(waits)[len(waits) // 2] * 1000:.0f} ms, "
          f"slowest {max(waits) * 1000:.0f} ms over {len(waits)} keys")

after = board_state_capture()
check(after is not None and state_fingerprint(after) == state_fingerprint(before),
      "the board's state is what it was")

finish("test_click_menu")
