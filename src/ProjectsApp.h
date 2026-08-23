// SPDX-License-Identifier: MIT
#ifndef PROJECTS_APP_H
#define PROJECTS_APP_H

#include <Arduino.h>

#include "config.h"   // JL_PROJECT_RUN_HISTORY (single run file vs numbered)

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
// (or creates) a project state file and makes it the persistent active
// context (SlotManager::loadSlotFromPath adopts it), exactly like clicking a
// YAML in the Files browser. The run file is a verbatim byte copy of the
// chosen wiring; the first save strips `meta:`/`guide:` (never round-tripped)
// and keeps parts:/bridges:/power:/guideProgress:/runSource:.
//
// WHICH FILE depends on JL_PROJECT_RUN_HISTORY (config.h):
//
//   0, THE DEFAULT - ONE run file per project:
//
//        /projects/<dir>/<dir>_run.yaml
//
//     Silently reused. A launch with no file creates it and goes. A launch
//     with a file REOPENS it - no prompt - unless a guided build is
//     MID-FLIGHT in it (guideProgress with step < of), in which case exactly
//     one prompt asks resume / start-fresh-which-overwrites / cancel.
//     KEEPING a run is `slots` > `save to`, not a new file per launch.
//
//   1 - the wave-2 numbered scheme:
//
//        /projects/<dir>/<dir>_<N>.yaml        N decimal, 1..9999, no padding
//
//     N = max+1 per launch, with the load-latest / start-new prompt,
//     `z ... run=<N>` and the pile-up hint.
//
// NAMESPACE INVARIANT (design-launcher §3 - "belt and braces on both sides"):
//   - variant files are  wiring*.yaml      (minus *_original*)
//   - run files are      <dir>_run.yaml    and/or  <dir>_<digits>.yaml
//   - the three sets are disjoint UNLESS the project directory itself is
//     named `wiring*` (then `wiring_run.yaml` / `wiring_1.yaml` would match
//     both), or `slot*` (which would collide with the canonical slot
//     namespace at a glance).
//     Guards, both sides: listVariantFiles() skips any name matching THIS
//     directory's run-file patterns (numbered AND `_run`, case-insensitively
//     on the extension exactly as the wiring test is), projectScanRunFiles()
//     requires the exact `<dir>_` prefix followed by 1-4 DIGITS - so it can
//     never pick up `<dir>_run.yaml`, whose middle is not a number - and
//     projectBeginRun() REFUSES a project directory whose name
//     startsWith("wiring") or startsWith("slot") - prefix, not equality,
//     because SlotManager::isTemplatePath() would false-positive on
//     /projects/wiringX/wiringX_run.yaml and the template write-guard and the
//     run files must stay disjoint.
//     The two run-file spellings are disjoint from EACH OTHER for the same
//     reason ("run" is not a digit string), so a board that used to be on a
//     numbered build keeps its old files as inert leftovers: single-file mode
//     never scans for them, never opens one and never deletes one. The Files
//     browser removes them.
//
// Provisioning can neither overwrite nor back up a run file: initializeProjects
// iterates the compiled projectFiles[] canonical paths only, and the
// forced-refresh branch writes wiring_original*.yaml backups. No code change
// is needed for that - it is an invariant of the namespaces above.

// Max projects the picker lists, and max variants per project. Both bound
// stack/static arrays in the launcher - not a filesystem limit.
#define PROJECTS_MAX 12
#define PROJECT_VARIANTS_MAX 6

// Root of the project tree on the filesystem.
#define PROJECTS_DIR "/projects"

// The single-file mode's run-file basename suffix: <dir>_run.yaml. Deliberately
// NOT a number, which is exactly what keeps it out of the numbered scan.
#define PROJECT_RUN_SUFFIX "_run"

// Highest run number a project directory can hold (4 digits, design §1.1).
// Numbering is MONOTONIC: gaps left by deleted runs are never reused, because
// reusing one resurrects a stale terminal/HIL transcript's idea of which file
// is which. JL_PROJECT_RUN_HISTORY only.
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

