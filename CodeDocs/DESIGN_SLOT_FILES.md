# Slots Become Files — the path-based active context (as built, wave 2)

The active context's identity is a **path**, always. This is the layer the
projects launcher (DESIGN_PROJECTS_SUBSYSTEM.md) and the guided build
(DESIGN_GUIDED_PLACEMENT.md) are both built on, and it is the layer that retires
the `slot_555.yaml`-parses-as-slot-0 class of bug for good.

Distilled from the wave-2 design work plus everything the bench forced on it.
Where this document and an older note disagree, this one is the code.

---

## 0. The model

`SlotManager` carries `char activeSlotPath[128]` (a fixed array, not a `String`:
the auto-save path runs from `service()` on every idle flush and must not churn
the heap). Two worlds, one invariant:

- **Numbered** — slots 0–7 and Python's slot 99. The path is the canonical
  `/slots/slotN.yaml`; `activeSlotNumber` / `netSlot` keep their numeric value.
  Nothing numbered changed behaviour.
- **A file** — anything else. `activeSlotNumber` and `netSlot` are **both**
  `SLOT_FILE_CONTEXT` (`= -2`, declared beside `NUM_SLOTS` in `RotaryEncoder.h`)
  and the path is the sole identity.

**The invariant that does the work: `netSlot == activeSlotNumber`, in both
worlds.** That is what makes every stale `saveSlot(netSlot)` call site fail
loudly on an invalid slot −2 instead of silently clobbering a numbered slot.
`saveSlot` carries an explicit loud `saveSlot(-2)` BUG guard for the same reason.

**−2 never leaves the firmware.** The wire value for "a file" is **−1**
everywhere a host can see it (§5).

```cpp
static constexpr int SLOT_FILE_CONTEXT = -2;
const char* getActiveSlotPath() const;
bool isPathContext() const;                                 // number == -2
bool saveActiveSlot(String&, bool skipValidation = false);  // the dispatch, §3
bool writeStateToPath(const char* path, String& err);       // the single write door, §4
static int slotNumberForCanonicalPath(const String&);       // the strict matcher, §1
void updateLastActive();                                    // §2
char temporarySlotOriginalPath[128];                        // §6
char previewOriginalPath[128];                              // §6
```

### The strict matcher

`slotNumberForCanonicalPath()` returns `n` **only** for exactly
`/slots/slot<n>.yaml`, `0 ≤ n < NUM_SLOTS` (plus 99). Everything else is −1 →
file context. The old `extractSlotNumberFromPath()` matched any basename
starting `slot` and ending `.yaml`, so `slot_555.yaml` → `"_555"` →
`String::toInt()` = **0** → the idle auto-save wrote the project's wiring over
`/slots/slot0.yaml`. The strict matcher retires that whole class; the
FileManager's long "don't name project files slot*.yaml" warning went with it.

### Adoption, and the atomicity contract

`loadSlotFromPath(path)` **adopts**: on success it sets `activeSlotPath = path`
and `activeSlotNumber = netSlot = slotNumberForCanonicalPath(path)`, falling
back to `SLOT_FILE_CONTEXT`. That is the heart of the rework — before it, a
Files-browser load left tracking on the *previous* slot and the next dirty
auto-save wrote the loaded file's content into that slot.

**Tracking is adopted only on a successful open AND parse. A `false` return
means "nothing happened to the context."** Three cases, and the third is the one
callers must handle:

1. **Open failure** (missing file, open refused) — nothing happened at all. The
   read is kept strictly *before* `fromYAML`, and that ordering **is** the
   atomicity for this case.
2. **Parse/validate failure** — `fromYAML` has already `clear()`ed and
   repopulated `globalState` by the time it returns false, so leaving trackers
   alone is not enough. **The prior context is re-loaded from its own file**
   (through the public loaders, so it inherits parts expansion, fakeGpio, the
   refresh and the power re-assert; a `restoringContext` member bounds the
   recursion to depth 1). Trackers end where they started, and the
   `SLOT_CHANGED:-1` emission is suppressed while restoring so a failed load of
   X does not announce a spurious change for the context it just put back.
3. **Double failure → the terminal state.** See §7.

