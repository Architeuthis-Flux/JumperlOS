// SPDX-License-Identifier: MIT
// Non-blocking guide-step viewer. Contract: StepViewer.h.
//
// The retained screen (OledGui, persist=true) is the idle screen while
// armed - it yields to every toast/reading via notePanelTakenByOther and
// returns at the next idle, which IS the no-modality property. Wheel turns
// are consumed only while armed and only outside menus (Menus is BLOCKING
// while open, so this service never sees those turns at all).

#include "StepViewer.h"

#include "GuideScript.h"       // guideParse, GuideScript, step model
#include "OledGui.h"
#include "PartLabels.h"        // emphasis
#include "PartPlacement.h"     // partPinNode
#include "RotaryEncoder.h"     // encoderDirectionState
#include "States.h"            // globalState (cursor persistence)

StepViewer& StepViewer::getInstance() {
    static StepViewer instance;
    return instance;
}
StepViewer& stepViewer = StepViewer::getInstance();

// The step table: one static, same budget the old guide runner's static had
// (~7.5 KB V5 / ~3.8 KB OG - that one died with GuidedFlow, so this is the
// same RAM coming back for the ambient replacement).
static GuideScript viewerScript;

// The retained screen: header (title + counter) and two body lines.
static OledScreen viewerScreen;
static int elHeader = -1, elBody1 = -1, elBody2 = -1;

static void viewerBuildScreen() {
    viewerScreen.clearElements();
    viewerScreen.w = 128;
    viewerScreen.h = 32;
    elHeader = viewerScreen.addText("", 1, 0, "Pragmatism", 8);
    if (elHeader >= 0) viewerScreen.setAnchor(elHeader, OLED_H_LEFT, OLED_V_TOP);
    elBody1 = viewerScreen.addText("", 1, 13, "Pragmatism", 7);
    elBody2 = viewerScreen.addText("", 1, 23, "Pragmatism", 7);
}

int StepViewer::stepCount() const {
    return active ? viewerScript.numSteps : 0;
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

    // Header: "<title> 3/10". Body: text[96] split near the middle on a
    // space so two 64-char elements never truncate silently mid-word.
    char header[64];
    snprintf(header, sizeof(header), "%s %d/%d",
             viewerScript.title[0] ? viewerScript.title : "Steps",
             cursor + 1, viewerScript.numSteps);

    const char* text = viewerScript.steps[cursor].text;
    char line1[64] = "", line2[64] = "";
    size_t len = strlen(text);
    if (len <= 30) {
        strncpy(line1, text, sizeof(line1) - 1);
    } else {
        // Split at the last space at or before char 30 (or hard-split).
        int cut = 30;
        for (int i = 30; i > 12; i--) {
            if (text[i] == ' ') { cut = i; break; }
        }
        size_t l1 = (size_t)cut;
        if (l1 >= sizeof(line1)) l1 = sizeof(line1) - 1;
        memcpy(line1, text, l1);
        line1[l1] = '\0';
        const char* rest = text + cut + (text[cut] == ' ' ? 1 : 0);
        strncpy(line2, rest, sizeof(line2) - 1);
    }

    if (elHeader >= 0) viewerScreen.setText(elHeader, header);
    if (elBody1 >= 0) viewerScreen.setText(elBody1, line1);
    if (elBody2 >= 0) viewerScreen.setText(elBody2, line2);
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

    // Auto-disarm: board clear zeroes parts.guideSource; a context switch
    // loads a state whose guideSource names another file (or none).
    if (globalState.parts.guideSource[0] == '\0' ||
        strncmp(globalState.parts.guideSource, armedSource, sizeof(armedSource)) != 0) {
        disarm();
        return ServiceStatus::IDLE;
    }

    // The wheel browses steps while the viewer is armed (its relative job
    // here). Menus never hand their turns down (BLOCKING); Highlighting runs
    // after this service and gets whatever is left - i.e. nothing while
    // armed, everything again the moment the viewer is off.
    if (encoderDirectionState == UP) {
        encoderDirectionState = NONE;
        cursor = (cursor + 1) % viewerScript.numSteps;
        showStep(true);
        return ServiceStatus::BUSY;
    }
    if (encoderDirectionState == DOWN) {
        encoderDirectionState = NONE;
        cursor = (cursor - 1 + viewerScript.numSteps) % viewerScript.numSteps;
        showStep(true);
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