// Create the project's run file - /projects/<dir>/<dir>_run.yaml, or
// <dir>_<max+1>.yaml under JL_PROJECT_RUN_HISTORY - by byte-copying
// `templatePath` into it, and open it as the active context (design §1.2). On
// a load failure the just-created file is deleted and the copy+load is
// retried ONCE; a second failure leaves the previous context untouched
// (loadSlotFromPath is atomic on failure) and returns false with `err` set.
// Stamps `runSource: <templatePath>` and saves on success.
//
// In single-file mode this is ALSO the fresh-overwrite path: the copy lands on
// top of an existing <dir>_run.yaml, which is what "start fresh" means. The
// user consented at the prompt and the old bytes are gone from the first
// write, so the recovery discipline is the same as create's, no more.
//
// Refuses a project directory named wiring*/slot* (see the namespace invariant
// above) and, in numbered mode, a project whose run counter has reached
// PROJECT_RUN_MAX_N.
//
// `deferPower` hands the load's rail/DAC apply to the CALLER
// (slotLoadDeferPowerApply, States.h): the launcher uses it so a guided
// project's `power:` cannot reach the rails before its power_on step. The
// caller then owes the context either applyStatePowerToHardware() or a
// deliberate substitute - the guide's own 0 V, or the pre-guide restore.
bool projectBeginRun(const String& dir, const String& templatePath,
                     String& runPathOut, String& err, bool deferPower = false);

// /projects/<dir>/<dir>_run.yaml - the single-file mode's one run file.
// Defined in BOTH modes (the Files browser and the docs name it either way);
// only the launcher's use of it is behind the flag.
String projectRunFilePath(const String& dir);

// Highest existing run NUMBER for `dir` inside `projectPath`, 0 when none.
// countOut receives how many numbered run files exist (the pile-up hint's
// input). Never matches <dir>_run.yaml - the middle has to be 1-4 digits.
// Compiled in both modes; only JL_PROJECT_RUN_HISTORY calls it.
int projectScanRunFiles(const String& projectPath, const String& dir, int& countOut);

// What `z ... new|load|run=<N>` selects. DEFAULT = reuse the existing run
// file when there is one, else new - the launcher's own defaults WITHOUT the
// interactive prompt (headless has to be deterministic, so a mid-flight
// guided build resumes rather than asking). RUN_N is JL_PROJECT_RUN_HISTORY
// grammar and is refused, by name, on a single-file build.
enum class ProjectRunMode : uint8_t { DEFAULT, NEW, LOAD, RUN_N };

// Headless/HIL entry (the `z` single-char command). Serves guided AND
// non-guided projects; `project` is a project directory name ("555") or a
// wiring path ("/projects/555/wiring.alt.yaml" - selects that variant for a
// new run). noScript skips the companion script deterministically.
// Returns false when nothing could be launched (the reason is printed).
bool runProjectHeadless(const String& project, ProjectRunMode mode, int runN,
                        bool noScript);

// MicroPython `load_project("<name>")`: open the project's run file, or
// create it from the wiring when the project has none. LOAD ONLY - no guide,
// no script, and never a prompt (a mid-flight guided build is simply
// reopened; resuming it is the launcher's job). The literal-path form of
// load_project deliberately does NOT come through here (it stays a raw
// loadSlotFromPath adopt, which is what makes the shipped template
// read-only-context guard in SlotManager still load-bearing).
bool projectOpenLatestOrNew(const String& project, String& runPathOut);

// The Files-browser seam: a click on /projects/<dir>/wiring*.yaml routes here
// instead of adopting the template. Runs the full interactive flow for that
// project with the clicked file as the chosen variant (design-slots §3).
void projectRunFromTemplate(const String& wiringPath);

// The app entry point (apps[] row "Guides" / top-level menu line "Guides").
void projectsAppLauncher(void);

#endif // PROJECTS_APP_H
