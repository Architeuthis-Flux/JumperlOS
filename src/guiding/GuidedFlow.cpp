// SPDX-License-Identifier: MIT
// Guided-placement runtime (task 6: manual advance). Design authority:
// CodeDocs/DESIGN_GUIDED_PLACEMENT.md §2-§4, §6-§7; as-built parts contract
// in src/routing/PartPlacement.h + the States.h header comment.
//
// Shape: probeMode's blocking-loop pattern - a GuideSession struct and a
// guideTick() enum state machine, pumped by jOS.serviceInner() so only the
// CRITICAL "inner set" services run (Highlighting / MeasureMode / SlotManager
// are silenced for the guide's whole life - auto-save can't race a step, the
// guide saves explicitly at each commit).
//
// Status-line grammar (one line per REST-state transition, machine-parseable):
//   GUIDE step=<i>/<n> id=<type>_<part|node> state=<STATE>
// with ` check=<name>` appended at VERIFY (emitted only when a REAL check
// launches - the machine now rests there while the check polls) and
// ` check=<name> val=<x> ok=<0|1>[ on_fail=<policy>]` at RESULT. Emitted
// states: INIT, WAIT, PROBE_WAIT, VERIFY (real checks only), RESULT, COMMIT,
// BACK, DONE, EXIT. ENTER stays internal (never a resting state).
//
// Electrical checks (task 7): STEP_VERIFY drives GuideChecks.cpp as a polled
// sub-state (begin / poll / abort - the seam in GuidedFlow.h). Results feed
// the on_fail policy at STEP_RESULT:
//   pass                  -> commit (verify-only 'v' returns to the wait)
//   measured FAIL + warn  -> note + back to the wait; the NEXT confirm
//                            advances without re-running (a deliberate
//                            "I saw the X" second press); 'v' re-runs
//   FAIL + retry/block    -> back to the wait; confirm re-runs the check
//                            (they differ in wording only - both skippable
//                            via the explicit skip gesture, per §1.2)
//   FAIL + skip           -> auto-skip the step
//   SKIPPED/UNSUPPORTED   -> nothing was measured: warn-class prints the
//                            note and proceeds with the confirm the user
//                            already gave (no double-press for a check that
//                            could not run); block still gates, skip skips

#include "GuidedFlow.h"

#include "Commands.h"          // requestLedShow, refreshConnections
#include "FilesystemStuff.h"   // safeFileReadAll
#include "Graphics.h"          // b (breadboard print), cycleTerminalColor
#include "GraphicOverlays.h"   // graphicOverlayState
#include "JumperlOS.h"         // jOS.serviceInner()
#include "NetManager.h"        // printNodeOrName, findNodeInNet
#include "PartPlacement.h"     // applyPartPlacement / removePartPlacement / partPinNode
#include "Peripherals.h"       // setTopRail / setBotRail / setDac0voltage / setDac1voltage
#include "Probing.h"           // justReadProbe, probeButton
#include "ReadingDisplay.h"
#include "RotaryEncoder.h"
#include "States.h"            // globalState, SlotManager
#include "config.h"            // jumperlessConfig.hardware.probe_revision
#include "oled.h"

// States.cpp's node-name resolution - the exact helper bridges: and parts:
// parsing use, so guide steps speak the same node vocabulary.
extern int parseNodeName(const String& nodeName);
extern bool parseBoolean(const String& val, bool& success);

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

enum class GuideState : uint8_t {
    INIT, STEP_ENTER, STEP_WAIT, STEP_PROBE_WAIT, STEP_VERIFY,
    STEP_RESULT, STEP_COMMIT, STEP_BACK, DONE, EXIT
};

// KEVIN'S RULING (wave 3): the modal ADJUST state and the 260 ms confirm pend
// are GONE. "honestly this whole thing is confusing, we need to streamline
// this interface." The interface is DIRECT GESTURES ONLY - a probe tap on a
// free hole MOVES the part, a tap on its own lit footprint SNAPS it, and the
// wheel BROWSES and nothing else. With no double-click gesture left to
// disambiguate, a wheel click confirms the instant it is released again (the
// pend existed only to tell click from double-click). The encoder still emits
// DOUBLECLICKED; the guide simply does not consume it - see guideReadInput.

struct GuideSession {
    GuideScript* script;
    GuideState state;
    int  stepIdx;                       // current step (0-based); == numSteps at DONE
    bool done;                          // loop exit flag (set by EXIT)
    bool exitRequested;                 // true = user quit; false = ran to DONE
    bool verifyOnly;                    // 'v' pressed: run check, don't commit
    GuideState waitReturn;              // where a verify-only RESULT returns to
    bool committed[MAX_GUIDE_STEPS];
    bool skipped[MAX_GUIDE_STEPS];
    bool powerApplied;                  // a POWER_ON step has run (rails live)
    bool summaryShown;                  // DONE printed its summary already
    unsigned long lastPulseMs;
    bool pulseOn;
    int  savedRotaryDivider;
    // Check machinery (STEP_VERIFY drives GuideChecks.cpp)
    bool checkRunning;                  // guideCheckBegin issued, polling
    int  checkOutcome;                  // GUIDE_CHECK_* (valid at STEP_RESULT)
    char checkVal[24];                  // machine token for the RESULT val=
    bool checkWarned;                   // measured warn-fail on this step: the
                                        // next confirm advances without re-run
    // Pre-guide rails, captured by the LAUNCHER before its run-file load
    // (design §4.2 as corrected). The guide only ever REPORTS them at the exit
    // tail - the caller owns the actual restore, because the early-return
    // paths never reach a session at all.
    bool  haveRestorePower;
    float restoreTop, restoreBot, restoreDac0, restoreDac1;
};

// Single static instances - GuideScript is ~7.5 KB (V5) and the guide is a
// modal app on core 0, never re-entrant (same argument as the parser scratch
// in PartPlacement.cpp).
static GuideScript guideScript;
static GuideSession guideSession;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static const char* guideCheckName(GuideCheck c) {
    switch (c) {
        case GuideCheck::PRESENCE:   return "presence";
        case GuideCheck::CONTINUITY: return "continuity";
        case GuideCheck::VF:         return "vf";
        case GuideCheck::VOLTAGE:    return "voltage";
        case GuideCheck::OSCILLATES: return "oscillates";
        case GuideCheck::I2C_ACK:    return "i2c";
        case GuideCheck::RAIL_SANE:  return "rail_sane";
        default:                     return "none";
    }
}

static const char* guideTypeName(GuideStepType t) {
    switch (t) {
        case GuideStepType::PLACE:      return "place";
        case GuideStepType::CONNECT:    return "connect";
        case GuideStepType::POWER_ON:   return "power_on";
        case GuideStepType::VERIFY:     return "verify";
        case GuideStepType::RUN_SCRIPT: return "run";
        default:                        return "note";
    }
}

// `GUIDE step=... id=...` id for a step: <type>_<part|node> (place_R1,
// connect_12, verify_7, note_3); power_on and run stand alone.
static void guideStepId(const GuideScript& sc, int idx, char* out, size_t outLen) {
    if (idx < 0 || idx >= sc.numSteps) {
        snprintf(out, outLen, "guide");
        return;
    }
    const GuideStep& st = sc.steps[idx];
    switch (st.type) {
        case GuideStepType::PLACE:
            if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
                snprintf(out, outLen, "place_%s", globalState.parts.parts[st.partIdx].name);
            } else {
                snprintf(out, outLen, "place_%d", idx + 1);
            }
            break;
        case GuideStepType::CONNECT:   snprintf(out, outLen, "connect_%d", st.n1); break;
        case GuideStepType::VERIFY:    snprintf(out, outLen, "verify_%d", st.target); break;
        case GuideStepType::POWER_ON:  snprintf(out, outLen, "power_on"); break;
        case GuideStepType::RUN_SCRIPT: snprintf(out, outLen, "run_%d", idx + 1); break;
        default:                       snprintf(out, outLen, "note_%d", idx + 1); break;
    }
}

static void guideStatusLine(const GuideSession& s, const char* stateName,
                            const char* extra = nullptr) {
    char id[32];
    int shown = s.stepIdx;
    if (shown >= s.script->numSteps) shown = s.script->numSteps - 1;
    guideStepId(*s.script, shown, id, sizeof(id));
    Serial.print("\r\nGUIDE step=");
    Serial.print((s.stepIdx < s.script->numSteps) ? (s.stepIdx + 1) : s.script->numSteps);
    Serial.print("/");
    Serial.print(s.script->numSteps);
    Serial.print(" id=");
    Serial.print(id);
    Serial.print(" state=");
    Serial.print(stateName);
    if (extra != nullptr && extra[0] != '\0') {
        Serial.print(" ");
        Serial.print(extra);
    }
    Serial.println();
    Serial.flush();
}

// Probe-button polarity: the probe_revision>3 swap lives only in the
// MicroPython wrappers (jl_probe_button_*, JumperlessMicroPythonAPI.cpp);
// raw probeButton.getButtonPress() is unswapped. Post-swap semantics here:
// 0 = none, 1 = CONNECT/confirm, 2 = REMOVE/back.
static int guideProbeButton(void) {
    int bPress = probeButton.getButtonPress(true);
    if (jumperlessConfig.hardware.probe_revision > 3) {
        if (bPress == 1) bPress = 2;
        else if (bPress == 2) bPress = 1;
    }
    return bPress;
}

void guideForcePowerSafe(void) {
    // save=1 on purpose: the slot YAML must persist the SAFE state, so a
    // half-built slot re-loaded outside the guide comes up unpowered instead
    // of zapping an unfinished circuit with the project's rail voltage. The
    // power_on step re-applies the file's values (GuideScript.topRail etc).
    setTopRail(0.0f, 1, 0);
    setBotRail(0.0f, 1, 0);
    setDac0voltage(0.0f, 1, 0);
    setDac1voltage(0.0f, 1, 0);
}

// Display name for the net holding `node` - the netDisplayName pattern
// (Highlighting.cpp): custom name, then the net's own name, then "Net N".
static const char* guideNetNameForNode(int node, char* buf, size_t bufLen) {
    int netNum = findNodeInNet(node);
    if (netNum <= 0 || netNum >= MAX_NETS) return "(no net)";
    // findNodeInNet's gpio/adc fallbacks return NODE values - only accept a
    // real net at that index (same guard as partsReassertNetNames).
    if (globalState.connections.nets[netNum].number != netNum) return "(no net)";
    const char* name = globalState.display.getNetName(netNum);
    if (name != nullptr && name[0] != '\0') return name;
    name = globalState.connections.nets[netNum].name;
    if (name != nullptr && name[0] != '\0') return name;
    snprintf(buf, bufLen, "Net %d", netNum);
    return buf;
}

static void guideIdentifyRow(int row) {
    char buf[16];
    const char* name = guideNetNameForNode(row, buf, sizeof(buf));
    Serial.print("\r\n  row ");
    printNodeOrName(row, 0, -1, &Serial);
    Serial.print(" -> ");
    Serial.println(name);
    Serial.flush();
    ReadingDisplay::show(name, row);
}

// ---------------------------------------------------------------------------
// Overlay rendering (V5 LED matrix; every painter below no-ops on OG)
// ---------------------------------------------------------------------------
//
// OG fallback decision (DESIGN §4.2 offered two): on OG the guide paints NO
// LEDs at all and relies on OLED + terminal; each COMMIT's refreshConnections
// lights the newly committed bridges through the ordinary 1-LED-per-row net
// render, which is §4.2's "bridges as colors" effect for finished steps
// without pre-committing anything. Pre-committing the CURRENT step's bridges
// for target lighting was the alternative; it buys one dim LED per target
// row at the cost of un-commit bookkeeping on a board that can't show the
// footprint anyway - the simpler path is the correct one here.

// Overlay coordinate orientation - VERIFIED, three independent sources agree
// (see task-6 report for the full chains):
//  1. printChar's columnMask maps font bit b0 -> pixel offset 0, and glyphs
//     render upright on the bench (FileManager menus, probeCalib labels), so
//     pixel offset 0 = the physically UPPER hole of a row-column in BOTH
//     halves; screenMap's overlay row 1 block is exactly the offset-0 pixels.
//  2. The wire renderer's staple geometry ("position 0 = the outer tip",
//     Graphics.cpp) mirrors bottom-half columns (4 - column), which is only
//     consistent if pixel order runs the same physical direction in both
//     halves.
//  3. Kevin's published API docs (Jumperless-docs 09.5, Graphic Overlays):
//     "Row 1-5: Top half (A-E) ... y=5 (Row 5/E)".
// Net: overlay rows 1..10 run VISUAL TOP -> BOTTOM. Row 1 = top outer edge
// (A), row 5 = ravine-adjacent top (E), row 6 = ravine-adjacent bottom (F),
// row 10 = bottom outer edge (J). GraphicOverlays.h's old "(E, D, C, B, A)"
// guess was the wrong lettering; fixed there alongside this task.
// 0 = the verified order above; 1 flips within each half (fallback knob only,
// nothing sets it).
#define GUIDE_OVERLAY_ROW_ORDER 0

