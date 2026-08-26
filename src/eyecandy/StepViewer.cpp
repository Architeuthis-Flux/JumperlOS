// SPDX-License-Identifier: MIT
// Non-blocking guide-step viewer. Contract: StepViewer.h (the bench-law
// grip rules live there).
//
// The retained screen (OledGui, persist=true) is the idle screen while
// armed - it yields to every toast/reading via notePanelTakenByOther and
// comes back either at the next showJogo32h() or when a wheel turn
// reclaims it.

#include "StepViewer.h"

#include "GuideScript.h"       // guideParse, GuideScript, step model
#include "Highlighting.h"      // highlightedNet - net scroll owns the wheel
#include "Menus.h"             // inClickMenu
#include "OledGui.h"
#include "PartLabels.h"        // emphasis
#include "PartPlacement.h"     // partPinNode
#include "Probing.h"           // inPadMenu, probeActive
#include "RotaryEncoder.h"     // encoderDirectionState, encoderButtonState
#include "States.h"            // globalState (cursor persistence)
#include "oled.h"              // showJogo32h (post-exit panel restore)

StepViewer& StepViewer::getInstance() {
    static StepViewer instance;
    return instance;
}
StepViewer& stepViewer = StepViewer::getInstance();

// The step table: one static, same budget the old guide runner's static had
// (~7.5 KB V5 / ~3.8 KB OG - that one died with GuidedFlow, so this is the
// same RAM coming back for the ambient replacement).
static GuideScript viewerScript;

// The retained screen: header (title + counter) and three body lines, all
// Andale Mono 5pt on an 8 px pitch - showMultiLineSmallText's bench-proven
// small-text recipe (oled.cpp:2667-2683: 4 lines of ~21 chars on the 128x32;
// the first flash used Pragmatism 8/7pt, whose 18/16 px yAdvances couldn't
// even fit three lines - "the font is way too big").
static OledScreen viewerScreen;
static int elHeader = -1, elBody1 = -1, elBody2 = -1, elBody3 = -1;

static const int VIEWER_WRAP_COLS = 21;

static void viewerBuildScreen() {
    viewerScreen.clearElements();
    viewerScreen.w = 128;
    viewerScreen.h = 32;
    elHeader = viewerScreen.addText("", 1, 0, "Andale Mono", 5);
    elBody1 = viewerScreen.addText("", 1, 8, "Andale Mono", 5);
    elBody2 = viewerScreen.addText("", 1, 16, "Andale Mono", 5);
    elBody3 = viewerScreen.addText("", 1, 24, "Andale Mono", 5);
}

// Word-wrap `text` into up to `nLines` lines of `cols` chars. Returns how
// much of the text was consumed; the caller marks truncation.
static size_t viewerWrap(const char* text, char lines[][64], int nLines, int cols) {
    size_t pos = 0;
    size_t len = strlen(text);
    for (int l = 0; l < nLines; l++) {
        lines[l][0] = '\0';
        while (pos < len && text[pos] == ' ') pos++;
        if (pos >= len) continue;
        size_t remain = len - pos;
        size_t take = remain <= (size_t)cols ? remain : (size_t)cols;
        if (remain > (size_t)cols) {
            // Break at the last space inside the window (hard-split when the
            // window is one long token).
            for (size_t i = take; i > (size_t)cols / 3; i--) {
                if (text[pos + i] == ' ') { take = i; break; }
            }
        }
        if (take > 63) take = 63;
        memcpy(lines[l], text + pos, take);
        lines[l][take] = '\0';
        pos += take;
    }
    return pos;
}

int StepViewer::stepCount() const {
    return active ? viewerScript.numSteps : 0;
}

const GuideScript* StepViewer::armedScript() const {
    return active ? &viewerScript : nullptr;
}

// `VIEWER step=` id, the old GUIDE grammar's successor: <type>_<part|node>.
static void viewerStepId(int idx, char* out, size_t outLen) {
    if (idx < 0 || idx >= viewerScript.numSteps) {
        snprintf(out, outLen, "step");
        return;
    }
    const GuideStep& st = viewerScript.steps[idx];
    switch (st.type) {
        case GuideStepType::PLACE:
            if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
                snprintf(out, outLen, "place_%s", globalState.parts.parts[st.partIdx].name);
            } else {
                snprintf(out, outLen, "place_%d", idx + 1);
            }
            break;
        case GuideStepType::CONNECT:    snprintf(out, outLen, "connect_%d", st.n1); break;
        case GuideStepType::VERIFY:     snprintf(out, outLen, "verify_%d", st.target); break;
        case GuideStepType::POWER_ON:   snprintf(out, outLen, "power_on"); break;
        case GuideStepType::RUN_SCRIPT: snprintf(out, outLen, "run_%d", idx + 1); break;
        default:                        snprintf(out, outLen, "note_%d", idx + 1); break;
    }
}