> **`fromYAML` CAN return false.** The wave-1 premise that it never does was
> wrong, and it was load-bearing: `fromYAML` ends in `return validate(errorMsg)`,
> and `PowerState::validate` rejects any of topRail/bottomRail/dac0/dac1 outside
> ±8 V; `JumperlessState::validate` also rejects invalid nodes in `bridges`.
> Both are reachable through flows this design explicitly endorses — host-editing
> a run file over MSC, or clicking a hand-written YAML in the Files browser.
> Without case 2, a `topRail: 12.0` in a clicked file left `globalState` replaced
> with the foreign file's remnants while tracking still pointed at slot N, and
> the next idle auto-save overwrote slot N with them. No −2 is involved on that
> path, so the loud `saveSlot(-2)` guard never fires. The dirty flag is clear
> right after the failure, which narrows the window but does not close it:
> `fromYAML`'s own bridge-dropping sanitizer calls `markDirty()`, so a file that
> both drops bridges *and* fails power validation is dirty on exit with no user
> action at all.

---

## 1. What −2 means to each consumer

Test `netSlot == SLOT_FILE_CONTEXT` **exactly**, never `< 0`: `-1` and
`NUM_SLOTS` are live defcon sentinels in `attractMode`.

| Site | Behaviour in a file context |
|---|---|
| `service()` auto-save | `saveActiveSlot(err, true)`; `syncFromGlobalNetSlot()` stays ahead of it (a no-op at −2, load-bearing for the wheel flow). The failure print is path-or-slot aware and **throttled to 10 s** — the dirty flag deliberately survives a failure, so an unsaveable context re-attempts every idle pass |
| `saveActiveSlot` | `>= 0` → `saveSlot`; `-2` + non-empty path → `saveStateToActivePath`; `-1` → "No active slot to save" |
| `deleteSlot` / `setActiveSlot` / `saveSlot` | all keep the path companion in step. `saveSlot` has always MOVED the active context to `slotNum`, so the path moves with it — otherwise an explicit `W 3` from a run file leaves the path on the run file while the number says 3, and the next auto-save writes slot 3's content into the run file |
| `loadfile:` (main.cpp) | branches on `== SLOT_FILE_CONTEXT`; a boot context whose file will not open falls back to slot 0 **and rewrites `last_active.txt`** |
| `Q` (`cmd_queryActiveSlot`) | `ACTIVE_SLOT:-1` + `ACTIVE_PATH:` — see §5 |
| `<` / `>` slot cycling | a file context maps to 0 before the wrap logic |
| `attractMode()` | entering the cycle from a file context starts at the ends (`DOWN → 0`, `UP → NUM_SLOTS-1`). No phantom "file" position in the ring this wave |
| `W` (Wokwi) | `-2` takes the active-context branch naturally; persistence via `saveActiveSlot`, re-apply via `loadSlotFromPath` |
| clear (`x`) | `clearActiveContext()` — **connections only**. `x` must not reset power and drop the user's rails to 0 V |
| `previewSlotColors(slot < 0)` | early-returns in a file context: previewing "current" from a file context is a no-op, and `enterPreviewMode` only takes numbered slots |
| Undo | `g_slotCursor[NUM_SLOTS + 1]`; index `NUM_SLOTS` (8) is the shared **file-context bucket**, mapped **only** from `SLOT_FILE_CONTEXT`. Bucket-8 transactions are session-only: **one** `undoTxnIsPersistable()` predicate is used by both `undoSerialize`'s selection loop and `undoHasPersistableTxns` — skipping in only one gives either persisted file-context txns or a permanent dirty-retry loop. The persist header stays `NUM_SLOTS`-sized, so the file format is unchanged |
| USBfs | pre-mount save via `saveActiveSlot`; **the eject reload now reloads by path** (the old `if (netSlot >= 0)` guard silently discarded every host edit made to a file context over MSC) |
| Ser3 status JSON | `"slot": -1` for a file context, plus an **always-emitted** `"slot_path"` |
| `printNodeFile` | skipped entirely in a file context rather than building `nodeFileSlot-2.txt`. (It has been a silent no-op for every context since the YAML migration — it opens the legacy `nodeFileSlot<n>.txt` — so making it newly print something would be a behaviour change dressed as a port.) |