static const char* GUIDE_OVERLAY_FP  = "_GUIDE_FP_";
static const char* GUIDE_OVERLAY_TGT = "_GUIDE_TGT_";

// One warning per session when addOverlay refuses (-1: the destination
// project already fills the 8 overlay slots) - without it the guide just
// silently paints no LEDs and a bench user has no idea why. Reset by
// guideSessionBegin (which runs on BOTH targets, so the flag itself lives
// outside the V5 guard below). The TGT overlay rewrites at ~2 Hz, hence
// once, not per-call.
static bool guideOverlayAddWarned = false;

// LED overlay painting is V5-only: the OG has one LED per breadboard row and
// no overlay fabric, so every call site below is already inside a
// `#if defined(OG_JUMPERLESS) return;` early-out. Guarding the whole BLOCK
// and not just the call sites is SOURCE HYGIENE, not a RAM saving - the
// linker's --gc-sections already dropped the unused 1.2 KB
// guideOverlayScratch from the OG image (measured: .bss identical either
// way, the symbol simply absent from the OG ELF). The point is that dead
// code stops being compiled at all.
#if !defined(OG_JUMPERLESS)

// Dim class colors + pin-1 marker (DESIGN §4.1).
static const uint32_t GUIDE_COLOR_POWER  = 0x1A0000;
static const uint32_t GUIDE_COLOR_GND    = 0x001A02;
static const uint32_t GUIDE_COLOR_SIGNAL = 0x000818;
static const uint32_t GUIDE_COLOR_NC     = 0x040404;
static const uint32_t GUIDE_COLOR_PIN1   = 0x180800;

static uint32_t guidePinClassColor(uint8_t pinClass) {
    switch (pinClass) {
        case 1: return GUIDE_COLOR_POWER;
        case 2: return GUIDE_COLOR_GND;
        case 3: return GUIDE_COLOR_NC;
        default: return GUIDE_COLOR_SIGNAL;
    }
}

// ~4x brightness for the pulsing target phase, per-channel clamped.
static uint32_t guideBrighten(uint32_t c) {
    uint32_t r = ((c >> 16) & 0xFF) * 4; if (r > 0xFF) r = 0xFF;
    uint32_t g = ((c >> 8) & 0xFF) * 4;  if (g > 0xFF) g = 0xFF;
    uint32_t bl = (c & 0xFF) * 4;        if (bl > 0xFF) bl = 0xFF;
    return (r << 16) | (g << 8) | bl;
}

// Paint breadboard node n (1-60) into a full-board 10x30 overlay buffer:
// col = ((n-1) % 30), all 5 sub-rows of its half (a row IS one net - the
// whole 5-hole column lights). Nodes off the breadboard (rails, nano) have
// no overlay cells and are skipped. Sub-row selective drawing would consult
// GUIDE_OVERLAY_ROW_ORDER; whole-column fills are orientation-independent.
static void guidePaintNode(uint32_t* buf300, int node, uint32_t color) {
    if (node < 1 || node > 60) return;
    int col = (node - 1) % 30;              // 0-based
    int rowBase = (node <= 30) ? 0 : 5;     // 0-based overlay row of the half
    for (int r = 0; r < 5; r++) {
        buf300[(rowBase + r) * 30 + col] = color;
    }
}

// Scratch overlay buffer (300 x 4 B): static, single-threaded modal use.
static uint32_t guideOverlayScratch[MAX_OVERLAY_PIXELS];

static void guideAddOverlayWarned(const char* name) {
    if (graphicOverlayState.addOverlay(name, 1, 1, 30, 10, guideOverlayScratch) < 0 &&
        !guideOverlayAddWarned) {
        guideOverlayAddWarned = true;
        Serial.println("\r\n  (guide LED overlays unavailable - this project's overlay"
                       " slots are full; OLED/terminal guidance continues)");
    }
}

#endif // !OG_JUMPERLESS (overlay painting block)

// _GUIDE_FP_: dim outlines of every PLACED part (rebuilt whole on commit /
// back). Persistent for the whole guide; never written into a SLOT FILE - the
// reserved-name exclusion is overlayIsSessionOnly(), and it gates the YAML
// serializer only. serializeOverlaysToJSON() has no such filter and DOES emit
// `_GUIDE_` overlays, which is deliberate: that is the surface
// overlay_serialize() exposes to the REPL, and test_guide_flow's phase-13b
// needle reads this overlay through it to prove the footprint is rebuilt whole
// rather than accumulated.
static void guideRenderFootprints(void) {
#if defined(OG_JUMPERLESS)
    return;   // overlays no-op on OG (see the fallback note above)
#else
    memset(guideOverlayScratch, 0, sizeof(guideOverlayScratch));
    bool any = false;
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            int node = partPinNode(p, pin);
            // 1-60 only: a compact leg sitting in a RAIL hole has no overlay
            // cells (guidePaintNode skips it), so counting it as `any` used to
            // register an entirely transparent overlay and burn one of the
            // eight slots for nothing (T6 review, nit 4).
            if (node < 1 || node > 60) continue;
            uint32_t color = (pin.pinNumber == 1) ? GUIDE_COLOR_PIN1
                                                  : guidePinClassColor(pin.pinClass);
            guidePaintNode(guideOverlayScratch, node, color);
            any = true;
        }
    }
    if (any) {
        guideAddOverlayWarned(GUIDE_OVERLAY_FP);
    } else {
        graphicOverlayState.removeOverlay(GUIDE_OVERLAY_FP);
    }
#endif
}

// _GUIDE_TGT_: the current step's target holes. place = all pin rows (pin 1
// in marker color), pulsing; connect = n1 static + n2 pulsing (§4.1); verify
// = target pulsing. note/power_on/run have no target.
static void guideRenderTarget(const GuideSession& s, bool pulseOn) {
#if defined(OG_JUMPERLESS)
    (void)s; (void)pulseOn;
    return;
#else
    if (s.stepIdx < 0 || s.stepIdx >= s.script->numSteps) {
        graphicOverlayState.removeOverlay(GUIDE_OVERLAY_TGT);
        return;
    }
    const GuideStep& st = s.script->steps[s.stepIdx];
    memset(guideOverlayScratch, 0, sizeof(guideOverlayScratch));
    bool any = false;
    switch (st.type) {
        case GuideStepType::PLACE:
            if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
                const PartDefinition& p = globalState.parts.parts[st.partIdx];
                for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                    const PartPin& pin = p.pins[j];
                    int node = partPinNode(p, pin);
                    if (node < 1 || node > 60) continue;   // rails paint nothing
                    uint32_t base = (pin.pinNumber == 1) ? GUIDE_COLOR_PIN1
                                                         : guidePinClassColor(pin.pinClass);
                    guidePaintNode(guideOverlayScratch, node,
                                   pulseOn ? guideBrighten(base) : base);
                    any = true;
                }
            }
            break;
        case GuideStepType::CONNECT: {
            uint32_t c1 = guideBrighten(GUIDE_COLOR_SIGNAL);
            guidePaintNode(guideOverlayScratch, st.n1, c1);
            guidePaintNode(guideOverlayScratch, st.n2,
                           pulseOn ? c1 : GUIDE_COLOR_SIGNAL);
            any = (st.n1 >= 1 && st.n1 <= 60) || (st.n2 >= 1 && st.n2 <= 60);
            break;
        }
        case GuideStepType::VERIFY: {
            uint32_t c = GUIDE_COLOR_SIGNAL;
            guidePaintNode(guideOverlayScratch, st.target,
                           pulseOn ? guideBrighten(c) : c);
            any = (st.target >= 1 && st.target <= 60);
            break;
        }
        default:
            break;
    }
    if (any) {
        guideAddOverlayWarned(GUIDE_OVERLAY_TGT);
    } else {
        graphicOverlayState.removeOverlay(GUIDE_OVERLAY_TGT);
    }
#endif
}

static void guideClearOverlays(void) {
#if !defined(OG_JUMPERLESS)
    graphicOverlayState.removeOverlay(GUIDE_OVERLAY_FP);
    graphicOverlayState.removeOverlay(GUIDE_OVERLAY_TGT);
#endif
}

