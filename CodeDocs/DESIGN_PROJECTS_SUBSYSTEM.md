# Projects Subsystem — Design (2026-08-20)

Companion to the approved plan (projects + guided placement branch). Sibling docs:
DESIGN_GUIDED_PLACEMENT.md, DESIGN_PART_ID_FOLLOWUP.md. Where this doc and the plan
disagree, the plan wins (notably: `parts:` uses the guided design's pin syntax with
flat `type:`/`value:`/`part_id:` hooks; `connect:` is node-only; guided builds go
into a real slot chosen up front; starter catalog is 4 projects).

## 0. Summary

| Question | Decision |
|---|---|
| Layout | `/projects/<short_name>/` dirs: `wiring.yaml` (+ `wiring.<variant>.yaml`), `main.py` (+ `main.<variant>.py`), `README.md` |
| Envelope | Project wiring = slot YAML v2 superset with `meta:`, `parts:`, `guide:`; same tolerant parser + indent-hardening |
| Variants | One complete standalone YAML per variant; launcher picks by `meta.variant` |
| File naming | `wiring*.yaml`, NOT `slot_*.yaml` — relax the FileManager guard instead (see the `extractSlotNumberFromPath` trap) |
| Provisioning | Parallel `projectFiles[]` table + `initializeProjects(force)` mirroring the examples system; new `scripts/generate_projects.py` → `src/snakes/projectFiles.h` |
| Menu | One line under Apps, one `apps[]` row, dynamic picker in the launcher |
| Slot flow (non-guided) | `enterTemporarySlot(8)` → `loadSlotFromPath` → run script → keep-prompt → `saveSlot(n)` + `exitTemporarySlot(false)` + `loadSlot(n)` |
| Python API | `jl_load_slot_path()` + `jumperless.load_project(name_or_path)` |
| Config keys | **None** for v1 (provisioning is existence/hash-driven like examples) |

## 1. Decisions with justification

### Layout and display names

```
/projects/
  555/
    wiring.yaml        # default variant (complete, standalone)
    main.py
    README.md          # optional; opens in eKilo from the file manager
  i2cscrn/
    wiring.yaml        # variant "direct" (Jumperless drives display)
    wiring.nano.yaml   # variant "nano"
    main.py
    main.nano.py       # optional per-variant script override
```

- Directory per project: groups variants + script + README; MSC users see one folder
  per project.
- **Dir names ≤7 chars**: `FileManager::updateOLEDStatus()`
  (FilesystemStuff.cpp:446-467) renders the click-menu view on the LED matrix as two
  7-char rows (dir name / filename). Full human name lives in `meta.title` (OLED and
  terminal show full strings).
- **File-naming (load-bearing):** do NOT name project wiring `slot_*.yaml`.
  `extractSlotNumberFromPath()` (States.cpp:2798) matches any basename starting `slot`
  and ending `.yaml`: `"slot_555.yaml"` → `"_555"` → `String::toInt()` = **0** →
  `activeSlotNumber = 0; netSlot = 0`, and the SlotManager idle auto-save writes the
  project wiring over the user's `/slots/slot0.yaml`. Instead relax
  `FileManager::selectCurrentFile()` (FilesystemStuff.cpp:1277):

```cpp
} else if ( lower.endsWith( ".yaml" ) &&
            ( lower.startsWith( "slot" ) || fullPath.startsWith( "/projects/" ) ) ) {
```

  With `wiring.yaml`, `extractSlotNumberFromPath` returns −1 and slot tracking is
  untouched (verified: States.cpp:2982 only assigns when `slotNum >= 0`).

### Envelope + parser changes

A project wiring file **is a slot YAML** (loadable by `loadSlotFromPath` with zero
special-casing) plus `meta:` (launcher-only lightweight text scan), `parts:`, `guide:`.

**Required change (States.cpp `fromYAML` ~:1300):** the section-header checks run on
**trimmed** lines regardless of nesting, so a nested key like `config:` or a guide
step containing `power:` would hijack the section state. Fix:

1. Add `meta:`/`parts:`/`guide:` to the header chain (content contained).
2. Indent-harden: capture `bool indented` **before** trim; only recognize top-level
   section headers when `!indented`. Accepted behavior change: a hand-edited slot file
   with an indented `bridges:` header stops parsing that section — the serializer
   never indents headers, so real files are unaffected.
