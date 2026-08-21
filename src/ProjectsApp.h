// SPDX-License-Identifier: MIT
#ifndef PROJECTS_APP_H
#define PROJECTS_APP_H

#include <Arduino.h>

// Projects launcher (design: CodeDocs/DESIGN_PROJECTS_SUBSYSTEM.md §1
// "Menu + launcher"). A project lives in /projects/<short_name>/ as
// wiring.yaml (+ wiring.<variant>.yaml), main.py (+ main.<variant>.py) and an
// optional README.md. A project wiring file IS a slot YAML plus the
// launcher-only `meta:` block and the guided-placement `parts:`/`guide:`
// sections (States.h's header comment documents what the slot parser does
// with them: `meta:`/`guide:` are swallowed and never round-tripped, which is
// why the launcher text-scans `meta:` itself).

// Max projects the picker lists, and max variants per project. Both bound
// stack/static arrays in the launcher - not a filesystem limit.
#define PROJECTS_MAX 12
#define PROJECT_VARIANTS_MAX 6

// Root of the project tree on the filesystem.
#define PROJECTS_DIR "/projects"

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

// Guided-flow seam (task 6: src/guiding/GuidedFlow.cpp is the runtime).
// Contract:
//   - called BEFORE any temp-slot entry, only for wirings that carry an
//     un-indented `parts:` or `guide:` line;
//   - true  = the guided flow handled everything (resume offer, destination
//             slot, guideRun, cleanup - INCLUDING cancels at its own pickers:
//             a user who cancelled a guided build did not ask for the
//             non-guided path) and the launcher returns at once;
//   - false = the launcher proceeds with the non-guided temp-slot path
//             (currently never returned - kept for future gating).
// The guided path never touches temporarySlotActive: it builds directly into
// a destination slot (design §7).
bool runGuidedProject(const String& dir, const String& wiringPath);

// Headless/HIL entry (the 'z' single-char command): same flow with the
// destination slot supplied up front, bypassing the encoder pickers. The
// resume offer still appears and is serial-drivable (yesNoMenu takes y/n;
// any other byte cancels).
bool runGuidedProjectTo(const String& wiringPath, int destSlot);

// The app entry point (apps[] row "Projects" / menu line "-Project\31s").
void projectsAppLauncher(void);

#endif // PROJECTS_APP_H