// THE DRAG TRAIL, and why the guide's every LED refresh is CLEAR-FIRST.
//
// Kevin, wave 3: ">>> dragging a part doesn't clear the LEDs behind it,
// filling up the board".
//
// The overlays themselves were never the accumulator, and that is worth
// stating because it is the obvious suspect: guideRenderFootprints() and
// guideRenderTarget() both memset guideOverlayScratch and repaint it whole,
// and GraphicOverlayState::addOverlay() memcpy-REPLACES the stored pixels of
// an overlay that already exists. Register `_GUIDE_FP_` after a move and its
// 300 cells describe the new footprint exactly, with nothing of the old one
// left in them.
//
// The accumulation is one layer down, in the LED pipeline:
//   1. renderGraphicOverlays() writes only NON-TRANSPARENT overlay pixels
//      into the `leds` buffer (GraphicOverlays.cpp: `if (color == 0)
//      continue;`). Cells that stopped being lit are not painted black -
//      they are simply not painted.
//   2. showNets() only lights rows that belong to a net (lightUpNet); it
//      never clears the buffer either.
//   3. clearLEDsExceptRails() runs ONLY when core 1 took a request carrying
//      core1req::LED_CLEAR - i.e. a NEGATIVE requestLedShow() (main.cpp,
//      `if (clearBeforeSend == 1)`).
// The guide only ever posted requestLedShow(1). So every hole a footprint
// ever occupied stayed lit at its last colour for the rest of the session,
// and each move added its vacated rows to the pile: the board fills up.
//
// Highlighting.cpp:267/620 already solved exactly this bug this exact way
// ("use a negative value to force clearBeforeSend, ensuring old highlights
// are fully cleared"). One helper so no guide site can forget: the clear is
// per FRAME and costs a memset on core 2, which is why "clear and rebuild
// the whole _GUIDE_ namespace on every applied move" is affordable.
//
// This also fixes two ghosts nobody had named: browsing to the DONE view
// removes `_GUIDE_TGT_` (its pixels used to stay lit under the summary), and
// guideExitTail removes BOTH overlays (their pixels used to survive the guide
// and sit on the board afterwards).
static void guideShowLeds(void) {
    requestLedShow(-1);   // negative = LED_CLEAR | LED_NETS
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Local copy of the parts parser's flow-map field extractor (PartPlacement.cpp
// keeps its own static; the grammar is frozen by the States.h format doc, so
// the duplication is two matched implementations of one documented rule).
// The key must start a field (preceded by '{', ',' or whitespace).
static String guideFlowField(const String& body, const char* key) {
    int idx = -1;
    int from = 0;
    while (true) {
        int cand = body.indexOf(key, from);
        if (cand < 0) return String("");
        char before = (cand == 0) ? '{' : body.charAt(cand - 1);
        if (before == '{' || before == ',' || before == ' ' || before == '\t') {
            idx = cand;
            break;
        }
        from = cand + 1;
    }
    int start = idx + strlen(key);
    int commaIdx = body.indexOf(',', start);
    int braceIdx = body.indexOf('}', start);
    int endIdx = body.length();
    if (commaIdx >= 0 && (braceIdx < 0 || commaIdx < braceIdx)) endIdx = commaIdx;
    else if (braceIdx >= 0) endIdx = braceIdx;
    String val = body.substring(start, endIdx);
    val.trim();
    return val;
}

// True when `body` opens a flow map this line does not close - i.e. a
// `- {...` step that wrapped onto the following line. Double-quote-aware:
// `text:` prose is free to contain a brace and must not be read as structure.
//
// DOUBLE QUOTES ONLY, and that is not an omission. This format blesses exactly
// one string delimiter: guideFlowField splits on bare , and } with no quote
// handling at all, guideScalar strips `"`, and guideParseStepLine pairs
// `"` ... `"` to lift `text:`. A detector that also honoured `'` would be the
// only thing in the parser that did - and it would turn an ordinary apostrophe
// into an unterminated string, swallow the closing brace, and report a
// perfectly balanced line as unclosed. The join would then eat the NEXT line,
// which may be the next step. `{part: O'Brien, ...}` parsed clean before the
// join existed and must keep doing so.
//
// Residual, stated so nobody "fixes" it back: a SINGLE-quoted YAML string
// carrying an unmatched `{` is misread. That shape was never supported here
// anyway - guideScalar would keep the quotes as literal characters.
static bool guideFlowMapUnclosed(const String& body) {
    int depth = 0;
    bool inString = false;
    for (unsigned int i = 0; i < body.length(); i++) {
        char c = body.charAt(i);
        if (inString) {
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') depth++;
        else if (c == '}' && depth > 0) depth--;
    }
    return depth > 0;
}

static String guideScalar(const String& rest) {
    String v = rest;
    v.trim();
    if (v.length() >= 2 && v.charAt(0) == '"') {
        int endQuote = v.indexOf('"', 1);
        if (endQuote > 0) return v.substring(1, endQuote);
    }
    return v;
}

static GuideCheck guideCheckFromString(const String& s) {
    if (s == "presence")   return GuideCheck::PRESENCE;
    if (s == "continuity") return GuideCheck::CONTINUITY;
    if (s == "vf")         return GuideCheck::VF;
    if (s == "voltage")    return GuideCheck::VOLTAGE;
    if (s == "oscillates") return GuideCheck::OSCILLATES;
    if (s == "i2c")        return GuideCheck::I2C_ACK;
    if (s == "rail_sane")  return GuideCheck::RAIL_SANE;
    return GuideCheck::NONE;
}

// Default check for a place step that named none, by part class (§5.2).
static GuideCheck guideDefaultCheckForPart(const PartDefinition& p) {
    if (p.defaultVerify != 0) return (GuideCheck)p.defaultVerify;
    String t = String(p.typeStr);
    if (t == "resistor")               return GuideCheck::CONTINUITY;
    if (t == "led" || t == "diode")    return GuideCheck::VF;
    if (t == "capacitor" || t == "ic") return GuideCheck::PRESENCE;
    return GuideCheck::NONE;
}

// One `- {do: ..., text: "..."}` flow-map step line (already trimmed to the
// braces). Malformed steps are skipped with a warning, like bridges.
static void guideParseStepLine(const String& body, GuideScript& out, String& err) {
    if (out.numSteps >= MAX_GUIDE_STEPS) {
        // Warned once by the caller (step cap); just drop.
        return;
    }
    GuideStep st;
    memset(&st, 0, sizeof(st));
    st.partIdx = -1;
    st.n1 = st.n2 = st.target = -1;
    st.timeoutMs = 1500;
    st.onFail = GuideOnFail::WARN;

    // text: is parsed by quote-pair and must be the LAST field (§1.2) - split
    // it off first so commas inside the prose can't derail field extraction.
    String fields = body;
    int textIdx = -1;
    int from = 0;
    while (true) {
        int cand = fields.indexOf("text:", from);
        if (cand < 0) break;
        char before = (cand == 0) ? '{' : fields.charAt(cand - 1);
        if (before == '{' || before == ',' || before == ' ' || before == '\t') {
            textIdx = cand;
            break;
        }
        from = cand + 1;
    }
    if (textIdx >= 0) {
        int q1 = fields.indexOf('"', textIdx);
        int q2 = (q1 >= 0) ? fields.indexOf('"', q1 + 1) : -1;
        if (q1 >= 0 && q2 > q1) {
            String txt = fields.substring(q1 + 1, q2);
            strncpy(st.text, txt.c_str(), sizeof(st.text) - 1);
        } else {
            // Warn, don't silently drop - the step still runs, promptless.
            err += "guide step " + String(out.numSteps + 1) +
                   ": unclosed text: quote (text dropped); ";
        }
        fields = fields.substring(0, textIdx);
    }

    String v = guideFlowField(fields, "do:");
    if (v == "note")           st.type = GuideStepType::NOTE;
    else if (v == "place")     st.type = GuideStepType::PLACE;
    else if (v == "connect")   st.type = GuideStepType::CONNECT;
    else if (v == "power_on")  st.type = GuideStepType::POWER_ON;
    else if (v == "verify")    st.type = GuideStepType::VERIFY;
    else if (v == "run")       st.type = GuideStepType::RUN_SCRIPT;
    else {
        err += "guide step " + String(out.numSteps + 1) + ": unknown do: '" + v + "' (skipped); ";
        return;
    }

    v = guideFlowField(fields, "part:");
    if (v.length() > 0) {
        String name = guideScalar(v);
        st.partIdx = (int8_t)globalState.parts.findByName(name.c_str());
        if (st.partIdx < 0) {
            err += "guide step " + String(out.numSteps + 1) + ": unknown part '" + name + "' (skipped); ";
            return;
        }
    }
    v = guideFlowField(fields, "n1:");
    if (v.length() > 0) st.n1 = (int16_t)parseNodeName(v);
    v = guideFlowField(fields, "n2:");
    if (v.length() > 0) st.n2 = (int16_t)parseNodeName(v);
    v = guideFlowField(fields, "target:");
    if (v.length() > 0) st.target = (int16_t)parseNodeName(v);
    v = guideFlowField(fields, "check:");
    if (v.length() > 0) st.check = guideCheckFromString(v);
    v = guideFlowField(fields, "min:");
    if (v.length() > 0) st.min = v.toFloat();
    v = guideFlowField(fields, "max:");
    if (v.length() > 0) st.max = v.toFloat();
    v = guideFlowField(fields, "on_fail:");
    if (v == "retry")      st.onFail = GuideOnFail::RETRY;
    else if (v == "skip")  st.onFail = GuideOnFail::SKIP;
    else if (v == "block") st.onFail = GuideOnFail::BLOCK;
    v = guideFlowField(fields, "timeout_ms:");
    if (v.length() > 0) st.timeoutMs = (uint16_t)v.toInt();
    v = guideFlowField(fields, "probe_confirm:");
    if (v.length() > 0) {
        bool ok;
        bool bval = parseBoolean(v, ok);
        if (ok) st.probeConfirm = bval;
    }
    v = guideFlowField(fields, "script:");
    if (v.length() > 0) {
        String sp = guideScalar(v);
        strncpy(st.script, sp.c_str(), sizeof(st.script) - 1);
    }

    // Per-type requirements (skip-with-warning, never abort the parse).
    if (st.type == GuideStepType::PLACE && st.partIdx < 0) {
        err += "guide step " + String(out.numSteps + 1) + ": place needs part: (skipped); ";
        return;
    }
    if (st.type == GuideStepType::CONNECT && (st.n1 < 0 || st.n2 < 0)) {
        err += "guide step " + String(out.numSteps + 1) + ": connect needs n1:+n2: (skipped); ";
        return;
    }
    if (st.type == GuideStepType::VERIFY && st.target < 0) {
        err += "guide step " + String(out.numSteps + 1) + ": verify needs target: (skipped); ";
        return;
    }
    // Default check for a place step that didn't name one.
    if (st.type == GuideStepType::PLACE && st.check == GuideCheck::NONE &&
        guideFlowField(fields, "check:").length() == 0) {
        st.check = guideDefaultCheckForPart(globalState.parts.parts[st.partIdx]);
    }
    // power_on defaults to rail_sane (§5.2's matrix row) - an explicit
    // `check: none` still opts out. Worst case is a warn-class note in a
    // project with no class-tagged pins (the check passes as "norows").
    if (st.type == GuideStepType::POWER_ON && st.check == GuideCheck::NONE &&
        guideFlowField(fields, "check:").length() == 0) {
        st.check = GuideCheck::RAIL_SANE;
    }

    out.steps[out.numSteps++] = st;
}

// Auto-synthesis (§1.3): one intro note, one place per part in file order,
// one power_on when the file carries a power: section.
static void guideSynthesizeSteps(GuideScript& out) {
    out.autoFromParts = true;
    out.numSteps = 0;

    GuideStep st;
    memset(&st, 0, sizeof(st));
    st.partIdx = -1;
    st.n1 = st.n2 = st.target = -1;
    st.timeoutMs = 1500;
    st.onFail = GuideOnFail::WARN;
    st.type = GuideStepType::NOTE;
    snprintf(st.text, sizeof(st.text), "%s: %d part%s to place. next=confirm",
             out.title[0] ? out.title : "Guided build",
             globalState.parts.numParts,
             globalState.parts.numParts == 1 ? "" : "s");
    out.steps[out.numSteps++] = st;

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS &&
                    out.numSteps < MAX_GUIDE_STEPS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        memset(&st, 0, sizeof(st));
        st.n1 = st.n2 = st.target = -1;
        st.timeoutMs = 1500;
        st.onFail = GuideOnFail::WARN;
        st.type = GuideStepType::PLACE;
        st.partIdx = (int8_t)i;
        st.check = guideDefaultCheckForPart(p);
        if (p.value[0] != '\0') {
            snprintf(st.text, sizeof(st.text), "Place %s (%s), pin 1 at row %d",
                     p.name, p.value, p.baseRow);
        } else {
            snprintf(st.text, sizeof(st.text), "Place %s, pin 1 at row %d",
                     p.name, p.baseRow);
        }
        out.steps[out.numSteps++] = st;
    }

    if (out.hasPower && out.numSteps < MAX_GUIDE_STEPS) {
        memset(&st, 0, sizeof(st));
        st.partIdx = -1;
        st.n1 = st.n2 = st.target = -1;
        st.timeoutMs = 1500;
        st.onFail = GuideOnFail::WARN;
        st.type = GuideStepType::POWER_ON;
        st.check = GuideCheck::RAIL_SANE;   // §5.2's power_on row
        snprintf(st.text, sizeof(st.text), "Confirm to power up");
        out.steps[out.numSteps++] = st;
    }
}

bool guideParse(const char* yamlPath, GuideScript& out, String& err) {
    memset(&out, 0, sizeof(out));
    strncpy(out.sourcePath, yamlPath, sizeof(out.sourcePath) - 1);

    // STREAMED line-by-line (the readProjectMeta idiom): a whole-file malloc
    // big enough for any project wiring (32 KB) does not reliably exist on
    // the live heap - the first bench run died right here with a failed
    // malloc. One String per line is all this parser ever holds.
    File f = safeFileOpen(yamlPath, "r");
    if (!f) {
        err = "guideParse: cannot read " + String(yamlPath);
        return false;
    }

    bool inGuide = false;
    bool inSteps = false;
    bool inPower = false;
    bool autoFlag = false;
    bool capWarned = false;
    int  explicitSteps = 0;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.replace("\r", "");

        bool indented = (line.length() > 0 &&
                         (line.charAt(0) == ' ' || line.charAt(0) == '\t'));
        line.trim();

        if (line.length() == 0 || line.startsWith("#")) continue;

        if (!indented) {
            // Top-level section routing (same indent-hardening rule as
            // fromYAML: headers count on un-indented lines only).
            inGuide = line.startsWith("guide:");
            inPower = line.startsWith("power:");
            inSteps = false;
            continue;
        }

        if (inPower) {
            int colon = line.indexOf(':');
            if (colon > 0) {
                String key = line.substring(0, colon);
                key.trim();
                String val = guideScalar(line.substring(colon + 1));
                if (key == "topRail")         { out.topRail = val.toFloat();    out.hasPower = true; }
                else if (key == "bottomRail") { out.bottomRail = val.toFloat(); out.hasPower = true; }
                else if (key == "dac0")       { out.dac0 = val.toFloat();       out.hasPower = true; }
                else if (key == "dac1")       { out.dac1 = val.toFloat();       out.hasPower = true; }
            }
            continue;
        }

        if (!inGuide) continue;

        if (line.startsWith("- ")) {
            if (!inSteps) continue;   // stray list item outside steps:
            String body = line.substring(2);
            body.trim();

            // CONTINUATION LINES (task 8 discovery, fixed here). A step whose
            // flow map wrapped onto the next line used to SWALLOW the step
            // after it: the wrapped tail fell through to the key:value arm
            // below, which sets inSteps = false, and the next `- ` item was
            // then dropped by the guard above - two authored steps parsed as
            // ONE, with no diagnostic at all. Join the tail on instead.
            //
            // Every single-line step leaves depth 0 on the first test, so this
            // loop never runs for one and no existing file changes behaviour.
            // BOUNDED on purpose: this parser is streamed line-by-line because
            // a whole-file String killed the heap on the bench, so an unclosed
            // brace must not be allowed to accumulate the rest of the file.
            const int  MAX_CONT_LINES = 4;
            const unsigned int MAX_STEP_CHARS = 512;
            int joined = 0;
            while (guideFlowMapUnclosed(body) && f.available() &&
                   joined < MAX_CONT_LINES && body.length() < MAX_STEP_CHARS) {
                String cont = f.readStringUntil('\n');
                cont.replace("\r", "");
                cont.trim();
                joined++;
                if (cont.length() == 0 || cont.startsWith("#")) continue;
                body += " ";
                body += cont;
            }
            if (guideFlowMapUnclosed(body)) {
                err += "guide step " + String(out.numSteps + 1) +
                       ": flow map never closes (truncated after " +
                       String(joined) + " continuation line(s)); ";
            }

            if (out.numSteps >= MAX_GUIDE_STEPS) {
                if (!capWarned) {
                    err += "guide: more than " + String(MAX_GUIDE_STEPS) +
                           " steps - extras ignored; ";
                    capWarned = true;
                }
                continue;
            }
            guideParseStepLine(body, out, err);
            explicitSteps++;
            continue;
        }

        int colon = line.indexOf(':');
        if (colon <= 0) continue;
        String key = line.substring(0, colon);
        key.trim();
        String rest = line.substring(colon + 1);
        rest.trim();
        if (key == "title") {
            String t = guideScalar(rest);
            strncpy(out.title, t.c_str(), sizeof(out.title) - 1);
            inSteps = false;
        } else if (key == "auto") {
            bool ok;
            bool bval = parseBoolean(guideScalar(rest), ok);
            if (ok) autoFlag = bval;
            inSteps = false;
        } else if (key == "steps") {
            inSteps = true;
        } else {
            // Inside steps:, a line that is neither a `- ` item nor a
            // guide-level key is a BLOCK-style step body ("- id: x" then
            // "  do: place"), which this parser does not support. The
            // flow-map wrap is joined above; this is the residual, and it
            // is worth saying out loud because inSteps goes false here and
            // the step that follows is then dropped in silence.
            if (inSteps) {
                err += "guide step spans lines - unsupported, next step may "
                       "be lost (at '" + key + ":'); ";
            }
            inSteps = false;   // unknown guide: scalar - ignore
        }
    }
    safeFileClose(f, false);

    // Synthesis rule: authored steps always win. `auto: true` alongside a
    // steps: list is contradictory - warn, keep the authored steps.
    if (out.numSteps == 0 && (autoFlag || globalState.parts.numParts > 0)) {
        guideSynthesizeSteps(out);
    } else if (autoFlag && explicitSteps > 0) {
        err += "guide: both auto: true and steps: present - steps: wins; ";
    }

    if (out.title[0] == '\0') {
        strncpy(out.title, "Guided build", sizeof(out.title) - 1);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// NEXT/PREV are the WHEEL's own keys. They are deliberately NOT SKIP/BACK any
// more (design §3.1): a wheel turn navigates and nothing else - it never
// skips, never un-commits, and CAN NEVER EXIT THE GUIDE. Only hold and `q`
// leave. `s` and `p`/probe-REMOVE keep their teeth.
enum class GuideKey : uint8_t { NONE, CONFIRM, SKIP, BACK, VERIFY, QUIT, TAP,
                                MOVE, SNAP, NEXT, PREV };

// `t <row>` / `m <row>`: digits until newline or 300 ms of silence. Shared by
// both keys so the two number readers can never drift.
static int guideReadRowArg(const char* what) {
    String num;
    unsigned long last = millis();
    while (millis() - last < 300) {
        if (Serial.available() > 0) {
            char d = Serial.read();
            last = millis();
            if (d >= '0' && d <= '9') { num += d; continue; }
            if (d == ' ' && num.length() == 0) continue;
            break;   // newline / anything else ends the number
        }
        jOS.serviceInner();
        delayMicroseconds(200);
    }
    int row = num.toInt();
    if (num.length() > 0 && row >= 1 && row <= 60) return row;
    Serial.print("\r\n  ");
    Serial.print(what);
    Serial.println(" <row>: row must be 1-60");
    return -1;
}

// One input event per tick. Serial keys (port 1): n/space=confirm-next,
// p=back, s=skip, v=verify, q=quit, `t <row>`=probe-tap override,
// `m <row>`=move the current place-part's pin 1, c=snap compact/expanded.
// \r, \n and unmapped bytes are explicitly ignored - the launch command
// line's terminator and the port-priming newline land in this loop's lap, and
// a stray byte must never advance a step (it cancels PICKERS by convention,
// but the guide owns its keys - the picker handoff rule from the brief).
//
// m and c are the HEADLESS TWINS of the probe pads (Kevin's control-surface
// principle: the pads are absolute - tap a hole and it acts THERE - so the
// serial forms are absolute too, a row number and a mode toggle, never a
// relative nudge). `tapRow` carries the row for TAP and MOVE alike.
//
// Takes no session: with the confirm pend gone there is no per-session input
// state left to carry, so reading a key is a pure function of the hardware.
static GuideKey guideReadInput(int& tapRow) {
    tapRow = -1;
    rotaryEncoderButtonStuff();

    // Encoder: hold = quit, click = confirm (INSTANT again), wheel = browse
    // next / prev. There is no double-click gesture any more, so nothing has
    // to be disambiguated and nothing waits.
    //
    // THE UNCONSUMED DOUBLECLICKED, walked through, because "we just don't
    // read it" is only safe if the driver's own edges stay coherent
    // (RotaryEncoder.cpp is deliberately NOT touched):
    //   press 1   -> PRESSED
    //   release 1 -> lastButtonEncoderState = PRESSED, state = RELEASED, and
    //                the test below fires: ONE confirm, state cleared to IDLE.
    //   press 2   -> inside doubleClickLength the driver sets DOUBLECLICKED
    //                (RotaryEncoder.cpp:590). No arm below matches it; the
    //                guide does nothing.
    //   release 2 -> the driver copies DOUBLECLICKED into
    //                lastButtonEncoderState and sets RELEASED. The test below
    //                demands lastButtonEncoderState == PRESSED, so the second
    //                click is SWALLOWED, not confirmed twice.
    // Net: a double-click on the wheel is exactly one confirm. A click that
    // turns into a hold still reaches HELD and still quits.
    if (encoderButtonState == HELD) {
        encoderButtonState = IDLE;
        return GuideKey::QUIT;
    }
    if (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) {
        encoderButtonState = IDLE;
        return GuideKey::CONFIRM;
    }
    if (encoderDirectionState == UP) {
        encoderDirectionState = NONE;
        return GuideKey::NEXT;
    }
    if (encoderDirectionState == DOWN) {
        encoderDirectionState = NONE;
        return GuideKey::PREV;
    }

    // Probe buttons (post-polarity-swap): CONNECT = confirm, REMOVE = back.
    int pb = guideProbeButton();
    if (pb == 1) return GuideKey::CONFIRM;
    if (pb == 2) return GuideKey::BACK;

    if (Serial.available() > 0) {
        char c = Serial.read();
        switch (c) {
            case 'n': case ' ': return GuideKey::CONFIRM;
            case 'p':           return GuideKey::BACK;
            case 's':           return GuideKey::SKIP;
            case 'v':           return GuideKey::VERIFY;
            case 'q':           return GuideKey::QUIT;
            case 'c':           return GuideKey::SNAP;
            // The WHEEL's serial twins (§7.1). The wheel is relative, so its
            // twins are relative too - and like the wheel they only ever
            // browse: `>` past the last step lands in the DONE view, `>`
            // again wraps to step 1, and neither can exit.
            case '>':           return GuideKey::NEXT;
            case '<':           return GuideKey::PREV;
            case 't': {
                int row = guideReadRowArg("t");
                if (row < 0) return GuideKey::NONE;
                tapRow = row;
                return GuideKey::TAP;
            }
            case 'm': {
                int row = guideReadRowArg("m");
                if (row < 0) return GuideKey::NONE;
                tapRow = row;
                return GuideKey::MOVE;
            }
            default:
                return GuideKey::NONE;   // \r \n and unmapped: ignored
        }
    }

    // Probe tap (deduped): the same path `t <row>` feeds.
    int probeRow = justReadProbe();
    if (probeRow >= 1 && probeRow <= 60) {
        tapRow = probeRow;
        return GuideKey::TAP;
    }

    return GuideKey::NONE;
}

// ---------------------------------------------------------------------------
// Prompt rendering (OLED + terminal mirror)
// ---------------------------------------------------------------------------

// OLED hotplug poll (probeCalibApp's pattern): reconnect attempt at most
// once a second; returns true when a fresh connection came up.
static bool guideOledHotplugPoll(void) {
    static unsigned long lastPoll = 0;
    if (oled.isConnected() || millis() - lastPoll < 1000) return false;
    lastPoll = millis();
    if (!autoDetectAndConfigureOled()) return false;
    oled.init();
    return oled.isConnected();
}

static void guideShowPrompt(const GuideSession& s) {
    const GuideStep& st = s.script->steps[s.stepIdx];

    // Terminal mirror (the only feedback with no OLED).
    cycleTerminalColor(false, 100.0, true, &Serial);
    Serial.print("\r\n[");
    Serial.print(s.stepIdx + 1);
    Serial.print("/");
    Serial.print(s.script->numSteps);
    Serial.print("] ");
    Serial.print(st.text[0] ? st.text : guideTypeName(st.type));
    if (st.type == GuideStepType::CONNECT) {
        Serial.print("  (");
        printNodeOrName(st.n1, 0, -1, &Serial);
        Serial.print(" -> ");
        printNodeOrName(st.n2, 0, -1, &Serial);
        Serial.print(")");
        if (st.probeConfirm) Serial.print("  [tap a target row to confirm]");
    }
    Serial.println();
    Serial.flush();

    if (oled.isConnected()) {
        char head[128];
        snprintf(head, sizeof(head), "%d/%d %s", s.stepIdx + 1, s.script->numSteps,
                 st.text[0] ? st.text : guideTypeName(st.type));
        oled.showMultiLineSmallText(head, true, true);
    }
}

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The step ring (design §3): numSteps + 1 positions, DONE sitting at index
// numSteps. Browsing is EPHEMERAL - the cursor is not progress.
// ---------------------------------------------------------------------------

// Lowest step that is neither committed nor skipped, or numSteps when none.
// THIS, not the browse cursor, is what resume means (§3.2): out-of-order
// commits must not change where the next launch picks up.
static int guideFirstUnfinished(const GuideSession& s) {
    for (int i = 0; i < s.script->numSteps && i < MAX_GUIDE_STEPS; i++) {
        if (!s.committed[i] && !s.skipped[i]) return i;
    }
    return s.script->numSteps;
}

// Lowest step that is not COMMITTED - a skipped step counts as unfinished
// here. The DONE view's confirm uses this ("jump to the first thing you have
// not actually built"), while persistence uses guideFirstUnfinished, where a
// deliberate skip is a decision the resume must respect.
static int guideFirstIncomplete(const GuideSession& s) {
    for (int i = 0; i < s.script->numSteps && i < MAX_GUIDE_STEPS; i++) {
        if (!s.committed[i]) return i;
    }
    return s.script->numSteps;
}

// Next unfinished step after `from`, wrapping; numSteps when none is left.
// The post-commit cursor rule (§3.1): after a jumped-back commit, stepIdx + 1
// would land on an already-committed step's WAIT.
static int guideNextUnfinished(const GuideSession& s, int from) {
    int n = s.script->numSteps;
    if (n <= 0) return 0;
    for (int d = 1; d <= n; d++) {
        int i = (from + d) % n;
        if (!s.committed[i] && !s.skipped[i]) return i;
    }
    return n;
}

static void guidePersistProgress(GuideSession& s) {
    strncpy(globalState.parts.guideSource, s.script->sourcePath,
            sizeof(globalState.parts.guideSource) - 1);
    globalState.parts.guideSource[sizeof(globalState.parts.guideSource) - 1] = '\0';
    // NOT the cursor (§3.2): the browse position is ephemeral, so what gets
    // written is the first genuinely unfinished step. Resume lands right no
    // matter how far the wheel wandered.
    globalState.parts.guideStep = (int16_t)guideFirstUnfinished(s);
    // The step TOTAL goes with it (`of:` in the flow map). This is the only
    // place that knows numSteps AND writes the file, and the single-run-file
    // launcher needs "step < of" answerable from the FILE ALONE - it decides
    // whether to prompt before it is allowed to load anything. See the
    // guideProgress format note in States.h.
    globalState.parts.guideTotal = (int16_t)s.script->numSteps;
    globalState.markDirty();
    String serr;
    if (!SlotManager::getInstance().saveActiveSlot(serr, /*skipValidation=*/true)) {
        Serial.println("\r\n  (progress save failed: " + serr + ")");
    }
}

// ---------------------------------------------------------------------------
// Moving and snapping a placed part (guide-UX design §1.1-§1.5, §2.2-§2.4)
// ---------------------------------------------------------------------------

static const char* guidePlacementName(uint8_t pl) {
    if (pl == PART_PLACEMENT_COMPACT) return "compact";
    if (pl == PART_PLACEMENT_CUSTOM)  return "custom";
    return "expanded";
}

// Candidate geometry is evaluated on a COPY of the part with the proposed
// row/mode - never on the live entry, because the live one is still expanded
// on the fabric until the remove→mutate→reapply sequence below runs.
// static: a PartDefinition is ~500 B and the guide is a modal, single-
// threaded app (the same argument deserializeParts makes for its `cur`).
static PartDefinition guideMoveScratch;

// "Free hole" for tap rule 3 (§1.2): a real breadboard row that no PLACED
// part already has a leg in and that carries no net. NOTE this is deliberately
// STRICTER than the move-legality rule below: `m <row>` onto a row that
// carries a net is a legal move (joining an existing net by placement is
// normal breadboarding, §1.5 rule 2), but a TAP there means "tell me what this
// row is" - identify has to keep working on every occupied row, which is the
// only affordance the probe has as a voltmeter.
static bool guideRowIsFree(int row) {
    if (row < 1 || row > 60) return false;
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            if (partPinNode(p, p.pins[j]) == row) return false;
        }
    }
    int netNum = findNodeInNet(row);
    if (netNum > 0 && netNum < MAX_NETS &&
        globalState.connections.nets[netNum].number == netNum) {
        return false;
    }
    return true;
}

