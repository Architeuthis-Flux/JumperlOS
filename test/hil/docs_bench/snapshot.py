"""Durable bench snapshot before the docs verification run (2026-09-07).
Host copies: test/hil/kevin_state_<ts>.yaml (untracked) + scratchpad.
Device copies: <file>.hilbak for every state file, using the HIL stale-bak rule
(a surviving .hilbak that DIFFERS stops the run; identical one is dropped)."""
import sys, os, json, time
sys.path.insert(0, "/Users/kevinsanto/Documents/GitHub/JumperlOS/test/hil")
import jl
TS = time.strftime("%Y-%m-%d_%H%M")
SP = os.path.dirname(os.path.abspath(__file__))
HIL = "/Users/kevinsanto/Documents/GitHub/JumperlOS/test/hil"
rec = {"ts": TS}

# 1. live state (bridges + power + parts) as pastable YAML
y = jl.board_state_capture()
if not y:
    sys.exit("FAIL: no Y snapshot from the board; refusing to continue")
for p in (f"{HIL}/kevin_state_{TS}.yaml", f"{SP}/kevin_state_{TS}.yaml"):
    open(p, "w").write(y)
rec["state_yaml"] = f"{HIL}/kevin_state_{TS}.yaml"
rec["fingerprint"] = sorted(str(x) for x in jl.state_fingerprint(y)) if not isinstance(jl.state_fingerprint(y), dict) else {k: str(v) for k, v in jl.state_fingerprint(y).items()}

# 2. active context
slot, path = jl.active_context()
rec["active"] = {"slot": slot, "path": path}
print("active context:", slot, path)

# 3. device-side .hilbak of every state file (generalized run_file_capture)
files = ["/slots/slot0.yaml", "/slots/slot3.yaml", "/slots/last_active.txt",
         "/config.txt", "/undo_history.txt", "/undo.hist", "/selftest.json"]
rec["files"] = {}
for f in files:
    snap = jl.run_file_capture(f)   # same stale-bak discipline; sys.exits on a differing bak
    rec["files"][f] = snap
    print("bak", f, snap)
for proj in ("555", "i2cscrn", "nand00", "eeprom"):
    rp = jl.project_run_path(f"/projects/{proj}")
    snap = jl.run_file_capture(rp)
    rec["files"][rp] = snap
    print("bak", rp, snap)

# 4. host text copies of the small text files (belt and braces)
os.makedirs(f"{SP}/device_files", exist_ok=True)
for f in ["/slots/slot0.yaml", "/slots/slot3.yaml", "/slots/last_active.txt", "/config.txt"]:
    if rec["files"][f]["existed"]:
        t = jl.device_text(f)
        open(f"{SP}/device_files/{f.strip('/').replace('/', '__')}", "w").write(t)
json.dump(rec, open(f"{SP}/snapshot_{TS}.json", "w"), indent=1)
json.dump(rec, open(f"{SP}/snapshot_latest.json", "w"), indent=1)
print("SNAPSHOT OK", TS)
