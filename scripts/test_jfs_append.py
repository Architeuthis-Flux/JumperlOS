# Self-check for FatFS append mode via jfs (run on the device).
#
# Regression test for FatFSImpl::_getFlags (lib/FatFS/src/FatFS.h): "a"/"a+"
# used to map to FA_CREATE_ALWAYS|FA_OPEN_APPEND, and FA_CREATE_ALWAYS makes
# f_open() truncate an existing file - so every append-mode open wiped the
# file and only the last write session survived.
#
# Run on the device (paste into the REPL, or exec via the jumperless-v5
# skill transport).
#
# Expected output: "PASS jfs append"

PATH = '/append_selftest.tmp'

if jfs.exists(PATH):
    jfs.remove(PATH)

# Reopening in 'a' must preserve prior content ('a' on a missing file creates it)
for i in range(3):
    f = jfs.open(PATH, 'a')
    f.write('line%d\n' % i)
    f.close()

f = jfs.open(PATH, 'r')
content = f.read()
f.close()
assert content == 'line0\nline1\nline2\n', 'append truncated: %r' % content

# 'a+' must preserve existing content too
f = jfs.open(PATH, 'a+')
f.write('line3\n')
f.seek(0)
content = f.read()
f.close()
jfs.remove(PATH)
assert content == 'line0\nline1\nline2\nline3\n', "'a+' truncated: %r" % content

print('PASS jfs append')