// §1.5: is `cand` (a scratch copy already carrying the proposed row + mode) a
// legal place for part `partIdx` to sit? Fills `reason` with the specific
// refusal when not - "off-board", "collides with R3", "dip pin 1 (row:) must
// be on the bottom half (31-60)". A refusal changes nothing at all.
static bool guideMoveLegal(const PartDefinition& cand, int partIdx,
                           char* reason, size_t reasonLen) {
    reason[0] = '\0';

    // Rule 1a: the footprint span itself, plus every listed pin resolving on
    // it - the SAME predicate the parser and place_part() apply, so the guide
    // can never move a part into a shape the next load would drop.
    if (!partGeometryOk(cand, reason, reasonLen)) return false;

    if (cand.placement == PART_PLACEMENT_COMPACT) {
        if (cand.footprint == 1) {
            snprintf(reason, reasonLen,
                     "ICs don't compact - their legs are the footprint");
            return false;
        }
        bool anyEligible = false;
        for (int j = 0; j < cand.numPins && j < MAX_PART_PINS; j++) {
            if (partPinCompactEligible(cand, cand.pins[j])) { anyEligible = true; break; }
        }
        if (!anyEligible) {
            snprintf(reason, reasonLen,
                     "no leg has a hole-row endpoint to sit in");
            return false;
        }
        // Same-row collapse (§2.2): two legs of ONE part in one row is a
        // short through the part - meaningless, so compact is refused rather
        // than silently building it.
        for (int a = 0; a < cand.numPins && a < MAX_PART_PINS; a++) {
            int na = partPinNode(cand, cand.pins[a]);
            if (na < 0) continue;
            for (int b = a + 1; b < cand.numPins && b < MAX_PART_PINS; b++) {
                if (partPinNode(cand, cand.pins[b]) != na) continue;
                snprintf(reason, reasonLen,
                         "legs %s and %s would both land on ",
                         cand.pins[a].name, cand.pins[b].name);
                size_t at = strlen(reason);
                if (at < reasonLen) {
                    snprintf(reason + at, reasonLen - at, "node %d", na);
                }
                return false;
            }
        }
    }

    // Rule 1b: every listed pin resolves in the mode it will actually sit in
    // (partGeometryOk only walked the footprint).
    for (int j = 0; j < cand.numPins && j < MAX_PART_PINS; j++) {
        if (partPinNode(cand, cand.pins[j]) >= 0) continue;
        snprintf(reason, reasonLen, "pin %s lands off-board", cand.pins[j].name);
        return false;
    }

    // Rule 2: no two PARTS' legs may share a 5-hole row when nobody ASKED for
    // that share - it silently merges two nets. Three exemptions, all of them
    // "the share is authored, not accidental":
    //
    //  - a rail: a rail is a long bus and two legs in it is what a rail is
    //    for (the `mine < 1 || mine > 60` skip below);
    //  - the part itself (`i == partIdx`);
    //  - THE ENDPOINT ROW A LEG DECLARES. A compact leg lands, by
    //    construction, on its own `pin.connect` - and in any real project
    //    that row is the leg of the very part it is wired to. Refusing it
    //    killed the photo semantics in the flagship scenario: in the shipped
    //    555, once U1 commits, `c` on R1/R2/R3/C1 every one came back
    //    `collides with U1 at row N`, because "resistor leg straight into the
    //    chip's pin row" IS the gesture. Landing on the row you declare a
    //    connection to is the feature. No merge can be silent there - the
    //    file says the two belong together - and the electrical side is
    //    already right by construction: expandOnePart suppresses the bridge
    //    (node == pin.connect) and removePartPlacement's hasConnection guard
    //    skips it symmetrically. Expanded/custom moves are untouched in
    //    practice: their resolved nodes are footprint rows, not endpoints.
    //
    // Rows that merely carry a net or a bridge were never refused at all -
    // they only get a heads-up (printed by the caller).
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        if (i == partIdx) continue;   // self never collides with itself
        const PartDefinition& other = globalState.parts.parts[i];
        if (!other.placed) continue;
        for (int j = 0; j < cand.numPins && j < MAX_PART_PINS; j++) {
            int mine = partPinNode(cand, cand.pins[j]);
            if (mine < 1 || mine > 60) continue;
            if (cand.pins[j].connect == mine) continue;   // authored share
            for (int k = 0; k < other.numPins && k < MAX_PART_PINS; k++) {
                if (partPinNode(other, other.pins[k]) != mine) continue;
                snprintf(reason, reasonLen, "collides with %s at row %d",
                         other.name, mine);
                return false;
            }
        }
    }
    return true;
}

