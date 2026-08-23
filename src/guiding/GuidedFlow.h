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
    bool     bandAdvisory;  // author wrote `enforce: false` - measure and REPORT
                            // the value, do not judge it. open/short still fail
                            // (those are placement errors, not value errors);
                            // only the in-band verdict is waived, and the
                            // measurement still lands in the part's
                            // measuredOhms for a companion script to use.
                            //
                            // Stored INVERTED on purpose: all four GuideStep
                            // construction sites here memset to zero, and three
                            // of them are the `auto:` synthesis path. A
                            // positive `enforce` would default to false at any
                            // site someone forgets, silently disarming every
                            // synthesized check. Zero has to mean "enforce".
    char     text[96];      // author prompt (parsed by quote-pair, LAST field)
    char     script[40];    // RUN_SCRIPT path: PARSED AND STORED, NEVER RUN.
                            // No run-step execution shipped on this branch and
                            // no bundled project uses `do: run`; the runtime
                            // prints a skip line and advances (GuidedFlow.cpp,
                            // RUN_SCRIPT case). Execution is deferred.
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
// The long-form terminal line for a terminal poll ("" when none):
// "R2: 46.3k (band 35.3k-58.8k, 105uA @ 5.0V)". Printed on PASS AND FAIL -
// invest-measurement.md §4: "show what it reads as it's placed", not "show it
// when it's wrong". The OLED gets ck.val through ReadingDisplay; this is the
// terminal's fuller sentence and it may contain spaces.
const char* guideCheckDetail(void);
void guideCheckAbort(void);

// ---------------------------------------------------------------------------
// Part values, bands and display formatting (invest-measurement.md §2 / §4)
// ---------------------------------------------------------------------------
// Pure functions - no hardware, no state - so they build on BOTH targets and
// the `guideband` debug command can exercise the whole band derivation
// off-bench. Implemented in GuideChecks.cpp OUTSIDE its OG_JUMPERLESS guard.

enum class PartValueKind : uint8_t { NONE, OHMS, FARADS, HENRIES, VOLTS };
struct ParsedPartValue {
    PartValueKind kind;
    float v;        // ohms / farads / henries / volts; -1 when kind == NONE
};

// PartDefinition.value ("10k", "4.7k", "330", "1M", "10uF", "100nF") with
// typeStr disambiguating unitless strings and the m/M quirk. Grammar: a
// strtof prefix, optional spaces, an optional multiplier char
// (p n u µ k K m M), an optional unit token (F H V R / "ohm"). On a RESISTOR
// both `m` and `M` mean MEGA - the shipped convention, milliohms are a
// non-goal; on a capacitor/inductor `m` is milli and `M` is a reject.
// Rejected (kind NONE, v -1): no digits, v <= 0, trailing junk ("2k2", "4R7"
// infix notation are documented non-goals), and any unit/type contradiction
// ("10uF" declared on a resistor).
ParsedPartValue parsePartValue(const char* s, const char* typeStr);

// ~3 significant figures, always <= 7 chars: 330 / 4.68k / 46.3k / 220k /
// 1.20M. Fits ck.val[24] with room for a prefix.
void formatOhms(float r, char* out, size_t n);

// The derived continuity band in OHMS. tol_total = tol_author (per-part
// `tol:`, 0 = the 15 % default) + tol_meas (R <= 10k: 5, <= 100k: 10, else
// 25). Returns false when rNom is unusable.
bool guideResistorBand(float rNom, uint8_t tolAuthorPct, float* lo, float* hi);

// Stimulus volts a continuity check drives for this nominal R
// (invest-measurement.md §1.5): 3.3 V by default, 5.0 V at >= 20k to lift the
// current off the shunt's floor. rNom <= 0 (no parsed value) gives 3.3.
float guideStimulusVolts(float rNom);

// `guideband <value> [type] [tol]` - the debug command's whole body. Prints
// one machine-parseable GUIDEBAND line.
void guideBandReport(const char* value, const char* typeStr, int tolPct, Stream* out);

#endif // GUIDED_FLOW_H
