"""Put the bench back exactly as snapshot.py found it (2026-09-07).
Order matters:
  1. leave any menu/mode, clear the scratch context (slot 3), rails to 0
  2. restore every captured file's BYTES or ABSENCE while slot 3 is still active
     (slot0.yaml must not be overwritten by an autosave of the wrong context)
  3. delete state files the tests may have created (slot1/2/4-7, undo.hist, extra run files)
  4. switch to slot 0  ->  the live state is Kevin's circuit again
  5. restore slot3.yaml (now inactive)
  6. reboot so config.txt / undo history / everything in RAM comes from the restored files
  7. verify: fingerprint diff of a fresh Y capture against the snapshot, active slot, rails
"""
import sys, os, json, time
sys.path.insert(0, "/Users/kevinsanto/Documents/GitHub/JumperlOS/test/hil")
import jl
SP = os.path.dirname(os.path.abspath(__file__))
rec = json.load(open(f"{SP}/snapshot_latest.json"))
snap_yaml = open(rec["state_yaml"]).read()
print("snapshot:", rec["ts"], "active was", rec["active"])

# 1. calm the board - but switch to slot 3 FIRST. Leaving a slot triggers a deferred save of that slot's
#    state; if `x` ran in slot 0 before the switch, that save would land on top of the slot0.yaml bytes
#    restored below (seen 2026-09-09 16:10: slot0.yaml came back as the 619-byte empty state).
jl.port1_command("", 1.0)
slot, path = jl.active_context()
print("active at start:", slot, path)
if slot != 3:
    jl.port1_command("<3", 4.0)
    time.sleep(3.0)   # let the deferred save of the old slot land before any file is restored
jl.jl_exec("probe_tap(0); set_switch_position(1)", timeout=10)
jl.port1_command("x", 2.0)
jl.jl_exec("dac_set(TOP_RAIL, 0.0); dac_set(BOTTOM_RAIL, 0.0)", timeout=15)
time.sleep(2.0)
slot, path = jl.active_context()
print("active now:", slot, path)

# 2. restore file bytes / absence (everything except slot3.yaml, which is active).
#    run_file_restore works from the device-side .hilbak the snapshot made and DELETES it on success, so a
#    second run finds nothing to restore from (seen 2026-09-09 16:10). Fall back to the host copies in
#    device_files/ (the capture appended one newline; strip it) whenever the .hilbak route reports False.
import binascii
def host_bytes(dev):
    f = f"{SP}/device_files/" + dev.lstrip("/").replace("/", "__")
    if not os.path.exists(f): return None
    b = open(f, "rb").read()
    return b[:-1] if b.endswith(b"\n") else b
def fnv32(b):
    h = 0x811C9DC5
    for x in b: h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return "0x%08X" % h
def write_from_host(dev):
    b = host_bytes(dev)
    if b is None or fnv32(b) != str(rec["files"][dev]["hash"]).upper().replace("0X", "0x"):
        print("   no host copy for", dev, "(or hash mismatch) - left as is"); return False
    jl.jl_exec(f"import jfs\nf = jfs.open({dev!r}, 'wb'); f.close()", timeout=15)
    for i in range(0, len(b), 1024):
        chunk = binascii.b2a_base64(b[i:i+1024]).decode().strip()
        jl.jl_exec(f"import jfs, binascii\nf = jfs.open({dev!r}, 'ab'); f.write(binascii.a2b_base64({chunk!r})); f.close()", timeout=20)
    return True
files = rec["files"]
later = "/slots/slot3.yaml"
for p, snap in files.items():
    if p == later: continue
    ok = jl.run_file_restore(snap)
    if not ok and snap.get("existed"):
        ok = write_from_host(p); print("   (from host copy)")
    print("restore", p, "existed" if snap["existed"] else "absent", "->", ok)

# 3. delete files the tests may have created
extra = ["/slots/slot1.yaml", "/slots/slot2.yaml", "/slots/slot4.yaml", "/slots/slot5.yaml", "/slots/slot6.yaml", "/slots/slot7.yaml",
         "/slots/slot1.json", "/slots/slot2.json"]
