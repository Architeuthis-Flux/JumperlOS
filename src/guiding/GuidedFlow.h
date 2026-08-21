// SPDX-License-Identifier: MIT
#ifndef GUIDED_FLOW_H
#define GUIDED_FLOW_H

#include <Arduino.h>

// Guided-placement runtime. Design: CodeDocs/DESIGN_GUIDED_PLACEMENT.md §2
// (this data model), §3 (state machine + controls), §4 (rendering), §5 (the
// verify matrix - GuideChecks.cpp), §7 (resume). The parts layer it commits
// through is src/routing/PartPlacement.h; guideProgress persistence is the
// one-line flow-map documented in States.h.

enum class GuideStepType : uint8_t { NOTE, PLACE, CONNECT, POWER_ON, VERIFY, RUN_SCRIPT };
enum class GuideCheck    : uint8_t { NONE, PRESENCE, CONTINUITY, VF, VOLTAGE, OSCILLATES, I2C_ACK, RAIL_SANE };
enum class GuideOnFail   : uint8_t { WARN, RETRY, SKIP, BLOCK };

struct GuideStep {
    GuideStepType type;
    int8_t   partIdx;       // PLACE: index into globalState.parts; -1 otherwise
    int16_t  n1, n2;        // CONNECT endpoints (-1 = unset)
    int16_t  target;        // VERIFY target node (-1 = unset)
    GuideCheck check;       // stored for task 7; resolves to NONE in this task
    float    min, max;
    GuideOnFail onFail;
    uint16_t timeoutMs;     // default 1500
    bool     probeConfirm;  // STEP_PROBE_WAIT gate (tap n1/n2, or `t <row>`)
    char     text[96];      // author prompt (parsed by quote-pair, LAST field)
    char     script[40];    // RUN_SCRIPT path (execution lands with task 9)
};

// Step capacity. OG runs the same machine but has MAX_PARTS 6 (auto-synthesis
// tops out at 8 steps) and an RP2040 with ~50 KB total free SRAM - a 48-step
// table (~7.5 KB) is V5 headroom the OG doesn't have, so it gets half.
#if defined(OG_JUMPERLESS)
#define MAX_GUIDE_STEPS 24
#else
#define MAX_GUIDE_STEPS 48
#endif

struct GuideScript {
    char title[32];
    char sourcePath[96];    // matches PartsState::guideSource[96]
    GuideStep steps[MAX_GUIDE_STEPS];
    int  numSteps;
    bool autoFromParts;     // steps were synthesized from parts: (auto mode)
    // The project file's power: section, re-read here because the guide
    // forces rails+DACs to 0 V from INIT until the power_on step - so the
    // values to apply at power_on must come from the file, not from whatever
    // the (deliberately zeroed) live state holds.
    bool  hasPower;
    float topRail, bottomRail, dac0, dac1;
};

// Parse the guide: section (plus power: and, when synthesizing, the already-
// loaded globalState.parts) of a project YAML. Call AFTER the wiring has been
// loaded into globalState - `part:` names resolve against the live parts
// table. Returns false only when the file can't be read; malformed steps are
// skipped with warnings appended to err (parse-warning style, like bridges).
bool guideParse(const char* yamlPath, GuideScript& out, String& err);

// BLOCKING guide runner (probeMode's shape: session struct + tick machine,
// pumping jOS.serviceInner() every pass). resumeStep < 0 = fresh start.
// Assumes the caller (ProjectsApp's guided seam / the 'z' command) already
// loaded the wiring into the destination slot and made it active.
void guideRun(const char* projectYamlPath, int resumeStep = -1);

struct GuideSession;               // ProbeSession-style, defined in the .cpp
void guideTick(GuideSession& s);

// Force rails + both DACs to 0 V (save=1 so the slot persists the safe
// state). Shared by guideRun's INIT and the destination-slot setup in
// ProjectsApp.cpp - the guide owns the "unpowered until power_on" rule.
void guideForcePowerSafe(void);

// ---------------------------------------------------------------------------
// GuideChecks seam (task 7) - implemented in GuideChecks.cpp
// ---------------------------------------------------------------------------
// STEP_VERIFY drives these as a polled sub-state: begin once, poll every
// tick (never blocking - the guide loop pumps jOS.serviceInner() between
// polls, which keeps the INA219 poll and the one-shot tap service alive),
// abort on any early exit (quit / skip / back mid-check). Teardown is
// guaranteed on EVERY path - ephemeral connections removed, DAC0/rails
// restored to globalState.power truth - and abort is idempotent, safe to
// call when no check is running.

#define GUIDE_CHECK_RUNNING      0
#define GUIDE_CHECK_PASS         1
#define GUIDE_CHECK_FAIL        -1   // measured bad - treat per on_fail
#define GUIDE_CHECK_SKIPPED     -2   // refused (rows/nodes in use) - nothing measured
#define GUIDE_CHECK_UNSUPPORTED -3   // not runnable (OG / missing fields) - warn-class

struct GuideCheckRun {
    const GuideStep*   step;
    const GuideScript* script;       // power: values for rail_sane's stimulus
    bool powerApplied;               // rails already live (power_on committed/resumed)
};

void guideCheckBegin(const GuideCheckRun& run);
// valOut receives the machine token for the RESULT line's val= (no spaces).
int  guideCheckPoll(char* valOut, size_t valLen);
// Optional human hint after a terminal poll ("" when none) - "flip it?" etc.
const char* guideCheckHint(void);
void guideCheckAbort(void);

#endif // GUIDED_FLOW_H