// An AUTO-SYNTHESIZED place step's prompt spells out the row ("Place R1 (10k),
// pin 1 at row 45") and that text is baked at parse time, so a move leaves it
// lying. We wrote that sentence, so we can rewrite it; authored prose we
// cannot, and the live "now: ..." line at the end of guideMovePart covers
// both. Format kept in lockstep with guideSynthesizeSteps.
static void guideRefreshPlaceStepText(GuideScript& sc, int stepIdx, int partIdx) {
    if (!sc.autoFromParts) return;
    if (stepIdx < 0 || stepIdx >= sc.numSteps) return;
    GuideStep& st = sc.steps[stepIdx];
    if (st.type != GuideStepType::PLACE || st.partIdx != partIdx) return;
    if (partIdx < 0 || partIdx >= globalState.parts.numParts) return;
    const PartDefinition& p = globalState.parts.parts[partIdx];
    if (p.value[0] != '\0') {
        snprintf(st.text, sizeof(st.text), "Place %s (%s), pin 1 at row %d",
                 p.name, p.value, p.baseRow);
    } else {
        snprintf(st.text, sizeof(st.text), "Place %s, pin 1 at row %d",
                 p.name, p.baseRow);
    }
}

// THE mutation sequence (design §1.1). Every applied move or snap goes
// through here, from the tap pads, the wheel (task 7) and the serial twins
// alike, and emits exactly one machine line.
//
// INVARIANT: never flip p.placement or p.baseRow while p.placed == true
// except through this remove → mutate → reapply order. removePartPlacement
// recomputes its bridge endpoints from partPinNode, so it MUST run against
// the geometry that applied them - mutating first strands every old bridge on
// the fabric with nothing left that knows how to find it.
//
// This is also the whole answer to "how do connect: bridges re-derive after a
// move": they are never stored per part. expandOnePart recomputes
// partPinNode against pin.connect at apply time, so remove → mutate → reapply
// IS the re-derivation.
//
// Every move persists immediately. The batched (`persist == false`) variant
// existed for exactly one caller - the wheel slide, which fired a move per
// detent - and died with the ADJUST mode: a tap or an `m <row>` is a single
// deliberate placement, not a stream, so one YAML write per gesture is the
// right cost and the run file is never left describing a stale row.
static bool guideMovePart(GuideSession& s, int partIdx, int newRow,
                          uint8_t newPlacement) {
    if (partIdx < 0 || partIdx >= globalState.parts.numParts) return false;
    PartDefinition& p = globalState.parts.parts[partIdx];
    if (newRow == p.baseRow && newPlacement == p.placement) {
        Serial.print("\r\n  (");
        Serial.print(p.name);
        Serial.print(" is already ");
        Serial.print(guidePlacementName(newPlacement));
        Serial.print(" at row ");
        Serial.print(newRow);
        Serial.println(")");
        return false;
    }

    guideMoveScratch = p;
    guideMoveScratch.baseRow = (int16_t)newRow;
    guideMoveScratch.placement = newPlacement;

    char reason[128];
    if (!guideMoveLegal(guideMoveScratch, partIdx, reason, sizeof(reason))) {
        Serial.print("\r\n  move refused: ");
        Serial.println(reason);
        Serial.flush();
        return false;
    }

    // Everything that must be read BEFORE the fabric changes: the rows this
    // part is about to leave (for the staleness scan) and the nets the new
    // rows already belong to (for the heads-up, which would otherwise report
    // the part's own freshly-merged net back to itself).
    int vacated[MAX_PART_PINS];
    int numVacated = 0;
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        int node = partPinNode(p, p.pins[j]);
        if (node >= 1 && node <= 60) vacated[numVacated++] = node;
    }
    static char joinNote[4][64];
    int numJoins = 0;
    for (int j = 0; j < guideMoveScratch.numPins && j < MAX_PART_PINS &&
                    numJoins < 4; j++) {
        int node = partPinNode(guideMoveScratch, guideMoveScratch.pins[j]);
        if (node < 1 || node > 60) continue;
        // Not news if the part is already there, and not news if the net it
        // "joins" is the one its OWN legs are currently making - snapping a
        // committed part to compact moves a leg into a row its own bridge
        // already reached, which is the whole point, not a surprise.
        bool wasMine = false;
        int netHere = findNodeInNet(node);
        for (int v = 0; v < numVacated; v++) {
            if (vacated[v] == node) wasMine = true;
            if (netHere > 0 && findNodeInNet(vacated[v]) == netHere) wasMine = true;
        }
        if (wasMine) continue;
        char nameBuf[16];
        const char* netName = guideNetNameForNode(node, nameBuf, sizeof(nameBuf));
        if (strcmp(netName, "(no net)") == 0) continue;
        snprintf(joinNote[numJoins++], sizeof(joinNote[0]),
                 "  (row %d joins net %s)", node, netName);
    }

    bool wasPlaced = p.placed;
    String err;
    if (wasPlaced) removePartPlacement(globalState, partIdx, err);
    p.baseRow = (int16_t)newRow;
    p.placement = newPlacement;
    if (wasPlaced) {
        applyPartPlacement(globalState, partIdx, err);
        refreshConnections(1);
        guideRenderFootprints();
    }
    if (err.length() > 0) {
        Serial.println("\r\n  (move warnings: " + err + ")");
    }
    globalState.markDirty();
    guidePersistProgress(s);
    guideRenderTarget(s, s.pulseOn);
    guideShowLeds();

    Serial.print("\r\nGUIDE move part=");
    Serial.print(p.name);
    Serial.print(" row=");
    Serial.print(p.baseRow);
    Serial.print(" placement=");
    Serial.println(guidePlacementName(p.placement));
    for (int i = 0; i < numJoins; i++) Serial.println(joinNote[i]);

    // Staleness scan (§1.1 caveat): authored `connect:` steps carry LITERAL
    // rows and do not re-derive, so one aimed at a row this part just left is
    // now pointing at an empty hole. Cheap to notice, impossible to guess at
    // later.
    for (int v = 0; v < numVacated; v++) {
        int node = vacated[v];
        bool stillMine = false;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            if (partPinNode(p, p.pins[j]) == node) { stillMine = true; break; }
            // A `connect:` row is STILL COVERED even when no leg sits in it:
            // the leg that left brought its routed bridge back to exactly that
            // endpoint. Without this, every compact -> expanded snap flagged
            // the endpoint row it had been sitting in as "vacated" while the
            // returning bridge still reached it (T6 review, nit 3).
            if (p.pins[j].connect == node) { stillMine = true; break; }
        }
        if (stillMine) continue;
        for (int i = s.stepIdx; i < s.script->numSteps; i++) {
            const GuideStep& st = s.script->steps[i];
            if (st.type != GuideStepType::CONNECT) continue;
            if (st.n1 != node && st.n2 != node) continue;
            Serial.print("  (note: step ");
            Serial.print(i + 1);
            Serial.print(" targets row ");
            Serial.print(node);
            Serial.print(", which ");
            Serial.print(p.name);
            Serial.println(" just left)");
        }
    }
    Serial.flush();

    // The prompt was baked at STEP_ENTER and still names the OLD row (T6
    // review). Refresh the text where we OWN it (auto-synthesized steps are
    // the ones that spell out "pin 1 at row N"), then re-render prompt + OLED
    // so the terminal and the panel agree with the part's new geometry.
    // Authored prose is left alone - we cannot rewrite someone's sentence -
    // which is why the live line below prints either way.
    if (s.stepIdx >= 0 && s.stepIdx < s.script->numSteps) {
        guideRefreshPlaceStepText(*s.script, s.stepIdx, partIdx);
        guideShowPrompt(s);
    }
    Serial.print("  now: ");
    Serial.print(p.name);
    Serial.print(" pin 1 at row ");
    Serial.print(p.baseRow);
    Serial.print(" (");
    Serial.print(guidePlacementName(p.placement));
    Serial.println(")");
    Serial.flush();
    return true;
}