out = jl.jl_exec("import jfs\n" + "\n".join(f"print({p!r}, jfs.exists({p!r}))" for p in extra), timeout=20)
for line in out.splitlines():
    if line.strip().endswith("True"):
        p = line.split()[0]
        if p not in files:
            jl.jl_exec(f"import jfs; jfs.remove({p!r}); print('removed', {p!r})", timeout=10)
            print("removed test-created", p)

# 4. back to slot 0
jl.port1_command("<0", 5.0)
time.sleep(1.5)

# 5. slot3.yaml last (the deferred save of slot 3 lands a few seconds after the switch; write after it)
time.sleep(3.0)
ok = jl.run_file_restore(files[later])
if not ok: ok = write_from_host(later); print("   (from host copy)")
print("restore", later, "->", ok)

# 6. reboot so RAM matches the files
jl.reboot_board()
jl.port1_wait_ready(30.0)
time.sleep(2.0)

# 7. verify
slot, path = jl.active_context()
print("active after reboot:", slot, path)
again = jl.board_state_capture()
if again is None:
    print("FAIL: no Y capture after restore"); sys.exit(1)
diffs = jl.state_fingerprint_diff(jl.state_fingerprint(snap_yaml), jl.state_fingerprint(again))
if diffs:
    print("DIFFERENCES vs snapshot:")
    for d in diffs: print("  -", d)
else:
    print("VERIFIED: live state matches the snapshot (bridges, power, parts)")
open(f"{SP}/restore_capture_{time.strftime('%Y-%m-%d_%H%M')}.yaml", "w").write(again)
import subprocess
print(subprocess.run(["python3", "/Users/kevinsanto/Documents/GitHub/JumperlOS/test/hil/port7.py", ":json:power\n", "2"], capture_output=True, text=True).stdout[-300:])

# ---------------------------------------------------------------- 8. post-reboot file checks (bytes after the reboot, files that should be absent, stray files the run made)
print("\n=== post-reboot file checks ===")

def norm_hash(v):
    """The snapshot stores what jl.py parsed from the device's 'fnv= 0x....' line; accept str/int/float forms."""
    if isinstance(v, (int, float)): return "0x%08X" % int(v)
    t = str(v).strip().upper()
    if not t.startswith("0X"): t = "0X" + t
    return "0x" + t[2:].zfill(8)

want = {p: snap for p, snap in rec["files"].items() if snap.get("existed")}
code = "import jfs\n"
for p in want:
    code += f"""
p = {p!r}
if jfs.exists(p):
    f = jfs.open(p, 'rb'); h = 0x811C9DC5; n = 0
    while True:
        c = f.read(512)
        if not c: break
        n += len(c)
        for x in c: h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    f.close()
    print('HASH', p, n, '0x%08X' % h)
else:
    print('HASH', p, 'MISSING')
"""
out = jl.jl_exec(code, timeout=90)
bad = 0
seen = set()
for line in out.splitlines():
    if not line.startswith("HASH"): continue
    parts = line.split(); p = parts[1]; seen.add(p)
    if parts[2] == "MISSING":
        print("  MISSING", p); bad += 1; continue
    h = norm_hash(parts[3]); exp = norm_hash(want[p]["hash"])
    print("  ", "ok  " if h == exp else "DIFF", p, parts[2], "bytes", h, "expected", exp)
    if h != exp: bad += 1
for p in want:
    if p not in seen: print("  NO ANSWER for", p); bad += 1

# files that were absent at the snapshot must be absent
absent = [p for p, snap in rec["files"].items() if not snap.get("existed")]
out = jl.jl_exec("import jfs\n" + "\n".join(f"print('EXISTS', {p!r}, jfs.exists({p!r}))" for p in absent), timeout=30)
for line in out.splitlines():
    if line.startswith("EXISTS") and line.endswith("True"):
        print("  STILL PRESENT (should be absent):", line.split()[1]); bad += 1

