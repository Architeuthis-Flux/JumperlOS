// SPDX-License-Identifier: MIT
#ifndef GUIDED_FLOW_H
#define GUIDED_FLOW_H

#include <Arduino.h>

// Guided-placement runtime, task 6 slice: manual advance only. Design:
// CodeDocs/DESIGN_GUIDED_PLACEMENT.md §2 (this data model), §3 (state
// machine + controls), §4 (rendering), §7 (resume). The parts layer it
// commits through is src/routing/PartPlacement.h; guideProgress persistence
// is the one-line flow-map documented in States.h.
//
// Electrical checks are task 7: every `check:` value is PARSED and STORED
// here, but guideTick resolves them all to NONE ("check pending firmware
// support") until GuideChecks lands.

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

#endif // GUIDED_FLOW_H