// The snap gesture (§2.4): compact <-> expanded for one part. Leaving compact
// always lands on EXPANDED - `custom` marks a row the user chose, and the row
// is preserved across the round trip either way, so nothing is lost that the
// file does not still say.
static void guideSnapPart(GuideSession& s, int partIdx) {
    if (partIdx < 0 || partIdx >= globalState.parts.numParts) return;
    const PartDefinition& p = globalState.parts.parts[partIdx];
    uint8_t next = (p.placement == PART_PLACEMENT_COMPACT) ? PART_PLACEMENT_EXPANDED
                                                           : PART_PLACEMENT_COMPACT;
    guideMovePart(s, partIdx, p.baseRow, next);
}

// A move never silently flips compact<->expanded (that is the snap gesture's
// job, §1.4): a compact part stays compact - only its footprint-fallback legs
// travel with baseRow - and an expanded one becomes CUSTOM, the marker that
// says "the user put it here deliberately, don't snap it back". Moving a
// custom part to yet another row keeps it custom.
static void guideMovePartToRow(GuideSession& s, int partIdx, int newRow) {
    if (partIdx < 0 || partIdx >= globalState.parts.numParts) return;
    const PartDefinition& p = globalState.parts.parts[partIdx];
    uint8_t mode = p.placement;
    if (mode != PART_PLACEMENT_COMPACT && newRow != p.baseRow) {
        mode = PART_PLACEMENT_CUSTOM;
    }
    guideMovePart(s, partIdx, newRow, mode);
}

// The current step's part index when it is a PLACE step, else -1 (with the
// refusal note the m/c keys and the tap rules share).
static int guidePlaceStepPart(const GuideSession& s, const char* what) {
    const GuideStep& st = s.script->steps[s.stepIdx];
    if (st.type == GuideStepType::PLACE && st.partIdx >= 0 &&
        st.partIdx < globalState.parts.numParts) {
        return st.partIdx;
    }
    if (what != nullptr) {
        Serial.print("\r\n  (");
        Serial.print(what);
        Serial.println(" works on place steps only)");
        Serial.flush();
    }
    return -1;
}

// The one-line history note a wait-state arrival prints, so a confirm on a
// step you browsed back to is never a surprise.
static void guideShowStepHistoryNote(const GuideSession& s) {
    if (s.stepIdx < 0 || s.stepIdx >= s.script->numSteps) return;
    if (s.committed[s.stepIdx]) {
        Serial.println("  (already committed - click re-verifies)");
    } else if (s.skipped[s.stepIdx]) {
        Serial.println("  (skipped)");
    }
}

// ---------------------------------------------------------------------------
// What the RESULT screen is ABOUT (design §5)
// ---------------------------------------------------------------------------

// The header line of the reading: the part for a place step ("R1 10k"), the
// check's own name otherwise ("continuity").
static void guideResultHeadName(const GuideScript& sc, int stepIdx,
                                char* out, size_t outLen) {
    const GuideStep& st = sc.steps[stepIdx];
    if (st.type == GuideStepType::PLACE && st.partIdx >= 0 &&
        st.partIdx < globalState.parts.numParts) {
        const PartDefinition& p = globalState.parts.parts[st.partIdx];
        if (p.value[0] != '\0') {
            snprintf(out, outLen, "%s %s", p.name, p.value);
        } else {
            snprintf(out, outLen, "%s", p.name);
        }
        return;
    }
    snprintf(out, outLen, "%s", guideCheckName(st.check));
}

// The node label in the reading's top-right corner: the step's declared
// target, else the first row the step's part actually occupies, else a connect
// endpoint, else nothing (show() renders name-only when rowNode <= 0).
static int guideResultPrimaryRow(const GuideScript& sc, int stepIdx) {
    const GuideStep& st = sc.steps[stepIdx];
    if (st.target > 0) return st.target;
    if (st.type == GuideStepType::PLACE && st.partIdx >= 0 &&
        st.partIdx < globalState.parts.numParts) {
        const PartDefinition& p = globalState.parts.parts[st.partIdx];
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int node = partPinNode(p, p.pins[j]);
            if (node > 0) return node;
        }
    }
    if (st.n1 > 0) return st.n1;
    return -1;
}

// The wheel's whole job (design §3.1). PURE NAVIGATION: no skip flag, no
// un-commit, no persistence - it only moves the cursor around a ring of
// numSteps + 1 positions with the DONE view at index numSteps, wrapping at
// BOTH ends. THE WHEEL CAN NEVER EXIT THE GUIDE; hold and `q` are the only
// ways out, from anywhere. `>` and `<` are its serial twins.
static void guideBrowse(GuideSession& s, int dir) {
    int n = s.script->numSteps;
    int next = s.stepIdx + dir;
    if (next > n) next = 0;        // past the DONE view -> back to step 1
    if (next < 0) next = n;        // before step 1 -> into the DONE view
    s.stepIdx = next;
    // Any landing re-arms the summary: browsing OUT of DONE must let the next
    // arrival print it again (today only STEP_BACK cleared this).
    s.summaryShown = false;
    s.state = (s.stepIdx >= n) ? GuideState::DONE : GuideState::STEP_ENTER;
}

// Skip-with-flag (§3.3): shared by the wait states' skip key, a mid-check
// skip (which aborts the check first at its call site), and on_fail: skip.
// Returns false when the skip was REFUSED and the caller must decide where to
// go instead (nothing has been changed in that case).
static bool guideDoSkip(GuideSession& s) {
    // RULING (fix round 1): a COMMITTED step cannot be skipped. Clearing
    // committed[i] while the bridges that commit built are still on the fabric
    // is a session/fabric divergence - the flag would say "never done" about
    // hardware that is done. Only became reachable when browsing let the
    // cursor park on a committed step. `p` / probe REMOVE is the gesture that
    // un-commits, and it removes the hardware too.
    if (s.committed[s.stepIdx]) {
        Serial.println("\r\n  (already committed - p removes it first)");
        Serial.flush();
        return false;
    }
    s.skipped[s.stepIdx] = true;
    s.committed[s.stepIdx] = false;
    Serial.println("\r\n  (skipped)");
    s.stepIdx++;
    guidePersistProgress(s);
    s.state = (s.stepIdx >= s.script->numSteps) ? GuideState::DONE
                                                : GuideState::STEP_ENTER;
    return true;
}

