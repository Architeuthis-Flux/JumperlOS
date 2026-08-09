"""Self-check for erase_fs_partition._choose_port().

Run it directly - no framework, no hardware:

    python3 scripts/test_erase_fs_port_choice.py

Guards the one destructive decision in the erase hook: which tty gets the
1200 bps touch (and therefore which board's FatFS partition gets wiped).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from erase_fs_partition import _choose_port  # noqa: E402

JL1 = "/dev/cu.usbmodemJLV5port1"
JL2 = "/dev/cu.usbmodemJLV5port3"
STALE = "/dev/cu.usbmodem701"

# No Jumperless visible (unplugged, or already in BOOTSEL): pass through and
# let the existing BOOTSEL timeout report it.
assert _choose_port(STALE, []) == STALE
assert _choose_port("", []) == ""

# $UPLOAD_PORT is a real Jumperless: honour it even when others are present.
assert _choose_port(JL1, [JL1]) == JL1
assert _choose_port(JL2, [JL1, JL2]) == JL2

# The bug this exists for: a stale IDE customPort names a tty that is gone,
# and exactly one Jumperless is connected.
assert _choose_port(STALE, [JL1]) == JL1
assert _choose_port("", [JL1]) == JL1

# Ambiguous AND wrong: must abort rather than wipe an arbitrary board.
for bad_port in (STALE, ""):
    try:
        _choose_port(bad_port, [JL1, JL2])
    except SystemExit as e:
        assert e.code == 1, e.code
    else:
        raise AssertionError(
            "_choose_port(%r, [2 boards]) should have exited" % bad_port)

print("ok")