### Power is re-asserted at one choke point

`applyStateToHardware()` calls `setRailsAndDACs(0)`, which unconditionally pushes
`globalState.power` to hardware; there is **no** `set_dacs_on_boot` /
`set_rails_on_boot` gate on that path. Every load route reaches it — and auditing
them found two that did not:

- `loadSlot(n)`'s **empty-slot branch** only swapped trackers, so switching to an
  empty slot left the *previous* context's rails and DACs energized. **Fixed.**
- `exitTemporarySlot()` did a raw `fromYAML` + `refreshConnections(-1,0,1)`, which
  restores BRIDGES but not rails/DACs — so exiting a calibration app left whatever
  voltages the app had set. **Fixed.**

Both fixes are at the choke point inside `SlotManager`, not per caller.

`applyStateToHardware(bool skipPower = false)` and its completion sibling
`applyStatePowerToHardware()` exist for the guided-launch transient; the latch
that drives them (`slotLoadDeferPowerApply`) is
DESIGN_PROJECTS_SUBSYSTEM.md §1b's, and `loadSlotFromPath` **latches and clears it
on entry** so the parse-failure restore and the terminal path still assert their
own power.

---

## 2. Persistence: `last_active.txt` and boot

Not config.txt — config saves are full-file rewrites with a diff gate, and
writing config on every slot switch would churn flash and the diff cache.

- **`/slots/last_active.txt`** — one line, the active path. Written by
  `updateLastActive()` through the write-back file cache (the same coalesced,
  cheap machinery `undoPersistHistory` uses).
- Called from `loadSlot` (both exits), `loadSlotFromPath`, `setActiveSlot`, and
  `saveSlot`. **Self-gating at that one choke point** rather than per caller: no
  write while `previewModeActive || temporarySlotActive`, never for slot 99 or
  temp 8, never for an empty path, **never for a read-only template** (§4), and
  it skips the write when the path is unchanged.
- Its dedup cache means "what the FILE holds", not "what the context is", and is
  **seeded by `seedBootContext()` from the value it read** — so the first
  `updateLastActive()` of a session does not rewrite the file with what it just
  read, while the fallback paths (which differ from the read value) still do.

```cpp
struct slots {
    int boot_mode = 1;   // 0 = always boot_slot; 1 = the last-active FILE
    int boot_slot = 0;   // used when boot_mode == 0; 0-7
} slots;                 // config.h, section [slots]
```

**Boot flow**, in order:

1. `RotaryEncoder.cpp` — `netSlot = 0`, the pre-config default. Unchanged.
2. `SlotManager` ctor — `activeSlotNumber = 0`, `activeSlotPath = "/slots/slot0.yaml"`.
3. `setup()` finishes, `configLoaded = true`.
4. `loop()`, `firstLoop == 1` → **`seedBootContext()`**:
   - `boot_mode == 0` → `netSlot = clamp(boot_slot, 0, 7)`, `setActiveSlot(n)`.
   - `boot_mode == 1` → read `last_active.txt`.
     - missing / empty → slot 0. **Byte-identical to the old boot**, so an
       upgraded board only changes behaviour after its first slot switch.
     - a canonical slot path → `setActiveSlot(n)`.
     - otherwise, `safeFileExists(path)` → adopt the path (`netSlot =
       activeSlotNumber = -2`, path set, **no load**).
     - file gone → slot 0 **and** rewrite `last_active.txt`, so the failure
       cannot recur every boot.
5. `goto loadfile:` — the single load path, branching on `== SLOT_FILE_CONTEXT`.
   A path that exists but will not open falls back to `loadSlot(0)`.
6. Power is asserted by whichever branch ran.

The `firstLoop` guard still prevents the pre-load dirty-save from firing on boot.

---

## 3. Saving: one dispatch, one door

`saveActiveSlot()` is the dispatch (§1). `writeStateToPath()` is the **single
door every state write passes** — the template refusal lives there rather than
only in `saveStateToActivePath`, because `writeStateToPath` is public and wrote
arbitrary paths with no guard. Provisioning is unaffected: it installs templates
by copying bytes through `safeFileWriteAll`, not through this API.

