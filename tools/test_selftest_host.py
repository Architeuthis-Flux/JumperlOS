#!/usr/bin/env python3
"""Smallest check that fails if selftest_host's report parsing breaks.

Run: python3 tools/test_selftest_host.py
"""

import json

from selftest_host import SENTINEL_RE, TEST_NAMES, scan_reports

noisy = (
    "boot garbage\r\nmenu text ::SELF (not a report)\r\n"
    '::SELFTEST::{"id":"E4638C","fw":"5.5.5.5","pass":false,'
    '"results":{"probe_cable":"pass","crossbar":"fail","tip_voltage":"pass",'
    '"psram":"skip","peripherals":"pass"},"details":{}}::END::\r\nmore noise'
)

m = SENTINEL_RE.search(noisy)
assert m, "sentinel not found in noisy buffer"
report = json.loads(m.group(1).strip())
assert report["id"] == "E4638C"
assert report["pass"] is False
assert set(report["results"].keys()) == set(TEST_NAMES)
assert report["results"]["crossbar"] == "fail"

# "none" payload (no stored report) must also match so --report handles it
m2 = SENTINEL_RE.search("::SELFTEST::none::END::")
assert m2 and m2.group(1).strip() == "none"

# Retry rounds: firmware prints one report per round; the host must see BOTH
# (keeping the last), so a round-1 fail can be superseded by a round-2 pass.
passing = (
    '::SELFTEST::{"id":"E4638C","fw":"5.5.5.5","pass":true,'
    '"results":{"probe_cable":"pass","crossbar":"pass","tip_voltage":"pass",'
    '"psram":"skip","peripherals":"pass"},"details":{}}::END::'
)
payloads, rest = scan_reports(noisy + "retry text\r\n" + passing + "tail")
assert len(payloads) == 2
assert json.loads(payloads[0])["pass"] is False
assert json.loads(payloads[1])["pass"] is True
assert rest == "tail"

# Partial trailing report stays in the buffer for the next chunk
payloads, rest = scan_reports('x::SELFTEST::{"pass":')
assert payloads == [] and rest.endswith('{"pass":')

print("ok")