3. Format rule regardless: nested lines must not begin with a reserved word —
   `deserializeOverlaysFromYAML` (called after the loop, States.cpp:~1384) does a
   second full-text scan, so `overlays:` in particular must never appear inside
   project sections.

**Variants: one complete standalone file per variant.** The parser is a single-pass
line machine; conditional sections would break "any project YAML loads directly".
Kevin's "Nano drives it vs Jumperless drives it" differ in wiring AND script —
covered by `main.<variant>.py`.

### Provisioning + generator + flash budget

Mirror the examples system exactly:

- `projectFiles[]` (same shape as `ExampleInfo`, FilesystemStuff.cpp:3348) +
  `initializeProjects(bool force)` following `initializeMicroPythonExamples` (:3464):
  fast existence check unforced; on force, 3-way FNV-1a (current hash / older hashes /
  user-edited → preserve as `*_original*`).
- Call sites: main.cpp:344 and configManager.cpp:~2534 (firmware-update refresh).
- `scripts/generate_projects.py`: walks `scripts/projects/<dir>/` (any text file),
  reuses `py_to_c_string`/`fnv1a32` from `generate_micropython_examples.py`, emits
  `src/snakes/projectFiles.h` (`PROJECT_<DIR>_<FILE>` constants + `*_HASHES` + table
  fragment); history in `scripts/projects_hash_history.json`.
- **Prerequisite drift fix:** `generate_micropython_examples.py` `main()` writes
  `project_root/src/micropythonExamples.h` but the live header is
  `src/snakes/micropythonExamples.h` — running it today creates a duplicate header on
  the include path (every src subdir is on `-I`). Fix output path + the printed
  instructions (`pythonStuff/ex/` → `scripts/ex/`) + README_GENERATE_EXAMPLES.md.

**Flash:** uf2 3,546,112 B → ~1.77 MB actual; program region 12 MB (16 MB −
`board_build.filesystem_size = 4m`) → ~10 MB headroom. 4-8 projects ≈ 60-150 KB
rodata. Non-issue.

### Menu + launcher

- **Dynamic, not static submenu:** APPSACTION (Menus.cpp:4456) resolves the app name
  from `previousMenuPositions[1]` — a static `--555` leaf at depth 2 resolves to its
  depth-1 parent. And no new top-level category: `categoryRanges[10][2]`
  (Menus.cpp:199) is already over-subscribed at 12 top-levels.
- menuTree.h (after :98): `"-Project\31s",` — `\31` breaks the matrix render at
  7 chars; `normalizeSpaces` strips it so the label matches app name `Projects`.
- Apps.cpp:75: `{ "Projects", 25, 1, projectsAppLauncher },`.
- **Launcher** — new `src/ProjectsApp.h/.cpp`, modeled on the `imagesApp(true)`
  selection loop (ImagesApp.cpp:264-400: `jOS.serviceInner()`, encoder delta,
  `encoderButtonState` RELEASED=select / HELD=cancel) and `runPythonScriptFromPath`
  (Apps.cpp:198):

```cpp
struct ProjectMeta { String dir; String title; String summary; String variant; String script; };
void projectsAppLauncher(void);
bool readProjectMeta(const String& yamlPath, ProjectMeta& out);   // meta: text scan
int  listProjects(ProjectMeta* out, int maxOut);                  // /projects/*/wiring.yaml
bool runProject(const String& dir, const String& wiringPath, const String& scriptPath);
bool runGuidedProject(const String& dir, const String& wiringPath); // guided handoff seam
```

```
projectsAppLauncher():
    initializeProjects()                        // self-heal
    projects = listProjects()
    LOOP picker: LED matrix dir name, OLED title+summary
        HELD -> return                          // nothing touched: no slot state entered
        click -> selected
    variants = glob(dir + "/wiring*.yaml"); if >1: second picker
    if wiring has parts:/guide: and runGuidedProject(dir, wiring): return   // guide owns slot flow
    enterTemporarySlot(8)                       // ONLY after selection
    loadSlotFromPath(wiring, err)               // activeSlot stays 8
    script = main.<variant>.py if exists else meta.script or main.py
    content = "_jl_project = {...}\n" + file contents
    Serial screen-clear; encoderButtonState reset; delay(400);   // Apps.cpp:198 dance
    executePythonFileContent(content)           // clickwheel-hold => KeyboardInterrupt
exitPrompt:
    "Keep this circuit?" -> yes: slot picker 0-7, then EXACTLY:
        saveSlot(n); exitTemporarySlot(false); loadSlot(n); refreshConnections(-1);
    no/timeout: exitTemporarySlot();            // restores original slot + hardware
```