void guideTick(GuideSession& s) {
    GuideScript& sc = *s.script;

    switch (s.state) {

        case GuideState::INIT: {
            // Banner + safe state. Rails/DACs to 0 V until power_on (§3.2);
            // remembered file values applied by the POWER_ON commit.
            b.clear();                       // scrub any menu text (no-op OG)
            guideClearOverlays();
            guideForcePowerSafe();
            ReadingDisplay::resetLastShown();

            cycleTerminalColor(true, 5.0, true, &Serial);
            Serial.print("\r\n=== Guided build: ");
            Serial.print(sc.title);
            Serial.print(" === (");
            Serial.print(sc.numSteps);
            Serial.println(" steps)");
            cycleTerminalColor(false, 100.0, true, &Serial);
            // The banner names the GESTURES, not the whole key list: the
            // serial twins (n/p/s/v/m/c/t/>/<) all still work and HelpDocs
            // still documents them, but a bench user with a wheel and a probe
            // should read one short line, not a cheat sheet (Kevin: "this
            // whole thing is confusing, we need to streamline this interface").
            Serial.println("wheel=browse  click=confirm  hold/q=quit  "
                           "probe: tap=move/snap/identify");
            Serial.println("rails + DACs held at 0V until the power_on step");
            Serial.flush();
            guideStatusLine(s, "INIT");

            // Resume bookkeeping: steps before the resume point count as
            // committed (the slot was saved at each commit). If a power_on
            // step is already behind us, the circuit was running when the
            // user left - re-apply the file's power so resume means resume.
            for (int i = 0; i < s.stepIdx && i < sc.numSteps; i++) {
                s.committed[i] = true;
                if (sc.steps[i].type == GuideStepType::POWER_ON) {
                    // Mirror of the COMMIT path: only claim powerApplied when
                    // the file actually has power to apply. (Was asymmetric -
                    // an unconditional powerApplied=true here suppressed the
                    // exit tail's "rails at 0V" note for a project with no
                    // power: section, even though nothing was ever applied.)
                    if (sc.hasPower) {
                        setTopRail(sc.topRail, 1, 0);
                        setBotRail(sc.bottomRail, 1, 0);
                        setDac0voltage(sc.dac0, 1, 0);
                        setDac1voltage(sc.dac1, 1, 0);
                        s.powerApplied = true;
                        Serial.println("  (resume past power_on: rails re-applied)");
                    } else {
                        Serial.println("  (resume past power_on: project has no power: section - rails stay at 0V)");
                    }
                }
            }

            guideRenderFootprints();
            guideShowLeds();

            if (sc.numSteps == 0 || s.stepIdx >= sc.numSteps) {
                s.state = GuideState::DONE;
            } else {
                s.state = GuideState::STEP_ENTER;
            }
            break;
        }

        case GuideState::STEP_ENTER: {
            const GuideStep& st = sc.steps[s.stepIdx];
            s.checkWarned = false;   // a fresh step arrival re-arms its check
            guideShowPrompt(s);
            // Browsing means landing on steps with history - say so, so a
            // confirm here is never a surprise.
            guideShowStepHistoryNote(s);
            s.pulseOn = true;
            s.lastPulseMs = millis();
            guideRenderTarget(s, s.pulseOn);
            guideShowLeds();
            if (st.probeConfirm) {
                s.state = GuideState::STEP_PROBE_WAIT;
                guideStatusLine(s, "PROBE_WAIT");
            } else {
                s.state = GuideState::STEP_WAIT;
                guideStatusLine(s, "WAIT");
            }
            s.waitReturn = s.state;
            break;
        }

        case GuideState::STEP_WAIT:
        case GuideState::STEP_PROBE_WAIT: {
            // ~2 Hz target pulse (rewrite colors + a clear-first show, §4.1).
            if (millis() - s.lastPulseMs >= 250) {
                s.lastPulseMs = millis();
                s.pulseOn = !s.pulseOn;
                guideRenderTarget(s, s.pulseOn);
                guideShowLeds();
            }
            guideOledHotplugPoll();

            int tapRow = -1;
            GuideKey k = guideReadInput(tapRow);
            switch (k) {
                case GuideKey::QUIT:
                    s.exitRequested = true;
                    s.state = GuideState::EXIT;
                    break;
                case GuideKey::CONFIRM:
                    // Confirming an ALREADY COMMITTED step re-verifies it and
                    // never re-commits (§3.1): no duplicate bridges, no second
                    // cursor advance. Browsing back to check your work is the
                    // whole point of the ring.
                    s.verifyOnly = s.committed[s.stepIdx];
                    s.state = GuideState::STEP_VERIFY;
                    break;
                case GuideKey::VERIFY:
                    s.verifyOnly = true;
                    s.state = GuideState::STEP_VERIFY;
                    break;
                case GuideKey::BACK:
                    s.state = GuideState::STEP_BACK;
                    break;
                case GuideKey::SKIP:
                    // Explicit skip-with-flag (§3.3). Progress moves and
                    // persists so resume lands after the skip.
                    guideDoSkip(s);
                    break;
                case GuideKey::NEXT:
                    guideBrowse(s, +1);
                    break;
                case GuideKey::PREV:
                    guideBrowse(s, -1);
                    break;
                case GuideKey::MOVE: {
                    // `m <row>` is the LITERAL twin of a tap: no DIP +30
                    // convenience here (that belongs to the pad, which cannot
                    // type), so `m 5` on a DIP earns the honest refusal.
                    int idx = guidePlaceStepPart(s, "m <row>");
                    if (idx >= 0) guideMovePartToRow(s, idx, tapRow);
                    break;
                }
                case GuideKey::SNAP: {
                    int idx = guidePlaceStepPart(s, "c");
                    if (idx >= 0) guideSnapPart(s, idx);
                    break;
                }
                case GuideKey::TAP: {
                    // Tap precedence (§1.2), in this exact order:
                    //   1 probe_confirm match  2 the part's own lit footprint
                    //   3 a free hole          4 identify (the fallback)
                    const GuideStep& st = sc.steps[s.stepIdx];
                    if (s.state == GuideState::STEP_PROBE_WAIT &&
                        (tapRow == st.n1 || tapRow == st.n2)) {
                        Serial.print("\r\n  tap on ");
                        printNodeOrName(tapRow, 0, -1, &Serial);
                        Serial.println(" - confirmed");
                        s.verifyOnly = false;
                        s.state = GuideState::STEP_VERIFY;
                        break;
                    }
                    int idx = guidePlaceStepPart(s, nullptr);   // silent: rules 2-3
                    if (idx >= 0) {
                        const PartDefinition& p = globalState.parts.parts[idx];
                        // 2. The footprint is what `_GUIDE_TGT_` is lighting
                        // up, so "tap the thing that's glowing" flips how it
                        // sits.
                        bool onFootprint = false;
                        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                            if (partPinNode(p, p.pins[j]) == tapRow) { onFootprint = true; break; }
                        }
                        if (onFootprint) {
                            guideSnapPart(s, idx);
                            break;
                        }
                        // 3. A free hole means "put pin 1 HERE". DIP
                        // convenience: pin 1 lives on the bottom half, so a
                        // top-half tap means the same column across the
                        // ravine.
                        if (guideRowIsFree(tapRow)) {
                            int want = tapRow;
                            if (p.footprint == 1 && want <= 30) want += 30;
                            guideMovePartToRow(s, idx, want);
                            break;
                        }
                    }
                    guideIdentifyRow(tapRow);   // 4. identify mode (§3.3)
                    break;
                }
                default:
                    break;
            }
            break;
        }

        case GuideState::STEP_VERIFY: {
            const GuideStep& st = sc.steps[s.stepIdx];
            if (st.check == GuideCheck::NONE) {
                s.checkOutcome = GUIDE_CHECK_PASS;
                strncpy(s.checkVal, "pass", sizeof(s.checkVal));
                s.state = GuideState::STEP_RESULT;
                break;
            }
            if (!s.checkRunning) {
                if (s.checkWarned && !s.verifyOnly) {
                    // Warn-armed manual advance: the user saw the X and
                    // confirmed again - commit without re-running the check.
                    Serial.println("\r\n  (advancing past the failed check)");
                    s.state = GuideState::STEP_COMMIT;
                    break;
                }
                // Status line BEFORE begin: the i2c check blocks inside its
                // begin (documented reuse), and its own output must land
                // after the line that says a check started.
                char extra[24];
                snprintf(extra, sizeof(extra), "check=%s", guideCheckName(st.check));
                guideStatusLine(s, "VERIFY", extra);
                GuideCheckRun run;
                run.step = &st;
                run.script = &sc;
                run.powerApplied = s.powerApplied;
                guideCheckBegin(run);
                s.checkRunning = true;
                break;
            }
            int r = guideCheckPoll(s.checkVal, sizeof(s.checkVal));
            if (r == GUIDE_CHECK_RUNNING) {
                // Mid-check the user keeps control: quit / skip / back all
                // abort with guaranteed teardown; other inputs are ignored
                // (a queued double-confirm must not act on the next step).
                int tapRow = -1;
                GuideKey k = guideReadInput(tapRow);
                if (k == GuideKey::QUIT) {
                    guideCheckAbort();
                    s.checkRunning = false;
                    s.exitRequested = true;
                    s.state = GuideState::EXIT;
                } else if (k == GuideKey::SKIP) {
                    guideCheckAbort();
                    s.checkRunning = false;
                    // A REFUSED skip must still leave VERIFY: checkRunning is
                    // already false, so staying here would simply re-begin the
                    // check on the next tick, forever.
                    if (!guideDoSkip(s)) s.state = s.waitReturn;
                } else if (k == GuideKey::BACK) {
                    guideCheckAbort();
                    s.checkRunning = false;
                    s.state = GuideState::STEP_BACK;
                }
                break;
            }
            s.checkRunning = false;
            s.checkOutcome = r;
            s.state = GuideState::STEP_RESULT;
            break;
        }

        case GuideState::STEP_RESULT: {
            const GuideStep& st = sc.steps[s.stepIdx];
            bool pass = (s.checkOutcome == GUIDE_CHECK_PASS);
            const char* onFailName =
                (st.onFail == GuideOnFail::RETRY)  ? "retry"
                : (st.onFail == GuideOnFail::SKIP) ? "skip"
                : (st.onFail == GuideOnFail::BLOCK) ? "block" : "warn";
            // 96: check=oscillates + a 23-char val + ok= + on_fail=retry is
            // 63 chars - 64 would truncate the policy token at the edge.
            char extra[96];
            if (pass) {
                snprintf(extra, sizeof(extra), "check=%s val=%s ok=1",
                         guideCheckName(st.check), s.checkVal);
            } else {
                snprintf(extra, sizeof(extra), "check=%s val=%s ok=0 on_fail=%s",
                         guideCheckName(st.check), s.checkVal, onFailName);
            }
            guideStatusLine(s, "RESULT", extra);

            // THE MEASUREMENT, on the panel (design §5). Only for checks that
            // actually measured something: `check: none` synthesises a PASS
            // with val="pass" and never touched the hardware, and
            // SKIPPED/UNSUPPORTED refused to run - a reading screen for any of
            // those would be a lie (and a 900 ms stall on every note step).
            bool measuredResult = (st.check != GuideCheck::NONE) &&
                                  (s.checkOutcome == GUIDE_CHECK_PASS ||
                                   s.checkOutcome == GUIDE_CHECK_FAIL);
            if (measuredResult) {
                char headName[40];
                guideResultHeadName(sc, s.stepIdx, headName, sizeof(headName));
                // resetLastShown FIRST: show() drops repeats by content, and a
                // `v` re-run that measures the same value must still repaint.
                ReadingDisplay::resetLastShown();
                ReadingDisplay::show(headName, guideResultPrimaryRow(sc, s.stepIdx),
                                     s.checkVal, nullptr, pass ? "ok" : "FAIL");
            }

            // THE MEASUREMENT IN WORDS, on PASS AND FAIL (invest-measurement
            // §4: "show what it reads as it's placed", not "show it when it's
            // wrong"). Terminal only - the OLED already got ck.val through
            // ReadingDisplay above, and this line is a sentence, not a token.
            const char* detail = guideCheckDetail();
            if (measuredResult && detail != nullptr && detail[0] != '\0') {
                Serial.print("  ");
                Serial.println(detail);
                Serial.flush();
            }

            const char* hint = guideCheckHint();
            if (!pass && hint != nullptr && hint[0] != '\0') {
                // AFTER the value, deliberately: the OLED ends on the
                // actionable sentence, and the hint slot in show() is sized
                // for a tag like "adjust?", not for 96 chars of prose.
                Serial.print("  ");
                Serial.println(hint);
                Serial.flush();
                if (oled.isConnected()) {
                    oled.showMultiLineSmallText(hint, true, true);
                }
            }

            // On a PASS the machine walks straight into COMMIT -> STEP_ENTER,
            // which repaints the panel with the next prompt inside ~1 ms, so
            // the reading would never be seen. Hold it - interruptibly, the
            // notify-style pump: any guide key breaks out, so an impatient `n`
            // costs nothing. On a fail nothing repaints, so no hold is needed.
            if (measuredResult && pass) {
                unsigned long holdUntil = millis() + 900;
                while ((long)(millis() - holdUntil) < 0) {
                    jOS.serviceInner();
                    int hRow = -1;
                    GuideKey hk = guideReadInput(hRow);
                    if (hk == GuideKey::QUIT) {
                        // A hold/`q` during the display hold means LEAVE, not
                        // "skip the animation" - it must not be swallowed.
                        s.exitRequested = true;
                        s.state = GuideState::EXIT;
                        return;
                    }
                    if (hk != GuideKey::NONE) break;
                    delayMicroseconds(200);
                }
            }

            if (s.verifyOnly) {
                s.verifyOnly = false;
                Serial.println(pass ? "  verify ok (no commit)"
                                    : "  verify failed (no commit)");
                if (pass) s.checkWarned = false;
                s.state = s.waitReturn;
                break;
            }
            if (pass) {
                s.checkWarned = false;
                s.state = GuideState::STEP_COMMIT;
                break;
            }

            // Fail dispatch. "Measured" = the check ran and the physics said
            // no; SKIPPED/UNSUPPORTED never measured anything - warn-class
            // policy proceeds on those with the confirm the user already
            // gave (no double-press for a check that could not run).
            bool measured = (s.checkOutcome == GUIDE_CHECK_FAIL);
            switch (st.onFail) {
                case GuideOnFail::SKIP:
                    // (Unreachable on a committed step - the verify-only
                    // branch above returns first - but never leave RESULT
                    // without a next state.)
                    if (!guideDoSkip(s)) s.state = s.waitReturn;
                    break;
                case GuideOnFail::RETRY:
                    Serial.println("  ✗ check failed - fix it and confirm to re-run (s skips)");
                    s.state = s.waitReturn;
                    break;
                case GuideOnFail::BLOCK:
                    Serial.println("  ✗ check failed - step blocked: fix and confirm to re-run, s skips explicitly");
                    s.state = s.waitReturn;
                    break;
                default: // WARN
                    if (measured) {
                        Serial.println("  ✗ check failed - n advances anyway, v re-runs");
                        s.checkWarned = true;
                        s.state = s.waitReturn;
                    } else {
                        Serial.println("  (check not run - continuing)");
                        s.state = GuideState::STEP_COMMIT;
                    }
                    break;
            }
            break;
        }

        case GuideState::STEP_COMMIT: {
            const GuideStep& st = sc.steps[s.stepIdx];
            String err;
            bool hardwareChanged = false;
            switch (st.type) {
                case GuideStepType::PLACE: {
                    int added = applyPartPlacement(globalState, st.partIdx, err);
                    if (added < 0 || err.length() > 0) {
                        Serial.println("\r\n  (placement warnings: " + err + ")");
                    }
                    hardwareChanged = true;
                    break;
                }
                case GuideStepType::CONNECT:
                    // hasConnection guard: bare addConnection on an existing
                    // bridge bumps its duplicate count (see expandOnePart) -
                    // a back-then-forward cycle must not stack duplicates.
                    if (!globalState.hasConnection(st.n1, st.n2)) {
                        if (!globalState.addConnection(st.n1, st.n2, err)) {
                            Serial.println("\r\n  (connect failed: " + err + ")");
                        }
                    }
                    hardwareChanged = true;
                    break;
                case GuideStepType::POWER_ON:
                    if (sc.hasPower) {
                        setTopRail(sc.topRail, 1, 0);
                        setBotRail(sc.bottomRail, 1, 0);
                        setDac0voltage(sc.dac0, 1, 0);
                        setDac1voltage(sc.dac1, 1, 0);
                        Serial.print("\r\n  power applied: topRail=");
                        Serial.print(sc.topRail);
                        Serial.print("V bottomRail=");
                        Serial.print(sc.bottomRail);
                        Serial.println("V");
                    } else {
                        Serial.println("\r\n  (no power: section in the project - rails stay at 0V)");
                    }
                    // Only claim power is live when something was applied -
                    // the exit tail's "rails left at 0V" note keys off this.
                    s.powerApplied = sc.hasPower;
                    break;
                case GuideStepType::RUN_SCRIPT:
                    // Script execution arrives with the provisioning /
                    // catalog tasks; the step parses + counts today.
                    Serial.print("\r\n  (run step '");
                    Serial.print(st.script);
                    Serial.println("' - script execution lands in a later task, skipped)");
                    break;
                default:
                    break;   // NOTE / VERIFY commit nothing
            }
            if (hardwareChanged) {
                // Net names re-assert inside the refresh (partsReassertNetNames
                // runs after both refresh paths - the as-built parts contract).
                refreshConnections(1);
                guideRenderFootprints();
            }
            s.committed[s.stepIdx] = true;
            s.skipped[s.stepIdx] = false;
            guideStatusLine(s, "COMMIT");   // still names the step we built
            // Post-commit cursor rule (§3.1): the NEXT UNFINISHED step,
            // wrapping - not stepIdx + 1, which after a jumped-back commit
            // drops the user on an already-committed step's WAIT.
            s.stepIdx = guideNextUnfinished(s, s.stepIdx);
            s.summaryShown = false;
            guidePersistProgress(s);
            s.state = (s.stepIdx >= sc.numSteps) ? GuideState::DONE
                                                 : GuideState::STEP_ENTER;
            break;
        }

        case GuideState::STEP_BACK: {
            if (s.stepIdx <= 0) {
                Serial.println("\r\n  (already at the first step)");
                s.state = GuideState::STEP_ENTER;
                break;
            }
            s.stepIdx--;
            s.summaryShown = false;   // backing out of DONE re-arms the summary
            const GuideStep& st = sc.steps[s.stepIdx];
            String err;
            bool hardwareChanged = false;
            switch (st.type) {
                case GuideStepType::PLACE:
                    removePartPlacement(globalState, st.partIdx, err);
                    hardwareChanged = true;
                    break;
                case GuideStepType::CONNECT:
                    if (globalState.hasConnection(st.n1, st.n2)) {
                        globalState.removeConnection(st.n1, st.n2, err);
                    }
                    hardwareChanged = true;
                    break;
                case GuideStepType::POWER_ON:
                    guideForcePowerSafe();
                    s.powerApplied = false;
                    Serial.println("\r\n  power back OFF (rails to 0V)");
                    break;
                default:
                    break;
            }
            (void)err;   // un-commit is best-effort; a missing bridge is fine
            if (hardwareChanged) {
                refreshConnections(1);
                guideRenderFootprints();
            }
            s.committed[s.stepIdx] = false;
            s.skipped[s.stepIdx] = false;
            guidePersistProgress(s);
            guideStatusLine(s, "BACK");
            s.state = GuideState::STEP_ENTER;
            break;
        }

        case GuideState::DONE: {
            // The DONE VIEW (§3.3): a browsable summary, not a doorway. It is
            // reached by committing the last unfinished step OR by wheeling
            // past either end of the ring, and it is left the same three ways
            // - browse out, confirm, or quit.
            if (!s.summaryShown) {
                s.summaryShown = true;
                // (The DONE-entry pend clear that used to open this block -
                // final-fix-wave F1 - is GONE with the pend itself. It closed
                // a race that can no longer be built: a wheel click confirms
                // on its own RELEASED edge now, in whatever state the machine
                // is in at that instant, so nothing can mature HERE 260 ms
                // after the turn that carried the cursor into this view. The
                // headline invariant - A WHEEL TURN CAN NEVER EXIT THE GUIDE -
                // holds by construction instead of by guard.)
                int commits = 0, skips = 0, unfinished = 0;
                for (int i = 0; i < sc.numSteps; i++) {
                    if (s.committed[i]) commits++;
                    else if (s.skipped[i]) skips++;
                    else unfinished++;
                }
                cycleTerminalColor(true, 5.0, true, &Serial);
                Serial.print("\r\n=== ");
                Serial.print(sc.title);
                Serial.print(": ");
                Serial.print(commits);
                Serial.print("/");
                Serial.print(sc.numSteps);
                Serial.print(" steps committed");
                if (skips > 0) {
                    Serial.print(", ");
                    Serial.print(skips);
                    Serial.print(" skipped");
                }
                if (unfinished > 0) {
                    Serial.print(", ");
                    Serial.print(unfinished);
                    Serial.print(" unfinished");
                }
                Serial.println(" ===");
                // Name them - "1 unfinished" with no index is a scavenger
                // hunt on a 20-step build. Capped so a long list cannot bury
                // the summary line itself.
                int listed = 0;
                for (int i = 0; i < sc.numSteps && listed < 8; i++) {
                    if (!s.skipped[i]) continue;
                    char id[32];
                    guideStepId(sc, i, id, sizeof(id));
                    Serial.print("  skipped:    [");
                    Serial.print(i + 1);
                    Serial.print("] ");
                    Serial.println(id);
                    listed++;
                }
                listed = 0;
                for (int i = 0; i < sc.numSteps && listed < 8; i++) {
                    if (s.committed[i] || s.skipped[i]) continue;
                    char id[32];
                    guideStepId(sc, i, id, sizeof(id));
                    Serial.print("  unfinished: [");
                    Serial.print(i + 1);
                    Serial.print("] ");
                    Serial.println(id);
                    listed++;
                }
                bool allBuilt = (guideFirstIncomplete(s) >= sc.numSteps);
                Serial.println(allBuilt
                    ? "wheel=browse  click=finish  hold/q=exit"
                    : "wheel=browse  click=jump to the first unbuilt step  hold/q=exit");
                Serial.flush();
                if (oled.isConnected()) {
                    char head[48];
                    if (allBuilt) {
                        snprintf(head, sizeof(head), "Done! %d/%d", commits, sc.numSteps);
                    } else {
                        snprintf(head, sizeof(head), "Done?\n%d left",
                                 sc.numSteps - commits);
                    }
                    oled.showMultiLineSmallText(head, true, true);
                }
                guideRenderTarget(s, false);   // stepIdx == numSteps -> clears TGT
                guideShowLeds();
                guideStatusLine(s, "DONE");
                // Machine line for the HIL: the summary's three counters, in
                // the grammar the status lines already use.
                Serial.print("\r\nGUIDE done committed=");
                Serial.print(commits);
                Serial.print(" skipped=");
                Serial.print(skips);
                Serial.print(" unfinished=");
                Serial.println(unfinished);
                Serial.flush();
            }
            // Wait for a MAPPED ack - this consumes the HIL's trailing 'q'
            // (a byte left unread here would land on the main menu after the
            // guide returns, where 'q' is the DMX app). Unmapped bytes and
            // line endings are ignored by guideReadInput already.
            int tapRow = -1;
            GuideKey k = guideReadInput(tapRow);
            switch (k) {
                case GuideKey::NEXT: guideBrowse(s, +1); break;
                case GuideKey::PREV: guideBrowse(s, -1); break;
                case GuideKey::BACK:
                    s.state = GuideState::STEP_BACK;   // re-open the last step
                    break;
                case GuideKey::CONFIRM: {
                    // Confirm FINISHES only when everything is actually built.
                    // Otherwise it is the shortcut back to the work: jump to
                    // the first step that was never committed (a skipped one
                    // counts - the user asked to come back to it).
                    int target = guideFirstIncomplete(s);
                    if (target >= sc.numSteps) {
                        s.state = GuideState::EXIT;
                    } else {
                        s.stepIdx = target;
                        s.summaryShown = false;
                        s.state = GuideState::STEP_ENTER;
                    }
                    break;
                }
                case GuideKey::QUIT:
                    s.state = GuideState::EXIT;
                    break;
                default:
                    break;   // s / v / t / m / c: nothing to do at the summary
            }
            break;
        }

        case GuideState::EXIT: {
            // Deviation from DESIGN §3.2's "yesNoMenu(Save progress?)",
            // documented: progress is already durably saved at every commit /
            // skip / back (direct-into-slot, §7) - the prompt would have
            // nothing to decide and would eat one serial byte.
            guideStatusLine(s, "EXIT");
            s.done = true;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Entry / exit
// ---------------------------------------------------------------------------

static void guideSessionBegin(GuideSession& s, GuideScript* sc, int resumeStep) {
    memset(&s, 0, sizeof(s));
    guideOverlayAddWarned = false;   // re-arm the once-per-session LED warning
    s.script = sc;
    s.state = GuideState::INIT;
    s.stepIdx = (resumeStep > 0) ? resumeStep : 0;
    if (s.stepIdx > sc->numSteps) s.stepIdx = sc->numSteps;
    s.waitReturn = GuideState::STEP_WAIT;

    // Encoder ownership, the picker/probeCalib way: one detent per step,
    // clean button edges (the click that launched us must not confirm the
    // first step), and put the divider back on exit.
    s.savedRotaryDivider = rotaryDivider;
    rotaryDivider = 8;
    resetEncoderPosition = true;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;
    probeButton.getButtonPress(true);   // swallow any stale press
}

static void guideExitTail(GuideSession& s) {
    // Belt and braces: no exit path leaves a check's stimulus or ephemeral
    // routes behind (abort is idempotent - a no-op when nothing is running).
    // Nothing to flush any more: every move persists as it happens now that
    // the batching wheel-slide is gone.
    guideCheckAbort();
    guideClearOverlays();
    guideShowLeds();
    rotaryDivider = s.savedRotaryDivider;

    if (!s.powerApplied) {
        // Ruling 1 (design §4.2): nothing the guide did energized the project,
        // so the user's own rails come back. THE TAIL ONLY REPORTS - the
        // caller performs the restore, because it also owns the early-return
        // paths that never reach a session. save=0 there is deliberate: the
        // run file keeps the safe 0 V guideForcePowerSafe wrote, so a
        // half-built project re-opened later still comes up unpowered.
        if (s.haveRestorePower) {
            Serial.print("\r\n  rails + DACs restored (top=");
            Serial.print(s.restoreTop, 2);
            Serial.print("V bot=");
            Serial.print(s.restoreBot, 2);
            Serial.print("V dac0=");
            Serial.print(s.restoreDac0, 2);
            Serial.print("V dac1=");
            Serial.print(s.restoreDac1, 2);
            Serial.println("V) - the project never powered up");
            if (oled.isConnected()) {
                oled.showMultiLineSmallText("Guide closed\nrails restored", true, true);
            }
        } else {
            // No capture (a caller that did not pass one) - say what is true.
            Serial.println("\r\n  rails + DACs left at 0V (safe state - power_on never ran)");
            if (oled.isConnected()) {
                oled.showMultiLineSmallText("Guide closed\nrails at 0V", true, true);
            }
        }
    } else {
        if (oled.isConnected()) {
            oled.showMultiLineSmallText("Guide closed", true, true);
        }
    }
    ReadingDisplay::resetLastShown();

    // Drain whatever the driver still has buffered so nothing lands on the
    // main menu's single-char reader after we return ('q' = DMX mode...).
    delay(50);
    while (Serial.available() > 0) Serial.read();
    Serial.flush();
}

GuideRunResult guideRun(const char* projectYamlPath, int resumeStep,
                        GuideRunPower* power, int* committedOut) {
    // Answer the out-params before ANY early return: a caller that reads
    // `applied` after a parse failure must see "nothing was energized", and a
    // caller that reads the commit count must see "no session, nothing built"
    // rather than whatever the previous session left in the static.
    if (power != nullptr) power->applied = false;
    if (committedOut != nullptr) *committedOut = 0;
    String err;
    if (!guideParse(projectYamlPath, guideScript, err)) {
        Serial.println("\r\nGUIDE parse failed: " + err);
        return GuideRunResult::PARSE_FAILED;
    }
    if (err.length() > 0) {
        Serial.println("\r\nGUIDE parse warnings: " + err);
    }
    if (guideScript.numSteps == 0) {
        Serial.println("\r\nGUIDE nothing to do (no steps, no parts)");
        return GuideRunResult::NOTHING_TO_DO;
    }

    // A finished build must not be relaunched (design-launcher §1.4): a saved
    // step at or past the end - including a source that was edited down to
    // FEWER steps than the saved index - means "complete". guideSessionBegin's
    // clamp would otherwise drop the user straight into the DONE summary.
    if (resumeStep >= guideScript.numSteps) {
        Serial.print("\r\nGUIDE already complete (step ");
        Serial.print(resumeStep);
        Serial.print("/");
        Serial.print(guideScript.numSteps);
        Serial.println(")");
        Serial.flush();
        return GuideRunResult::ALREADY_COMPLETE;
    }

    guideSessionBegin(guideSession, &guideScript, resumeStep);
    if (power != nullptr && power->haveCaptured) {
        guideSession.haveRestorePower = true;
        guideSession.restoreTop  = power->topRail;
        guideSession.restoreBot  = power->bottomRail;
        guideSession.restoreDac0 = power->dac0;
        guideSession.restoreDac1 = power->dac1;
    }
    while (!guideSession.done) {
        guideTick(guideSession);
        jOS.serviceInner();
        delayMicroseconds(50);
    }
    guideExitTail(guideSession);
    if (power != nullptr) power->applied = guideSession.powerApplied;
    if (committedOut != nullptr) {
        int built = 0;
        for (int i = 0; i < guideScript.numSteps && i < MAX_GUIDE_STEPS; i++) {
            if (guideSession.committed[i]) built++;
        }
        *committedOut = built;
    }

    // COMPLETED means NOTHING IS LEFT UNFINISHED - every step is committed or
    // deliberately skipped. It deliberately no longer keys off stepIdx: with
    // the browse ring the cursor can sit at the DONE view with half the build
    // untouched, and quitting from there must read as exit F (resume next
    // launch, no script offer), not as a finished build. Every pre-browse
    // path is unchanged: reaching DONE by walking the steps leaves nothing
    // unfinished by construction.
    return (guideFirstUnfinished(guideSession) >= guideScript.numSteps)
               ? GuideRunResult::COMPLETED
               : GuideRunResult::QUIT;
}
