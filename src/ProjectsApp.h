// SPDX-License-Identifier: MIT
#ifndef PROJECTS_APP_H
#define PROJECTS_APP_H

#include <Arduino.h>

// Projects launcher (design: CodeDocs/DESIGN_PROJECTS_SUBSYSTEM.md §1
// "Menu + launcher", reworked by
// .superpowers/sdd/projects-wave-2-bench-notes/design-launcher.md). A project
// lives in /projects/<short_name>/ as wiring.yaml (+ wiring.<variant>.yaml),
// main.py (+ main.<variant>.py) and an optional README.md. A project wiring
// file IS a slot YAML plus the launcher-only `meta:` block and the
// guided-placement `parts:`/`guide:` sections (States.h's header comment
// documents what the slot parser does with them: `meta:`/`guide:` are
// swallowed and never round-tripped, which is why the launcher text-scans
// `meta:` itself).
//
// RUN FILES (design-launcher §1). Launching a project no longer borrows a
// temporary slot and no longer asks "keep? which slot?". Every launch opens
// (or creates) a PER-RUN state file
//
//     /projects/<dir>/<dir>_<N>.yaml         N decimal, 1..9999, no padding
//
// and makes it the persistent active context (SlotManager::loadSlotFromPath
// adopts it), exactly like clicking a YAML in the Files browser. The run file
// is a verbatim byte copy of the chosen wiring; the first save strips
// `meta:`/`guide:` (never round-tripped) and keeps parts:/bridges:/power:/
// guideProgress:/runSource:.
//
// NAMESPACE INVARIANT (design-launcher §3 - "belt and braces on both sides"):
//   - variant files are  wiring*.yaml      (minus *_original*)
//   - run files are      <dir>_<digits>.yaml
//   - the two sets are disjoint UNLESS the project directory itself is named
//     `wiring*` (then `wiring_1.yaml` would match both), or `slot*` (which
//     would collide with the canonical slot namespace at a glance).
//     Guards, both sides: listVariantFiles() skips any name matching THIS
//     directory's run-file pattern, scanRunFiles() requires the exact
//     `<dir>_` prefix, and projectBeginRun() REFUSES a project directory
//     whose name startsWith("wiring") or startsWith("slot") - prefix, not
//     equality, because SlotManager::isTemplatePath() would false-positive on
//     /projects/wiringX/wiringX_1.yaml and the template write-guard and the
//     run files must stay disjoint.
//
// Provisioning can neither overwrite nor back up a run file: initializeProjects
// iterates the compiled projectFiles[] canonical paths only, and the
// forced-refresh branch writes wiring_original*.yaml backups. No code change
// is needed for that - it is an invariant of the two namespaces above.

// Max projects the picker lists, and max variants per project. Both bound
// stack/static arrays in the launcher - not a filesystem limit.
#define PROJECTS_MAX 12
#define PROJECT_VARIANTS_MAX 6

// Root of the project tree on the filesystem.
#define PROJECTS_DIR "/projects"

// Highest run number a project directory can hold (4 digits, design §1.1).
// Numbering is MONOTONIC: gaps left by deleted runs are never reused, because
// reusing one resurrects a stale terminal/HIL transcript's idea of which file
// is which.
#define PROJECT_RUN_MAX_N 9999

// How many run files in one project before the launcher prints the one-line
// pile-up hint (design §1.1 - user data, never auto-deleted).
#define PROJECT_RUN_PILEUP_HINT 20

struct ProjectMeta {
    String dir;      // directory name, e.g. "555" (<=7 chars by convention)
    String title;    // meta.title, or the dir name when there is no meta:
    String summary;  // meta.summary ("" when absent)
    String variant;  // meta.variant ("" when absent)
    String script;   // meta.script ("" when absent -> main.py)
};

// Lightweight text scan of the `meta:` block only (title/summary/variant/
// script; a quoted value takes the quote pair). Tolerant: a wiring file with
// no meta: still returns true with title = dir name. Returns false only when
// the file cannot be opened.
bool readProjectMeta(const String& yamlPath, ProjectMeta& out);

// Scan /projects/*/wiring.yaml. Returns how many entries were written to out[]
// (alphabetical by dir name).
int listProjects(ProjectMeta* out, int maxOut);

// ---------------------------------------------------------------------------
// Run files
// ---------------------------------------------------------------------------

// Allocate /projects/<dir>/<dir>_<max+1>.yaml, byte-copy `templatePath` into
// it, and open it as the active context (design §1.2). On a load failure the
// just-created file is deleted and the copy+load is retried ONCE; a second
// failure leaves the previous context untouched (loadSlotFromPath is atomic on
// failure) and returns false with `err` set. Stamps `runSource: <templatePath>`
// and saves on success.
//
// Refuses a project directory named wiring*/slot* (see the namespace invariant
// above) and a project whose run counter has reached PROJECT_RUN_MAX_N.
bool projectBeginRun(const String& dir, const String& templatePath,
                     String& runPathOut, String& err);

// Highest existing run number for `dir` inside `projectPath`, 0 when none.
// countOut receives how many run files exist (the pile-up hint's input).
int projectScanRunFiles(const String& projectPath, const String& dir, int& countOut);

// What `z ... new|load|run=<N>` selects. DEFAULT = load latest when runs
// exist, else new - the launcher's own defaults WITHOUT the interactive
// prompt (headless has to be deterministic).
enum class ProjectRunMode : uint8_t { DEFAULT, NEW, LOAD, RUN_N };

// Headless/HIL entry (the `z` single-char command). Serves guided AND
// non-guided projects; `project` is a project directory name ("555") or a
// wiring path ("/projects/555/wiring.alt.yaml" - selects that variant for a
// new run). noScript skips the companion script deterministically.
// Returns false when nothing could be launched (the reason is printed).
bool runProjectHeadless(const String& project, ProjectRunMode mode, int runN,
                        bool noScript);

// MicroPython `load_project("<name>")`: open the project's latest run file, or
// create <dir>_1.yaml when it has none. LOAD ONLY - no guide, no script. The
// literal-path form of load_project deliberately does NOT come through here
// (it stays a raw loadSlotFromPath adopt, which is what makes the shipped
// template read-only-context guard in SlotManager still load-bearing).
bool projectOpenLatestOrNew(const String& project, String& runPathOut);

// The Files-browser seam: a click on /projects/<dir>/wiring*.yaml routes here
// instead of adopting the template. Runs the full interactive flow for that
// project with the clicked file as the chosen variant (design-slots §3).
void projectRunFromTemplate(const String& wiringPath);

// The app entry point (apps[] row "Projects" / top-level menu line "Projects").
void projectsAppLauncher(void);

#endif // PROJECTS_APP_H