**Save-order hazard (do not "simplify"):** `saveSlot(n)` sets `activeSlotNumber = n`
while `temporarySlotActive` is still true (States.cpp:3035-3040);
`exitTemporarySlot(false)` restores tracking without touching hardware (:3147);
`loadSlot(n)` makes the kept project live. Skipping `exitTemporarySlot` leaves
`temporarySlotActive` latched, which blocks every future `enterTemporarySlot` (:3122
refuses to nest). Cancelling before selection must return without entering temp-slot
mode.

### Companion script contract

- `main.py`; per-variant `main.<variant>.py`.
- Guarantees: wiring loaded and routed; temp slot 8 active (non-guided path); global
  `_jl_project = {"dir","variant","wiring"}` injected; OLED optional.
- Must not: `switch_slot()`, `nodes_clear()`.
- **Serial input works**: `input()`/`sys.stdin` reads the global stream
  (`mp_hal_stdin_rx_chr`, Python_Proper.cpp:112-126) which the launcher points at
  `&Serial` — "type in the terminal, it appears on the display" is a bare `input()`
  loop (`scripts/ex/interaction_demo.py` is precedent). Also `probe_wait()`,
  `check_button()`, `clickwheel_*`.
- Termination: clickwheel hold raises KeyboardInterrupt (Python_Proper.cpp:5016);
  scripts wrap loops in try/except for cleanup; launcher resumes at the keep-prompt.

### Python API

- `int jl_load_slot_path(const char* path)` wrapping `loadSlotFromPath` — model on the
  FileManager call path (FilesystemStuff.cpp:1279, same core-0 app context). The
  `holdCore1Frames` dance in `jl_switch_slot` (JumperlessMicroPythonAPI.cpp:1698) is
  NOT needed (it exists only to flip `netSlot`).
- `load_project(name_or_path)`: arg containing `/` = literal path; else
  `/projects/<arg>/wiring.yaml`. Register in both module tables (~modjumperless.c:6520)
  + help text (~:4639). Update `scripts/jumperless.pyi`.

## 2. v1 catalog (starter 4 per Kevin; rest are fast follow-ups)

| dir | Project | Wiring sketch | Parts needed | Variants |
|---|---|---|---|---|
| `555` | 555 LED flasher | reference in the plan | 555, 2R, C, LED, R | default |
| `i2cscrn` | Type-to-screen | SSD1306 on breadboard, GPIO I2C direct; `input()` loop echoes typed lines | display only (**bare**) | direct (nano = follow-up) |
| `nand00` | Logic gates 101 | 74HC00: GPIO_1/2 drive inputs, output → LED row + GPIO_3 readback; interactive truth table | 74HC00, LED+R | — |
| `eeprom` | EEPROM dumper | 24Cxx: GPIO I2C + rails; hex-dump to terminal, optional write test | 8-pin 24Cxx | — |

Follow-ups: rcfiltr (wavegen sweep + response plot), shift595, count161
(74HC161/CD4017), nanoblk (Nano hello + INA current), opampled, audiofx.

## 3. Verification

- Bench: provisioning self-heal (delete /projects, reboot); firmware-update refresh
  preserves user edits as `*_original*`; full clickwheel flow incl. cancel at every
  stage; keep-flow survives power cycle; OLED-absent run; `wiring.yaml` from the Files
  browser loads (not eKilo).
- HIL `test/hil/test_projects.py`: provisioning existence; `load_project("555")` +
  net asserts; **slot-clobber regression** (active slot + `/slots/slot0.yaml`
  untouched); headless `compile()` of every main.py; optional SWD-encoder menu drive.

## 4. Risks

- Parser indent-hardening is a documented, safe behavior change.
- `overlays:` second-pass scan → reserved-word rule is mandatory.
- Net-name attachment under bridges-primary reconciliation is the one unproven format
  assumption — verify on hardware early (fallback: `set_net_name()` loop in main.py).
- Temp-slot bookkeeping: the exact save-order sequence; `temporarySlotActive` latch.
- Budgets: 122/150 menu lines, 26/30 apps — fine.
- Generator drift fix must land before anyone runs either generator.
