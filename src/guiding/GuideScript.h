// SPDX-License-Identifier: MIT
#ifndef GUIDE_SCRIPT_H
#define GUIDE_SCRIPT_H

#include <Arduino.h>

struct PartDefinition;

// The guide DATA layer: step model, the guide:/power: parser, and the
// electrical-check seam - everything about a guide that is not the (dying)
// modal runner. Split out of GuidedFlow.h so the ambient services
// (PartLabels / StepViewer), the on-demand check command, and GuideChecks.cpp
// depend only on data + polled checks, never on the blocking session machine.
// Format authority: the States.h header comment (guide: section) and
// CodeDocs/DESIGN_GUIDED_PLACEMENT.md §2 / §5.

enum class GuideStepType : uint8_t { NOTE, PLACE, CONNECT, POWER_ON, VERIFY, RUN_SCRIPT };
enum class GuideCheck    : uint8_t { NONE, PRESENCE, CONTINUITY, VF, VOLTAGE, OSCILLATES, I2C_ACK, RAIL_SANE };
enum class GuideOnFail   : uint8_t { WARN, RETRY, SKIP, BLOCK };

struct GuideStep {
    GuideStepType type;
    int8_t   partIdx;       // PLACE: index into globalState.parts; -1 otherwise
    int16_t  n1, n2;        // CONNECT endpoints (-1 = unset)
    int16_t  target;        // VERIFY target node (-1 = unset)
    GuideCheck check;
    float    min, max;
    GuideOnFail onFail;
    uint16_t timeoutMs;     // default 1500
    bool     probeConfirm;  // probe-tap gate (tap n1/n2, or `t <row>`)
    bool     bandAdvisory;  // author wrote `enforce: false` - measure and REPORT
                            // the value, do not judge it. open/short still fail
                            // (those are placement errors, not value errors);
                            // only the in-band verdict is waived, and the
                            // measurement still lands in the part's
                            // measuredOhms for a companion script to use.
                            //
                            // Stored INVERTED on purpose: all four GuideStep
                            // construction sites memset to zero, and three
                            // of them are the `auto:` synthesis path. A
                            // positive `enforce` would default to false at any
                            // site someone forgets, silently disarming every
                            // synthesized check. Zero has to mean "enforce".
    char     text[96];      // author prompt (parsed by quote-pair, LAST field)
    char     script[40];    // RUN_SCRIPT path: PARSED AND STORED, NEVER RUN.
                            // No run-step execution shipped and no bundled
                            // project uses `do: run`; execution is deferred.
};

// Step capacity. OG runs the same data model but has MAX_PARTS 6 (auto-
// synthesis tops out at 8 steps) and an RP2040 with ~50 KB total free SRAM -
// a 48-step table (~7.5 KB) is V5 headroom the OG doesn't have, so it gets half.
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
    // The project file's power: section, re-read here so consumers that need
    // the FILE's rail/DAC values (rail_sane stimulus, the step viewer's
    // power_on text) read them from the parse, not from whatever the live
    // state holds at that moment.
    bool  hasPower;
    float topRail, bottomRail, dac0, dac1;
};

// Parse the guide: section (plus power: and, when synthesizing, the already-
// loaded globalState.parts) of a project YAML. Call AFTER the wiring has been
// loaded into globalState - `part:` names resolve against the live parts
// table. Returns false only when the file can't be read; malformed steps are
// skipped with warnings appended to err (parse-warning style, like bridges).
bool guideParse(const char* yamlPath, GuideScript& out, String& err);

// Machine token for a check ("presence", "continuity", "vf", ... "none").
const char* guideCheckName(GuideCheck c);

// Default check for a part that named none, by part class (§5.2): resistor ->
// continuity, led/diode -> vf, capacitor/ic -> presence, else none. Also the
// rule `z check <part>` uses to synthesize a one-off step for a bare part.
GuideCheck guideDefaultCheckForPart(const PartDefinition& p);

// ---------------------------------------------------------------------------
// GuideChecks seam - implemented in GuideChecks.cpp
// ---------------------------------------------------------------------------
// Checks run as a polled sub-state: begin once, poll every pass (never
// blocking - the pump keeps the INA219 poll and the one-shot tap service
// alive between polls), abort on any early exit. Teardown is guaranteed on
// EVERY path - ephemeral connections removed, DAC0/rails restored to
// globalState.power truth - and abort is idempotent, safe to call when no
// check is running.

#define GUIDE_CHECK_RUNNING      0
#define GUIDE_CHECK_PASS         1
#define GUIDE_CHECK_FAIL        -1   // measured bad - treat per on_fail
#define GUIDE_CHECK_SKIPPED     -2   // refused (rows/nodes in use) - nothing measured
#define GUIDE_CHECK_UNSUPPORTED -3   // not runnable (OG / missing fields) - warn-class

struct GuideCheckRun {
    const GuideStep*   step;
    const GuideScript* script;       // power: values for rail_sane's stimulus
    bool powerApplied;               // rails already live
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

#endif // GUIDE_SCRIPT_H
