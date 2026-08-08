#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""MicroPython + filesystem sanity: exec, gc, jfs write/read/delete
round-trip, OLED calls don't fault."""

from jl import jl_exec, parse_kv, check, finish

out = jl_exec("""
import gc
gc.collect()
print("free=", gc.mem_free())
print("math=", sum(range(100)))

path = "/hil_selftest.txt"
f = jfs.open(path, "w")
jfs.write(f, "jumperless hil 12345")
jfs.close(f)
f = jfs.open(path, "r")
data = jfs.read(f, 64)
jfs.close(f)
print("roundtrip=", 1 if data == "jumperless hil 12345" else 0)
jfs.remove(path)
print("removed=", 0 if jfs.exists(path) else 1)

oled_clear()
oled_print("HIL test")
oled_clear()
print("oled= 1")
""", timeout=25)

vals = parse_kv(out)
check(vals.get("free", 0) > 10000, f"MicroPython heap has headroom ({vals.get('free')} bytes)")
check(vals.get("math") == 4950, "basic exec math works")
check(vals.get("roundtrip") == 1, "jfs write/read round-trip")
check(vals.get("removed") == 1, "jfs remove works")
check(vals.get("oled") == 1, "OLED print/clear calls return")

finish("test_micropython_fs")