# directory listings vs the snapshot-time listings (2026-09-07 23:0x). Only names the RUN is known to
# have created are removed; anything else that is extra is reported and left in place (Kevin's files
# are never touched on a guess).
expected = {
    "/": {"python_scripts", "images", "config.txt", "projects", "slots", "undo_history.txt", "selftest.json"},
    "/slots": {"slot0.yaml", "last_active.txt", "slot3.yaml"},
    "/python_scripts": {"examples", "lib", "modules"},
    "/python_scripts/lib": {"jumperless.py", "jumperless.pyi", "oledgui.py", "rp2.py"},
    "/projects": {"555", "i2cscrn", "nand00", "eeprom"},
    "/projects/555": {"README.md", "main.py", "wiring.yaml", "555_run.yaml"},
    "/projects/i2cscrn": {"README.md", "main.py", "wiring.yaml"},
    "/projects/nand00": {"README.md", "main.py", "wiring.yaml"},
    "/projects/eeprom": {"README.md", "main.py", "wiring.yaml"},
}
RUN_MADE = {  # exact names the agents created during the run, per directory
    "/": set(),
    "/slots": {"slot1.yaml", "slot2.yaml", "slot4.yaml", "slot5.yaml", "slot6.yaml", "slot7.yaml", "slotPython.yaml"},
    "/python_scripts": {"bench_repl_test.py", "bench_repl_test2.py", "_temp_repl_edit.py", "history.txt"},  # the last two: the mp_repl agent (its report names them)
    "/python_scripts/lib": set(),
    "/projects": {"bench555"},  # a recheck agent's byte-copy of 555 (workflow4_result.json); a directory, its 4 files go first
    "/projects/555": set(),
    "/projects/i2cscrn": {"i2cscrn_run.yaml"},
    "/projects/nand00": {"nand00_run.yaml"},
    "/projects/eeprom": {"eeprom_run.yaml"},
}
ex_count = 24
dirs = list(expected) + ["/python_scripts/examples"]
out = jl.jl_exec("import jfs\n" + "\n".join(f"print('LS', {d!r}, jfs.listdir({d!r}))" for d in dirs)
                 + "\nprint('LS', '/screens', jfs.listdir('/screens') if jfs.exists('/screens') else 'ABSENT')", timeout=40)
remove, left = [], []
for line in out.splitlines():
    if not line.startswith("LS "): continue
    d, lst = line[3:].split(" ", 1)
    try:
        items = set(str(x).rstrip("/") for x in eval(lst)) if lst != "ABSENT" else set()
    except Exception:
        print("  could not parse listing of", d, ":", lst[:80]); bad += 1; continue
    if d == "/python_scripts/examples":
        if len(items) != ex_count: print("  examples count", len(items), "expected", ex_count); bad += 1
        continue
    if d == "/screens":
        if lst != "ABSENT":
            # /screens did not exist at the snapshot; the run's OLED tests are the only writer since
            remove += [f"/screens/{x}" for x in items] + ["/screens"]
        continue
    extra = items - expected[d]; missing = expected[d] - items
    if missing: print("  MISSING in", d, sorted(missing)); bad += 1
    for x in sorted(extra):
        full = f"{d.rstrip('/')}/{x}"
        if x in RUN_MADE[d] or x.endswith(".hilbak"): remove.append(full)
        else: left.append(full)
for f in list(remove):
    if f == "/projects/bench555":
        for x in ("wiring.yaml", "main.py", "README.md", "bench555_run.yaml"):
            jl.jl_exec(f"import jfs\ntry:\n    jfs.remove('/projects/bench555/{x}')\nexcept Exception as e:\n    pass", timeout=15)
for f in remove:
    r = jl.jl_exec(f"import jfs\ntry:\n    jfs.remove({f!r}); print('removed')\nexcept Exception as e:\n    try:\n        jfs.rmdir({f!r}); print('removed dir')\n    except Exception as e2:\n        print('could not remove', e, e2)", timeout=15)
    print("  remove", f, "->", r.strip().splitlines()[-1] if r.strip() else "(no answer)")
for f in left:
    print("  unexpected extra, LEFT IN PLACE (not made by the run as far as the ledger knows):", f)
print("post-reboot checks:", "CLEAN" if bad == 0 and not left else f"{bad} problem(s), {len(left)} unexpected extra", "| removed:", remove)

# ---------------------------------------------------------------- 9. the V5 must enumerate without the UAC2 mic (usb_audio_mic.py turned it on live during the run)
audio = subprocess.run(["sh", "-c", "ioreg -l -w0 2>/dev/null | grep -c 'JL Audio'"], capture_output=True, text=True).stdout.strip()
print("JL Audio In interfaces after reboot:", audio, "(expected 0)")