void StepViewer::applyEmphasis() {
    int16_t nodes[PartLabels::MAX_EMPHASIS_NODES];
    int n = 0;
    if (cursor >= 0 && cursor < viewerScript.numSteps) {
        const GuideStep& st = viewerScript.steps[cursor];
        if (st.type == GuideStepType::PLACE && st.partIdx >= 0 &&
            st.partIdx < globalState.parts.numParts) {
            const PartDefinition& p = globalState.parts.parts[st.partIdx];
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS &&
                            n < PartLabels::MAX_EMPHASIS_NODES; j++) {
                int node = partPinNode(p, p.pins[j]);
                if (node >= 1 && node <= 60) nodes[n++] = (int16_t)node;
            }
        } else if (st.type == GuideStepType::CONNECT) {
            if (st.n1 >= 1 && st.n1 <= 60) nodes[n++] = st.n1;
            if (st.n2 >= 1 && st.n2 <= 60 && n < PartLabels::MAX_EMPHASIS_NODES)
                nodes[n++] = st.n2;
        } else if (st.type == GuideStepType::VERIFY) {
            if (st.target >= 1 && st.target <= 60) nodes[n++] = st.target;
        }
    }
    partLabels.setEmphasis(n > 0 ? nodes : nullptr, n);
}

void StepViewer::showStep(bool announce) {
    if (!active) return;
    if (cursor < 0) cursor = 0;
    if (cursor >= viewerScript.numSteps) cursor = viewerScript.numSteps - 1;

    // Header: "<title> 3/10", truncated so the counter always fits in the
    // 21-char line. Body: three wrapped lines; a ".." tail marks text that
    // ran past the panel (the terminal's `z steps` always has all of it).
    char counter[12];
    snprintf(counter, sizeof(counter), " %d/%d", cursor + 1, viewerScript.numSteps);
    char header[32];
    int titleCols = VIEWER_WRAP_COLS - (int)strlen(counter);
    if (titleCols < 4) titleCols = 4;
    snprintf(header, sizeof(header), "%.*s%s", titleCols,
             viewerScript.title[0] ? viewerScript.title : "Steps", counter);

    const char* text = viewerScript.steps[cursor].text;
    char lines[3][64];
    size_t consumed = viewerWrap(text, lines, 3, VIEWER_WRAP_COLS);
    if (consumed < strlen(text)) {
        size_t l = strlen(lines[2]);
        if (l > (size_t)VIEWER_WRAP_COLS - 2) l = (size_t)VIEWER_WRAP_COLS - 2;
        lines[2][l] = '\0';
        strncat(lines[2], "..", sizeof(lines[2]) - l - 1);
    }

    if (elHeader >= 0) viewerScreen.setText(elHeader, header);
    if (elBody1 >= 0) viewerScreen.setText(elBody1, lines[0]);
    if (elBody2 >= 0) viewerScreen.setText(elBody2, lines[1]);
    if (elBody3 >= 0) viewerScreen.setText(elBody3, lines[2]);
    OledGui::getInstance().requestRender();

    applyEmphasis();

    // Cursor persistence: the ordinary idle auto-save writes it; nothing is
    // force-saved per detent.
    globalState.parts.guideStep = (int16_t)cursor;
    globalState.parts.guideTotal = (int16_t)viewerScript.numSteps;
    globalState.markDirty();

    if (announce) {
        char id[32];
        viewerStepId(cursor, id, sizeof(id));
        Serial.print("\r\nVIEWER step=");
        Serial.print(cursor + 1);
        Serial.print("/");
        Serial.print(viewerScript.numSteps);
        Serial.print(" id=");
        Serial.print(id);
        Serial.print(" text=\"");
        Serial.print(text);
        Serial.println("\"");
        Serial.flush();
    }
}

int StepViewer::arm(const char* sourcePath, int cursorIn) {
    if (sourcePath == nullptr || sourcePath[0] == '\0') return -1;
    // Disarm FIRST: guideParse memsets the LIVE static step table before it
    // can fail (file deleted, FS unmounted), and a failed RE-arm used to
    // leave active=true over a zeroed script - the next wheel turn divided
    // by numSteps==0 (sweep finding, medium). A clean disarm before the
    // parse means every failure path lands in a coherent off state.
    disarm();
    String err;
    if (!guideParse(sourcePath, viewerScript, err)) {
        Serial.println("  (steps unavailable: " + err + ")");
        return -1;
    }
    if (err.length() > 0)
        Serial.println("  (guide parse warnings: " + err + ")");
    if (viewerScript.numSteps <= 0) {
        disarm();
        return 0;
    }

    strncpy(armedSource, sourcePath, sizeof(armedSource) - 1);
    armedSource[sizeof(armedSource) - 1] = '\0';
    active = true;
    holdLatch = false;
    cursor = cursorIn;
    viewerBuildScreen();
    OledGui::getInstance().activate(&viewerScreen, /*persist=*/true);
    showStep(/*announce=*/false);
    return viewerScript.numSteps;
}

void StepViewer::disarm() {
    if (active) {
        partLabels.clearEmphasis();
        OledGui& gui = OledGui::getInstance();
        gui.forgetIdle(&viewerScreen);
        if (gui.active() == &viewerScreen) gui.deactivate();
    }
    active = false;
    armedSource[0] = '\0';
}

