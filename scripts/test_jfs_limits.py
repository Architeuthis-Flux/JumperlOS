# Self-check for JFS hardening fixes (run on the device).
#
# Covers:
#  - jfs.open() FAILS (OSError) when 4 files are already open, instead of
#    returning an untracked handle that can never be closed (leaked forever)
#  - seek(pos, SEEK_END) with pos > file size returns False instead of
#    underflowing to a ~4GB offset that FatFS f_lseek write-mode expansion
#    would satisfy by allocating every free cluster on the volume
#
# Run on the device (paste into the REPL, or exec via the jumperless-v5
# skill transport).
#
# Expected output: "PASS jfs limits"

P = '/limits_selftest.tmp'

f = jfs.open(P, 'w')
f.write('x' * 10)
f.close()

# 1) Concurrent-open limit: 4 opens succeed, the 5th raises, and slots free up
handles = [jfs.open(P, 'r') for _ in range(4)]
raised = False
try:
    f5 = jfs.open(P, 'r')
    f5.close()
except OSError:
    raised = True
assert raised, '5th concurrent open should raise OSError'
for h in handles:
    h.close()
f = jfs.open(P, 'r')  # slots must be reusable after close
f.close()

# 2) SEEK_END guard: seeking farther back than the file is long must fail
f = jfs.open(P, 'r+')
assert f.seek(5, 2), 'seek 5 back from end should work'
assert not f.seek(100, 2), 'seek past start via SEEK_END should fail'
f.close()
assert jfs.stat(P)[6] == 10, 'file size changed by a failed seek'

jfs.remove(P)
print('PASS jfs limits')
