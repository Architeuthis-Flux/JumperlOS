// SPDX-License-Identifier: MIT
#ifndef GUIDED_FLOW_H
#define GUIDED_FLOW_H

#include <Arduino.h>

#include "GuideScript.h"   // step model, guideParse, check seam, band helpers

// Guided-placement MODAL RUNNER. Design: CodeDocs/DESIGN_GUIDED_PLACEMENT.md
// §3 (state machine + controls), §4 (rendering), §7 (resume). The parts layer
// it commits through is src/routing/PartPlacement.h.
//
// SCHEDULED FOR DEMOLITION (Guides-Simplification plan, A-M3): everything in
// this header dies with the blocking runner; the data layer it used lives on
// in GuideScript.h. Do not add new consumers of anything below.

// How a guideRun() call ended. The launcher needs this to tell exit F (quit
// mid-build: leave the run file alone, resume works next launch) from exit H
// (the build finished: offer the companion script) - design-launcher §2.2.
enum class GuideRunResult : uint8_t {
    PARSE_FAILED,      // the guide source could not be read (dangling runSource)
    NOTHING_TO_DO,     // parsed, but no steps and no parts to synthesize from
    ALREADY_COMPLETE,  // resumeStep >= numSteps: the build is finished, no session ran
    QUIT,              // the user quit before the last step
    COMPLETED          // every step was reached (DONE was shown)
};

// The rails across a guide session (guide-UX design §4, task 7).
//
// IN: the user's pre-guide rail/DAC values, captured by the LAUNCHER from live
// globalState.power BEFORE its run-file load - the last moment they still
// exist. The guide only REPORTS them (the exit tail names what is coming
// back); the caller owns the actual restore, because guideRun's early returns
// never start a session at all.
//
// OUT: `applied` is true when a power_on step really energized the project's
// rails (or a resume past one re-applied them). THE RULE: applied == true ->
// the project's power is the correct final state, restore nothing;
// applied == false -> put the captured values back with save=0, so the run
// file keeps the safe 0 V that guideForcePowerSafe wrote.
struct GuideRunPower {
    bool  haveCaptured;
    float topRail, bottomRail, dac0, dac1;
    bool  applied;              // out
};

// BLOCKING guide runner (probeMode's shape: session struct + tick machine,
// pumping jOS.serviceInner() every pass). resumeStep < 0 = fresh start.
// Assumes the caller (ProjectsApp's run flow / the 'z' command) already loaded
// the wiring into the ACTIVE CONTEXT - which is now the project's run file,
// while `projectYamlPath` stays the CANONICAL wiring (the run file loses its
// `guide:` section on the first save, so a guide parsed from it would work
// once and never resume).
// `committedOut` receives how many steps this SESSION ended up with committed
// (0 on every early return, since no session ran). The launcher gates the
// companion-script offer on it: a build whose every step was skipped reaches
// DONE and returns COMPLETED - "nothing left unfinished" is true - but nothing
// was BUILT, and a script must not run on a circuit nobody assembled.
GuideRunResult guideRun(const char* projectYamlPath, int resumeStep = -1,
                        GuideRunPower* power = nullptr,
                        int* committedOut = nullptr);

struct GuideSession;               // ProbeSession-style, defined in the .cpp
void guideTick(GuideSession& s);

// Force rails + both DACs to 0 V (save=1 so the slot persists the safe
// state). Shared by guideRun's INIT and the destination-slot setup in
// ProjectsApp.cpp - the guide owns the "unpowered until power_on" rule.
void guideForcePowerSafe(void);

#endif // GUIDED_FLOW_H