ServiceStatus StepViewer::service() {
    if (!active) return ServiceStatus::IDLE;

    // Belt-and-braces against an armed-but-empty table (arm() disarms
    // before parsing now, but the cursor math below must never see 0).
    if (viewerScript.numSteps <= 0) {
        disarm();
        return ServiceStatus::IDLE;
    }

    // Auto-disarm: board clear zeroes parts.guideSource; a context switch
    // loads a state whose guideSource names another file (or none).
    if (globalState.parts.guideSource[0] == '\0' ||
        strncmp(globalState.parts.guideSource, armedSource, sizeof(armedSource)) != 0) {
        disarm();
        return ServiceStatus::IDLE;
    }

    // Bench law (StepViewer.h): hands off unless the wheel has no other job.
    // The click menu runs as a modal loop on this core (its serviceInner
    // pass never reaches us), but these flags close every other door - pad
    // menus, probe mode, a BLOCKING adjuster, and the between-passes windows.
    if (inClickMenu != 0 || inPadMenu != 0) return ServiceStatus::IDLE;
    if (jOS.getBlockingService() != nullptr) return ServiceStatus::IDLE;
    if (probeActive) return ServiceStatus::IDLE;

    OledGui& gui = OledGui::getInstance();
    bool showing = (gui.active() == &viewerScreen) && gui.ownsPanel();

    // Click-and-HOLD while the steps screen is showing = the physical exit.
    // The button state is left alone (core 1's hold animation, and nothing
    // else consumes a hold-release at idle - menu entry needs RELEASED
    // after PRESSED, which a hold never produces).
    if (holdLatch) {
        if (encoderButtonState == IDLE) holdLatch = false;
    } else if (showing && (encoderButtonState == HELD ||
                           encoderButtonState == MEDIUM_HELD)) {
        holdLatch = true;
        disarm();
        Serial.println("\r\nVIEWER off");
        Serial.flush();
        oled.showJogo32h();
        return ServiceStatus::BUSY;
    }

    // A probe tap hands the wheel to net scroll (Highlighting) until the
    // highlight times out; the viewer only takes turns when nothing is
    // highlighted - so the wheel always does what the board/OLED shows.
    if (highlightedNet > 0) return ServiceStatus::IDLE;

    if (encoderDirectionState == UP || encoderDirectionState == DOWN) {
        if (!showing) {
            // First turn while yielded RECLAIMS the panel at the current
            // step; it does not move the cursor (turning past a reading
            // should show you where you were, not step 3 ahead of it).
            // Only OUR idle registration is reclaimed: if another
            // persistent screen took the idle slot since (a MicroPython
            // stats page), stealing it back would silently evict the
            // user's page (sweep finding) - the turn falls through to
            // Highlighting instead - so test BEFORE the ack, since
            // Highlighting is registered behind us and must still see the
            // detent we are declining.
            if (gui.idleScreen() != &viewerScreen) return ServiceStatus::IDLE;
            encoderDirectionState = NONE;
            gui.showIdle();
            showStep(false);
            return ServiceStatus::BUSY;
        }
        bool up = (encoderDirectionState == UP);
        encoderDirectionState = NONE;
        if (up) cursor = (cursor + 1) % viewerScript.numSteps;
        else cursor = (cursor - 1 + viewerScript.numSteps) % viewerScript.numSteps;
        showStep(false);   // wheel turns are silent on serial (bench law)
        return ServiceStatus::BUSY;
    }
    return ServiceStatus::IDLE;
}

void StepViewer::command(const String& rest) {
    String r = rest;
    r.trim();
    r.toLowerCase();

    if (r == "off") {
        disarm();
        Serial.println("VIEWER off");
        return;
    }
    if (r == "on") {
        if (globalState.parts.guideSource[0] == '\0') {
            Serial.println("VIEWER error no guide source in the active context");
            return;
        }
        int steps = arm(globalState.parts.guideSource, globalState.parts.guideStep);
        if (steps > 0) {
            Serial.print("\r\nVIEWER steps=");
            Serial.print(steps);
            Serial.print(" cursor=");
            Serial.println(cursor);
            showStep(true);
        } else if (steps == 0) {
            Serial.println("VIEWER error the guide has no steps");
        }
        return;
    }
    if (!active) {
        Serial.println("VIEWER error not armed (open a project, or `z steps on`)");
        return;
    }
    if (r == "next") {
        cursor = (cursor + 1) % viewerScript.numSteps;
        showStep(true);
    } else if (r == "prev") {
        cursor = (cursor - 1 + viewerScript.numSteps) % viewerScript.numSteps;
        showStep(true);
    } else if (r.length() > 0) {
        int n = r.toInt();
        if (n >= 1 && n <= viewerScript.numSteps) {
            cursor = n - 1;
            showStep(true);
        } else {
            Serial.println("VIEWER error step out of range (1-" +
                           String(viewerScript.numSteps) + ")");
        }
    } else {
        showStep(true);   // bare `z steps`: re-announce the current step
    }
}
