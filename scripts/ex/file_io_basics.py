"""
File I/O basics with the jfs filesystem module.

A guided tour of reading and writing files on the Jumperless flash
filesystem, framed as a tiny data logger. Everything shown here works
the same way in your own scripts.

jfs is available globally - no import needed. (MicroPython's built-in
open() and the os module work on the same files too, if you prefer them.)
"""

import time

LOG = '/python_scripts/file_io_demo.log'


def section(title):
    print()
    print("-" * 46)
    print(" " + title)
    print("-" * 46)
    time.sleep(0.3)


print("=" * 46)
print(" jfs File I/O Basics")
print("=" * 46)

# ----------------------------------------------------------------------
section("1. Writing a file ('w' creates or truncates)")

f = jfs.open(LOG, 'w')
f.print("Jumperless data log")          # like print(), but into the file
f.print("column:", "voltage")           # multiple args, joined with spaces
f.flush()                               # commit to flash NOW (see note)
f.close()

print("Wrote header with f.print() - it adds a newline for you.")
print("NOTE: writes are buffered. Data is committed when you close()")
print("or seek(), or when you call f.flush() yourself. If your script")
print("might crash or lose power mid-run, flush() at checkpoints.")

# ----------------------------------------------------------------------
section("2. Append mode ('a') - the data logger pattern")

print("Reopening in 'a' adds to the end instead of overwriting,")
print("so your log survives across runs, resets, and reboots.")
print()

for i in range(3):
    voltage = adc_get(0)                 # read a real "sensor"
    f = jfs.open(LOG, 'a')               # reopen fresh each sample -
    f.print("sample", i, "=", voltage)   # exactly what a logger does
    f.close()
    print("  logged sample %d: %.3f V" % (i, voltage))
    time.sleep(0.2)

f = jfs.open(LOG, 'r')
content = f.read()
f.close()
print("\nThe whole log so far:")
print(content)
lines = content.count('\n')
assert lines == 5, "expected 5 log lines, found %d - append lost data!" % lines
print("All 5 lines survived reopening - append works.")

# ----------------------------------------------------------------------
section("3. Reading: size, seek, tell, chunks")

f = jfs.open(LOG, 'r')
print("file size:", f.size(), "bytes")
print("position after open:", f.tell())

first = f.read(19)                       # read a fixed number of bytes
print("first 19 bytes: %r" % first)
print("position now:", f.tell())

f.seek(0)                                # jump back to the start
print("after seek(0), read() returns the rest of the file:")
print(f.read())

# whence=2 (jfs.SEEK_END) counts BACK from the end of the file:
f.seek(5, jfs.SEEK_END)
print("last 5 bytes: %r" % f.read())

# ...and seeking farther back than the file is long just fails safely:
ok = f.seek(10000, jfs.SEEK_END)
assert not ok, "seek past start should return False"
print("seek(10000, SEEK_END) returned False - too far, safely refused.")
f.close()

# ----------------------------------------------------------------------
section("4. Directories: exists, listdir, stat, rename, remove")

print("exists:", jfs.exists(LOG))
print("size via stat:", jfs.stat(LOG)[6], "bytes")

moved = '/python_scripts/file_io_demo_old.log'
jfs.rename(LOG, moved)
print("renamed to", moved)
print("/python_scripts now contains:")
for name in jfs.listdir('/python_scripts'):
    print("   ", name)

jfs.remove(moved)
print("removed the demo log - cleanup done.")

# ----------------------------------------------------------------------
section("5. The open-file limit (and why 'with' is your friend)")

print("Up to 8 jfs files can be open at once. A 9th open raises OSError:")
scratch = '/python_scripts/limit_demo.tmp'
f = jfs.open(scratch, 'w')
f.write('x')
f.close()

handles = [jfs.open(scratch, 'r') for _ in range(8)]
try:
    jfs.open(scratch, 'r')
    raised = False
except OSError:
    raised = True
    print("  9th open -> OSError, as expected")
assert raised, "9th concurrent open should raise OSError"

for h in handles:
    h.close()

# 'with' closes the file for you even if your code raises:
with jfs.open(scratch, 'r') as f:
    print("  with-block read: %r" % f.read())
jfs.remove(scratch)

print()
print("=" * 46)
print(" PASS - file I/O basics all good")
print("=" * 46)