A refused save returns **before** `clearDirty()`, so nothing is silently lost.

`runSource:` is a top-level scalar on the state, emitted and parsed in the same
commit (the States.h round-trip rule: `toYAML` is a wholesale rewrite, so a field
the parser accepts and the serializer skips is destroyed by the next auto-save).
It names the canonical wiring a run file was copied from — the provenance the
path itself cannot encode, and what variant resolution reads (see
DESIGN_PROJECTS_SUBSYSTEM.md §1b).

---

## 4. The template write-guard

`/projects/<dir>/wiring*.yaml` is **read-only**. Writes are refused, loudly;
loads are untouched.

This is not theoretical. During wave 2, adoption plus the idle auto-save
**destroyed three shipped templates on the bench**: `load_project("eeprom")`
pointed the active context at the shipped `wiring.yaml`, the state went dirty,
and the auto-save wrote `globalState` back over it. Because `toYAML` is a
wholesale rewrite and `guide:` / `meta:` are swallowed on parse and never
re-emitted, the templates came back with those sections **gone**:

```
/projects/i2cscrn/wiring.yaml  guide=0 meta=0 parts=1
/projects/nand00/wiring.yaml   guide=0 meta=0 parts=1
/projects/eeprom/wiring.yaml   guide=0 meta=0 parts=1
/projects/555/wiring.yaml      guide=1 meta=1 parts=1   <- survived
```

`parts:` survived because it round-trips. The 555 survived because the `z` flow
adopted its template only transiently, with no idle pass in between. The three
that died were reached through MicroPython `load_project()`, which lingers.

The guard, precisely:

- `writeStateToPath` refuses any `/projects/<dir>/wiring*.yaml`.
- `updateLastActive()` gates on the same predicate, so a board can never boot
  into a context it cannot save.
- `service()` skips the auto-save **attempt** for a template (the dirty flag
  stays set by design, so an unguarded attempt would print its refusal every idle
  pass forever); a 10 s rate-limited note replaces it. User-initiated saves still
  get the loud refusal.
- The predicate is `SlotManager::isTemplatePath()`, and the launcher's
  Files-browser deferral uses the same one, so the two sides cannot drift.

**Why not simply refuse to adopt templates?** That reintroduces the retired trap
exactly: tracking would stay on the previous slot and the auto-save would write
the template's content into THAT. Adoption plus a write-guard is the shape that
is safe at every step.

Run files (`<dir>_<N>.yaml`) deliberately do not match the predicate. A dirty
*template* context that is switched away from hits the loud refusal and drops its
edits: that is the guard working, and it is disclosed at the site.

---

## 5. What the user and the host see

- **`Q`** → `ACTIVE_SLOT:<n>` (0–7/99) or `ACTIVE_SLOT:-1` (a file), **always**
  followed by `ACTIVE_PATH:<full path>`. Numbered slots print their canonical
  path. There is **no `ACTIVE_CONTEXT:` line**; `-1` keeps every existing integer
  parser alive and the path line is the new truth.
- Entering a file context emits `SLOT_CHANGED:-1` + `ACTIVE_PATH:<path>`.
- Ser3 status JSON: `"slot": -1`, `"slot_path"` always emitted. Numbered flows
  are byte-identical apart from the new field.
- `listSlots` appends `Active file: <path>`; the status print and the USBfs
  status box say `Active File:` / `Current File:`.
- `activeContextLabel7()` (basename minus `.yaml`, truncated to 7; `Slot N` /
  `Python` for numbered contexts) exists per the design but **has no consumer
  yet** — the display sites that would use it all render from the raw path or
  from numbers today. Kept in place for the UI work; flagged here so nobody reads
  its absence from the call graph as a missing row.
- The Slots menu stays numbered-only by design: quick access to 0–7.

---

## 6. Temp slot 8 and preview

Both save/restore dances gained a **path companion**, because
`temporarySlotOriginal` / `originalSlotNumber` can now be −2 and
`loadSlot(-2)` cannot restore anything:

- `enterTemporarySlot` captures `temporarySlotOriginalPath`; `exitTemporarySlot`
  restores the pair and reloads **from the path**, so a calibration app launched
  from a run file returns to that run file. `updateLastActive()` stays gated off
  for the whole temp window; temp 8 never becomes the boot context.
- `enterPreviewMode` / `exitPreview` gained `previewOriginalPath`, and
  `enterPreviewMode` now raises `previewModeActive` **before** its internal
  `loadSlot`. That ordering fix has a **user-visible consequence**: preview
  genuinely does not apply the previewed slot's rails and DACs any more. It
  matches preview's documented contract ("loads into globalState without applying
  to hardware"), and it is what stops every wheel detent through the Slots menu
  from stamping a merely-glanced-at slot into `last_active.txt`.
- `exitPreview(applyPreview=true)` calls `updateLastActive()` **after** the flags
  clear — its internal `saveSlot` runs while the gate is still closed.

The MicroPython isolated session got the same treatment plus an ordering fix:
`jl_exit_micropython_restore_entry_state` used to run `restoreAndSaveStateBackup()`
while tracking still said slot 99, so the restored ENTRY state was written over
`slotPython.yaml`, undoing the python-state save two lines above. Tracking
(number **and** path) is restored first, and the restore takes the save only when
tracking actually restored.

---

## 7. The terminal state (`-1 / -1`)

When a parse failure's restoring re-load **also** fails, the manager enters the
**no-active-context state**: `activeSlotNumber == netSlot == -1`, empty path,
cleared state, one loud print.

**It is deterministic, not exotic.** Whenever the caller passes the *active* path
itself, the restore re-reads the same bad file and the second failure is
guaranteed. `USBfs`'s MSC-eject reload calls
`loadSlotFromPath(mgr.getActiveSlotPath())`, so **a host writing an invalid YAML
over the active run file lands here every time** — an explicitly endorsed flow.
Numbered contexts are rescued by `loadSlot`'s `/.bak` mirror; arbitrary paths have
no mirror, and **a `.bak` mirror for arbitrary paths was deliberately not built**:
the terminal state is the accepted outcome.

Two properties make it a *safe stop* rather than data loss:

1. **`-1/-1`, deliberately not `deleteSlot`'s `-1/0`.** With `netSlot == 0` the
   next `service()` pass would `syncFromGlobalNetSlot()` to 0 and write the
   cleared/foreign state over `/slots/slot0.yaml` — the same clobber class one
   level down. Equal at −1, the sync is a no-op and `saveActiveSlot` refuses.
   `updateLastActive()` self-gates on −1, so `last_active.txt` still names the
   saved context; if that file really is gone, boot's fallback rewrites it.
2. **Nothing routed, nothing powered beyond defaults.** `clear()` alone left the
   crossbar routed at the vanished context's bridges and the rails energized at
   its voltages while RAM read empty. The terminal block now does its own
   `refreshConnections` + `applyStateToHardware` — the common parse-failure path
   inherits those from the restoring loader, but this path had to do it itself.

Recovery: `<0`, a Files click, or a reboot (which comes back through the boot
slot-0 fallback). The HIL needle drives it with `fs_write` of bad YAML over the
active run file followed by `load_project` on the same path, in **one** REPL
snippet — split across two calls, the gap lets the idle auto-save rewrite the
file with valid content and the assertion goes vacuous.

---

## 8. `jl_switch_slot` — a known wart, parked

`jl_switch_slot(n)` returns the **previous slot number** on success and `-1` on
error, and the MicroPython binding raises `ValueError("Invalid slot number")` on
`-1`. From the terminal state the previous slot **is** `-1`, so `switch_slot(3)`
performs the switch correctly **and then raises**.

Bench-caught. Worked around in HIL (recover with the `<3` single-char command
instead); **not fixed in firmware**, because the fix changes a published Python
contract and no wave-2 path calls it.

**The agreed future shape**, when someone owns that API next:

| return | meaning | binding |
|---|---|---|
| `-2` | failure | **raises** |
| `-1` | legal: there was no previous slot | returns `-1` |
| `0..7`, `99` | the previous slot | returns it |

i.e. the binding raises **only on −2**.

