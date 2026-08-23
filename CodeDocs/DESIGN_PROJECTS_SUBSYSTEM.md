# Projects Subsystem — Design (2026-08-22, as-built through wave 3)

Companion to the approved plan (projects + guided placement branch). Sibling docs:
DESIGN_GUIDED_PLACEMENT.md, **DESIGN_SLOT_FILES.md** (the path-based active
context this subsystem is now built on), DESIGN_PART_ID_FOLLOWUP.md.

> **WAVE 2 SUPERSEDES THE SLOT FLOW.** Sections 0 and 1 below are the original
> wave-1 design and are kept for their reasoning; the **destination-slot picker,
> the temp-slot borrow and the keep-prompt are all deleted from the firmware**.
> A launch now opens `/projects/<dir>/<dir>_run.yaml` — a **run file** — as the
> persistent active context (wave 3: ONE per project, reused; the numbered
> `<dir>_<N>.yaml` scheme lives behind `JL_PROJECT_RUN_HISTORY`).
> **§1b is the as-built contract**; where it and §1 disagree, §1b wins. The same goes for the `z` grammar (destination slots are
> gone) and for variant selection (a load resolves its variant from the run
> file's own `runSource:`).

## 0. Summary

| Question | Decision |
|---|---|
| Layout | `/projects/<short_name>/` dirs: `wiring.yaml` (+ `wiring.<variant>.yaml`), `main.py` (+ `main.<variant>.py`), `README.md` |
| Envelope | Project wiring = slot YAML v2 superset with `meta:`, `parts:`, `guide:`; same tolerant parser + indent-hardening |
| Variants | One complete standalone YAML per variant; launcher picks by `meta.variant` |
| File naming | `wiring*.yaml`, NOT `slot_*.yaml` — relax the FileManager guard instead (see the `extractSlotNumberFromPath` trap) |
| Provisioning | Parallel `projectFiles[]` table + `initializeProjects(force)` mirroring the examples system; new `scripts/generate_projects.py` → `src/snakes/projectFiles.h` |
| Menu | ~~One line under Apps~~ → **wave 2: a TOP-LEVEL clickwheel row, before Apps**; one `apps[]` row; dynamic picker in the launcher |
| Slot flow (non-guided) | ~~`enterTemporarySlot(8)` → keep-prompt → `saveSlot(n)`~~ → **wave 2: `projectBeginRun` → the run file is the persistent context. No temp slot, no destination picker, no keep-prompt. Wave 3: ONE run file per project, `<dir>_run.yaml`; keeping a run is `slots` > `save to`.** See §1b |
| Python API | `jl_load_slot_path()` + `jumperless.load_project(name_or_path)` — **wave 2 splits the two forms**, see §1b |
| Config keys | **None** for v1 (provisioning is existence/hash-driven like examples). Wave 2 adds `[slots] boot_mode` / `boot_slot` — those belong to DESIGN_SLOT_FILES.md, not here |

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
- menuTree.h: `"Guides",` — a childless TOP-LEVEL row, at the head of the
  "things you run" cluster. 6 glyphs, so it renders whole on the LED half-row
  and needs no `\31`. **Named `Projects` through wave 2**, where 8 chars
  rendered as 7 glyphs (`Project`) and split at the S — that is the whole
  reason for the rename. The directory is still `/projects` and the machine
  grammar (`PROJECTS n=`, `RUNFILE`, `RUNS`, `SCRIPT`, `GUIDE`) is unchanged:
  the split between the human word and the machine word is deliberate.
- Apps.cpp: `{ "Guides", 25, 1, projectsAppLauncher },`. This row and the
  menuTree.h line are ONE unit — `getActionCategory` matches the menu line's
  text and `runApp(-1, name)` name-matches it against `apps[]`, so renaming
  either alone kills the row silently.
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

> **The whole block above is DELETED code as of wave 2.** It is kept because the
> hazard is real and the temp-slot machinery still exists for the calibration
> apps (Apps.cpp, SelfTest.cpp) — but `ProjectsApp.cpp` no longer touches it, by
> name or by call. Read §1b instead.

## 1b. Launcher AS BUILT (wave 2)

### The run file

A launch **opens or creates** the project's run file and leaves it as the
active context when the launcher returns. That is the whole persistence model:
there is no destination to choose and nothing to keep, because the run file
*is* the kept thing, exactly like clicking a YAML in the Files browser.

**ONE FILE PER PROJECT is the default** (wave 3, Kevin's ruling — the numbered
files "make way too many files"):

```
/projects/<dir>/<dir>_run.yaml
```

silently reused on every launch. **To KEEP a run**, use `slots` > `save to`
while it is the active context — that is the documented keep flow, and it is
why the launcher no longer mints a file per launch.

The wave-2 numbered scheme is not deleted; it compiles behind
**`JL_PROJECT_RUN_HISTORY`** (`src/config.h`, `0` by default), where the run
file is `<dir>_<N>.yaml`, `N = max+1` per launch, with the load-latest prompt,
`z … run=<N>` and the pile-up hint. Everything below that is not marked
"numbered mode" holds in both.

- The file is a byte copy of the chosen `wiring*.yaml`, plus a `runSource:`
  scalar naming the wiring it came from (the path itself cannot encode which
  variant produced it).
- **The prompt policy** (single-file mode). Absent → create and go, no prompt.
  Present with a guided build **mid-flight** in it → exactly one prompt,
  `y/click = resume, n = start fresh (overwrites), other = cancel`. Present
  otherwise — finished guide, non-guided, plain state → **silently reopen**.
- **Mid-flight** means the file's `guideProgress` says `step < of`. See
  "Deciding mid-flight" below; it is the reason `of:` exists.
- **Start fresh re-copies the template over the existing file.** Same
  validate-by-load + one-retry + delete-on-double-failure discipline as create:
  the user consented at the prompt and the old bytes were gone at the first
  write, so there is nothing extra to protect.
- Numbered mode only: `N` is **monotonic and never reused** (`max(n)+1`, capped
  at 9999 — deleting `_1` while `_2` exists yields `_3`); ≥20 run files prints a
  hint pointing at Files.
- The firmware **never deletes a run file** — it is user data. Old numbered
  files on a board that gets reflashed to single-file mode are **inert
  leftovers**: nothing scans for them, opens one or removes one. Delete them
  from the Files browser.
- **Namespace refusal**: a project directory whose name starts with `wiring` or
  `slot` is refused. `startsWith`, not equality: `/projects/wiringX/wiringX_run.yaml`
  would otherwise match `isTemplatePath()` and be a run file that can never be
  saved. `listVariantFiles` skips **both** run-file spellings (in both compile
  modes) and `projectScanRunFiles` requires the exact `<dir>_` prefix followed
  by 1–4 **digits**, so the guards cannot drift.
- Shipped `wiring*.yaml` templates stay **read-only**: `writeStateToPath` refuses
  any `/projects/<dir>/wiring*.yaml`, `updateLastActive` gates on the same
  predicate, and the idle auto-save skips the *attempt* (rate-limited note). Run
  files deliberately do not match the predicate. See DESIGN_SLOT_FILES.md §4.

#### Naming disjointness — the proof, both directions

| Pair | Why they cannot collide |
|---|---|
| `<dir>_run.yaml` vs `<dir>_<N>.yaml` | the numbered matcher requires the middle to be 1–4 **digits** with no leading zero; `"run"` is not. A directory literally called `Xrun` is not a counterexample — its single run file is `Xrun_run.yaml`, middle still `"run"` |
| `<dir>_run.yaml` vs the variant glob `wiring*.yaml` | the run file starts with the **directory name**, so it can only start with `wiring` when the directory does — which `projectBeginRun` refuses. `listVariantFiles` skips it anyway |
| `<dir>_run.yaml` vs `isTemplatePath()` | same predicate as the variant glob (basename `startsWith("wiring")`), same argument |
| `<dir>_run.yaml` vs `slotNumberForCanonicalPath` | that matcher is anchored on the literal `/slots/slot` prefix. A run file is an ordinary **path context** and reports `ACTIVE_SLOT:-1` + its path |
| `<dir>_run.yaml` vs provisioning | `initializeProjects` iterates the compiled `projectFiles[]` canonical paths only and the forced-refresh branch writes `wiring_original*.yaml`. Neither namespace can reach the other |

#### Deciding mid-flight without loading the file

The prompt offers **cancel**, and a cancel must leave the previous context
untouched (exit **B** below). So the decision has to be made *before*
`loadSlotFromPath` runs — which rules out reading `guideProgress` out of the
loaded state.

It also rules out recomputing `numSteps` launcher-side: `guideParse` resolves
`part:` names, and synthesizes `auto:` steps, against the **live** parts table,
which at that moment still holds the *previous* context. A pre-load parse would
silently drop every `place` step.

So the guide runtime persists the total alongside the step.
`guidePersistProgress` is the only code that both knows `numSteps` and writes
the file, and the progress line round-trips as

```
guideProgress: {source: "/projects/555/wiring.yaml", step: 3, of: 5}
```

`of:` is emitted **only when non-zero** (the `runSource:` rule — an unemitted
scalar is destroyed by the next idle auto-save, so serializer and parser land
together). A hand-written or pre-`of:` file therefore round-trips byte-identical
and reads as *total unknown*, and unknown means **not mid-flight**: reopen
silently and let the existing resume gates do what they always did.

Both error directions are benign, which is why "unknown → reopen" is safe:
under-detect resumes (nothing is overwritten, which is the entire risk),
over-detect costs one prompt whose default is resume — and `guideRun`'s
`ALREADY_COMPLETE` clamp is still the backstop against a file that lies.

`SlotManager::scanGuideProgressFile()` is the load-free reader. It shares one
flow-map parser (`parseGuideProgressLine`, States.cpp) with `fromYAML`'s parse
arm, so the "ONE shape only" contract cannot drift between them.

### The exit table (A–H) — the behavioural contract

"Previous context" means the number-and-path pair `Q` reports.

| Exit | Where | Live state after | On disk | Launcher does |
|---|---|---|---|---|
| **A** | hold / serial byte at the project picker | previous context | untouched | `  Cancelled.`, return |
| **B** | cancel or 20 s timeout at the run-file prompt (single-file: the mid-flight resume/fresh prompt; numbered: load-latest/start-new) | previous context | untouched | `  Cancelled.`, return |
| **C** | cancel at the variant picker | previous context | untouched | `  Cancelled.`, return |
| **D** | run-file create/copy fails | previous context | partial destination **deleted**; copy+load retried once. In single-file mode the "destination" may be an **existing** `<dir>_run.yaml` that start-fresh was overwriting — its old bytes were gone at the first copy, so what is deleted is the half-written replacement | `notify()` + return |
| **E** | run-file load fails (both tries) | previous context — **or** the terminal no-active-context state | the file we created **or overwrote** is deleted (see D; the rule is "created or overwrote *in this session*", never a file we merely found) | `notify()`, hint `(start a new run to rebuild from the project wiring)`, return |
| **F** | guide quit (hold/`q`) mid-build | run file active; rails per the rails rule below | run file holds `guideProgress` at the quit step | **nothing** — no script, no offer, and no `SCRIPT` lines at all |
| **G** | script ends / KeyboardInterrupt | run file active, the script's mutations live | run file saved | `waitForButtonRest(2000)` → `saveActiveSlot` → `  Run saved to <basename> (now your active circuit).` — mode-specific: `<dir>_run.yaml` by default, `<dir>_<N>.yaml` under `JL_PROJECT_RUN_HISTORY` |
| **H** | guide completes | run file active, powered per its own `power_on` | saved at every commit, then again by `finishRun` | the script **offer**: `SCRIPT offer=<path>`, prompt, `SCRIPT action=run\|skip` |

Two things the table cannot hold:

- **A–C are vacuously safe** — nothing is touched until every prompt is answered.
  D and E *have* flushed the **outgoing** context to its own file before the
  copy/load (the standard big-event flush, no worse than the idle auto-save that
  would have run anyway); "untouched" above means the *previous context pair*,
  not "nothing was written anywhere".
- The old "every cancel restores your original connections" guarantee is
  **deliberately dropped for F–H**. Launching a project now *means* switching
  your persistent context, the same as clicking a YAML in Files. The previous
  context is one Slots→Load away, untouched on disk.

### Variant resolution — `runSource:`, not the path

| Situation | Variant comes from |
|---|---|
| start-new, >1 genuine `wiring*.yaml` | the variant picker (`VARIANTS n=`) |
| start-new, one wiring | that wiring |
| **reopen** (silent reuse, mid-flight resume, or numbered load-latest / `run=<N>`) | the run file's **`runSource:`** — the picker is not offered, because a run file is not partitioned by variant and reopening must not contradict a variant choice |
| Files-browser click on a specific `wiring*.yaml`, run file **absent** or the user chose *start fresh* | that file pins the variant; the picker is skipped |
| Files-browser click on a specific `wiring*.yaml`, run file **reopened** | `runSource:` wins and the clicked file is **not** used. The launcher says so: `  (variant taken from runSource; the clicked file was not used - start fresh to change variant)`. This is the one place the door the user came through has less say than the file they land in |
| `runSource:` empty or dangling | the project's default `wiring.yaml`, with one printed line. That wiring is then also the **guide** source; the variant script collapses to `main.py` as a consequence |

Script resolution: `main.<variant>.py` → `meta.script` → `main.py`.

### Guided-ness, and which power lands

`runOpenedRunFile` decides guided-ness **before** any power reaches the rails:

```
guideProgress present                      -> RESUME the guide at the saved step
else runSource names a wiring with
     parts:/guide:                         -> a FRESH guide on this run file
else                                       -> the script
```

The middle arm is why the gate reads `runSource` and not just `guideSource`: a
run quit **before its first commit** never writes `guideProgress`, and the strict
gate would have run `main.py` against a circuit nobody built.

The two launcher entries pass `deferPower=true`, so `loadSlotFromPath` skips its
`setRailsAndDACs(0)` and the caller completes it — guided launches never
energize the project's declared rails on their way to the guide's 0 V, and
non-guided launches get the same power apply a few milliseconds later.
`load_project("<name>")` deliberately does **not** defer: it never runs a guide.

### The `z` grammar

```
z <project>[ new|load][ noscript]              single-file mode (default)
z <project>[ new|load|run=<N>][ noscript]      JL_PROJECT_RUN_HISTORY
```

- `<project>` is a directory name (`555`) or a wiring path
  (`/projects/555/wiring.alt.yaml`, which selects that variant for a NEW run).
- **No mode arg** = reuse the run file when it exists, else new — the
  launcher's own defaults *without* the interactive prompt, because headless
  has to be deterministic. A **mid-flight guided build therefore RESUMES**
  here; only the interactive door asks.
- `new` **overwrites** `<dir>_run.yaml` from the wiring, mid-flight or not.
  Explicit is explicit, and headless never prompts.
- `load` errors when there is no run file (`PROJECT error no run files for
  <dir>`, after a truthful `RUNS n=0`).
- **`run=<N>` is refused by NAME on a single-file build**:
  `PROJECT error run=<N> needs a JL_PROJECT_RUN_HISTORY build - this build
  keeps ONE run file per project (<dir>_run.yaml)`. Quietly opening
  `<dir>_run.yaml` instead would hide a stale assumption in whatever is
  driving. The HIL uses this line as its compile-mode probe.
- **Loud-fail on the old grammar.** A bare all-digit token *after* the project is
  the deleted destination slot and is a usage error. The parse is token-wise, so
  `z 555` is still a perfectly good all-digit **project** name.
- A path form combined with an **explicit** `load`/`run=<N>` is refused: the run
  file's `runSource` decides the variant either way, so the argument would be
  silently ignored. The **bare** path form reaching LOAD (runs already exist)
  prints `(variant taken from runSource; the path argument was not used)`.
- `noscript` suppresses the script on **every** headless path, guided or not.
- `z band <value> [type] [tol]` and `z shunt [n]` are exact-token subcommands
  (not prefixes — a prefix match would make a project named `band*` unlaunchable)
  and belong to the measurement machine, DESIGN_GUIDED_PLACEMENT.md §5.

### The two Python doors

`load_project` splits **by form**, and the split is what the two forms mean:

- `load_project("555")` → `projectOpenLatestOrNew` → open `555_run.yaml`, or
  create it from the wiring (numbered mode: open `555_<maxN>.yaml` / create
  `555_1.yaml`). **Load only** — no guide, no script, and never a prompt: a
  mid-flight guided build is simply reopened, because resuming it is the
  launcher's job.
- `load_project("/any/path.yaml")` → unchanged raw `loadSlotFromPath`.

The name form is precisely what destroyed three shipped templates during wave 2
(`load_project("eeprom")` adopted the template, the state went dirty, and the
idle auto-save rewrote it without its `guide:`/`meta:`). Routing it through
`projectBeginRun` closes that door while leaving the literal-path door — and the
write-guard that protects it — reachable and tested.

### Machine-line inventory, as built

| Line | Fate |
|---|---|
| `PROJECTS n=<count>` | unchanged |
| `VARIANTS n=<count>` | unchanged; dormant (nothing bundled ships two wirings) |
| `GSLOTS n=8` | **dead** — the destination picker is deleted |
| `SLOTS` (keep-flow picker) | **dead** — the keep-flow is deleted |
| `GUIDE resume slot=<n> step=<k>` | **dead** → `GUIDE resume file=<path> step=<k>` |
| `RUNS n=<count> latest=<path>` | **new** — printed whenever a run file already exists, before the prompt (if any) and from every headless `load`. Single-file mode: always `n=1` when the file is there, and **no RUNS line at all** when it is not, since there are no runs to count |
| `RUNFILE path=<path> action=new\|load` | **new** — after the run-file decision, before guide/script; emitted on **every** door (launcher, `z`, FileManager click, `load_project("<name>")`) |
| `GUIDE already complete (step k/n)` | **new** — the completion clamp refusing to relaunch a finished build |
| `SCRIPT offer=<path>\|none` / `SCRIPT action=run\|skip` | **new** — on every path that reaches the script step. **Not** exit F (a guide quit returns first), and **suppressed entirely** when a guide SESSION ran and committed zero steps, where `  (nothing was built - no script offer)` prints instead |
| `PROJECT error <reason>` | **new** — headless failures (`z`, `load_project`). Includes the single-file build's `run=<N> needs a JL_PROJECT_RUN_HISTORY build …` mode refusal |
| `  (variant taken from runSource; the clicked file was not used - start fresh to change variant)` | **new (wave 3)** — a Files-browser click on a `wiring*.yaml` that ends in a silent reopen |
| `ACTIVE_CONTEXT: <path>` | **never existed.** The status surface is `ACTIVE_SLOT:<n\|-1>` + `ACTIVE_PATH:<path>` (DESIGN_SLOT_FILES.md §5) |

### Companion script contract

- `main.py`; per-variant `main.<variant>.py`.
- Guarantees: wiring loaded and routed; ~~temp slot 8 active~~ **the run file is
  the active context** (wave 2); global `_jl_project = {"dir","variant","wiring"}`
  injected; OLED optional.
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

- Bench: **CodeDocs/PROJECTS_BENCH_CHECKLIST.md** is the ordered session script
  (provisioning self-heal, the firmware-update refresh and its `*_original*`
  preservation, the whole clickwheel flow with a cancel at every stage, the
  run-file lifecycle across a power cycle, OLED-absent, Files-browser clicks).
- HIL `test/hil/test_projects.py`: provisioning existence + the generated
  header's currency against `scripts/projects/*`; `load_project` both forms;
  the **slot-clobber regression** (active slot + `/slots/slot0.yaml` untouched);
  the run-file allocator, the `z` grammar's loud-fail, the non-destructive
  restart, and the terminal-state needle; headless `compile()` of every main.py.
  `test/hil/test_slot_files.py` owns the path-context contract underneath it.

## 4. Risks

- Parser indent-hardening is a documented, safe behavior change.
- `overlays:` second-pass scan → reserved-word rule is mandatory.
- Net-name attachment under bridges-primary reconciliation is the one unproven format
  assumption — verify on hardware early (fallback: `set_net_name()` loop in main.py).
- Temp-slot bookkeeping: the exact save-order sequence; `temporarySlotActive` latch.
- Budgets: 122/150 menu lines, 26/30 apps — fine.
- Generator drift fix must land before anyone runs either generator.