Also recorded, not fixed: `jl_switch_slot`'s dirty pre-save **prints and then
proceeds** on any failure (template refusal, failed `validate()`, write error), so
the outgoing edits are discarded with only a serial line. Aborting the switch
instead would strand the caller. The pre-save is a **call-site** responsibility on
purpose and deliberately not pushed inside `loadSlot` — forcing a save inside the
API is exactly what the boot `firstLoop` guard exists to prevent. (`loadSlot`'s
`fileCacheFlushNowAll("slot_switch")` drains dirty **cache entries** and SPIFTL
metadata; it never serializes a dirty in-RAM `JumperlessState`. The wave-1
justification that said otherwise was wrong, and `switch_slot` from a dirty file
context silently discarded the edits until this was fixed.)

---

## 9. Migration and downgrade

- **Old config, new firmware**: no `[slots]` section → defaults → `boot_mode = 1`.
  No `last_active.txt` yet → slot 0 = exactly the old boot. A board pinned to
  slot 0 therefore changes behaviour only after its first slot switch. Pin the old
  behaviour with `boot_mode 0`.
- **New config, old firmware**: configManager's parser skips unknown
  sections/keys (the incremental saver even preserves unknown lines), so `[slots]`
  is inert and `last_active.txt` is an unreferenced file. **No hazard.**
- **Run files on old firmware**: `555_1.yaml` clicked in the old Files browser
  takes the old `loadSlotFromPath`; the old matcher requires a basename starting
  `slot` → returns −1 → tracking untouched → no clobber. The residual (later
  edits auto-saving into the previously-active numbered slot) is precisely what
  this design fixes forward.
- **Downgrade drops fields silently**: old firmware skips `runSource:` and
  `placement:` as unknown keys and the next auto-save rewrites the file without
  them. See DEV_MERGE_HANDOFF.md's wave-2 section for the user-facing wording.
- **In-flight guided builds**: a guide left mid-build in a numbered slot by
  pre-wave-2 firmware loses its resume offer after upgrade (`findGuideProgressSlot`
  is deleted; resume reads run files). The slot's committed bridges and
  `guideProgress:` line remain in the file, just not offered.
- **Undo history file**: unchanged format.
- **Jumperless App / port-1 clients** must tolerate `ACTIVE_SLOT:-1`,
  `SLOT_CHANGED:-1`, and the JSON `"slot": -1` + `"slot_path"` field. Numbered
  workflows are byte-identical.

---

## 10. Test surface

`test/hil/test_slot_files.py` (72 checks) owns this layer:

- adoption and the strict matcher; `saveActiveSlot`'s three-way dispatch;
- the **template write-guard** — a fixture template carrying a `guide:` section
  is loaded, dirtied, waited past the flush gate, and asserted byte-identical
  with `guide:` intact and `last_active.txt` not pointing at it;
- **atomic-on-parse-failure, both restore arms**: the numbered arm asserts
  `slot2.yaml` byte-identical after an idle window *with no mutation* first
  (`fromYAML`'s sanitizer can `markDirty()`, so "the failed load wrote nothing"
  is a real check) and then dirties it; the `SLOT_FILE_CONTEXT` arm takes the
  recursive path and shares nothing with the other but the capture;
- `switch_slot`'s dirty pre-save, in **one** `jl_exec` snippet — split across two
  calls the REPL gap lets the idle auto-save write the edit and the assertion goes
  vacuous, which is exactly how it went vacuous the first time;
- **two real reboots**: `boot_mode 1` comes back up in the run-file context *with
  its wiring live*, `boot_mode 0` comes up on the pinned `boot_slot`. The suite
  restores `boot_mode`/`boot_slot` and the original context in its `finally`, and
  is positioned in `run_all.py` **before** the project/guide suites so it cannot
  land between two suites that share state;
- `test_projects.py` carries the terminal-state needle (§7) and the run-file
  allocator; `test_parts_roundtrip.py` carries the slot-clobber canary.

Bench-only, because the harness cannot drive a host mount: the **MSC round-trip**
(load a run file, mount, host-edit, eject, the edit must be live) — which is also
the trigger for §7's deterministic terminal state.
