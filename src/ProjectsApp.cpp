// SPDX-License-Identifier: MIT
// Projects launcher - the Apps-menu entry that turns /projects/<dir>/ into a
// running circuit. Design: CodeDocs/DESIGN_PROJECTS_SUBSYSTEM.md §1.
//
// Flow (non-guided path):
//   listProjects -> project picker -> variant picker (only when >1 wiring*.yaml)
//   -> guided seam (runGuidedProject, task 6) -> enterTemporarySlot(8)
//   -> loadSlotFromPath(wiring) -> run main[.variant].py with `_jl_project`
//   injected -> keep-prompt -> saveSlot(n) / exitTemporarySlot.
//
// TWO LATCHES run through this file; every exit path has to leave both clean:
//
//  1. SlotManager::temporarySlotActive (States.cpp:3220/:3241/:3266).
//     enterTemporarySlot refuses to nest, so a latch left set here bricks
//     every future temp-slot app until reboot. Rules:
//       - cancel BEFORE enterTemporarySlot -> plain return, no slot calls;
//       - enterTemporarySlot returning false -> plain return. We do NOT own
//         the latch in that case and exiting it would clobber whoever does;
//       - every path after a successful enter ends in exitTemporarySlot(...).
//     The keep sequence is EXACTLY saveSlot(n) -> exitTemporarySlot(false) ->
//     loadSlot(n) -> refreshConnections(-1): saveSlot sets activeSlotNumber
//     while the latch is still up, exitTemporarySlot(false) restores tracking
//     without touching hardware, loadSlot makes the kept project live. A
//     saveSlot FAILURE must not continue into that sequence - it would discard
//     the user's circuit and load a stale slot n - so it falls back to the
//     decline path (full exitTemporarySlot restore).
//
//  2. Menus::inClickMenu. Core 1 only renders b.print() text while it is 1
//     (main.cpp:1817) and only renders NETS while it is 0 (main.cpp:1746) -
//     and doMenuAction clears it before running an app (Menus.cpp:463). So the
//     pickers raise it for their own duration and drop it again (plus
//     b.clear() + requestLedShow(-1), the pickPythonScriptFromClickMenu exit
//     idiom) BEFORE the project's nets need to show.

#include "ProjectsApp.h"

#include "Commands.h"         // requestLedShow, refreshConnections
#include "FilesystemStuff.h"  // safeFile*, FatFS Dir walk
#include "Graphics.h"         // b.print (no-op on OG)
#include "GuidedFlow.h"       // guideRun / guideForcePowerSafe (task 6)
#include "JumperlOS.h"        // jOS.serviceInner()
#include "Menus.h"            // inClickMenu, yesNoMenu
#include "PartPlacement.h"    // removePartPlacement (guided restart path)
#include "Python_Proper.h"    // executePythonFileContent, setGlobalStreamWithInterrupt
#include "RotaryEncoder.h"    // encoder state machine
#include "States.h"           // SlotManager
#include "externVars.h"       // pauseCore2ForFlash / unpauseCore2ForFlash
#include "oled.h"

// LED matrix text colors, matched to FileManager's click-menu view
// (FilesystemStuff.cpp:465): dim blue header row, dim magenta item row.
static const uint32_t PICKER_HEADER_COLOR = 0x001008;
static const uint32_t PICKER_ITEM_COLOR = 0x100810;

// b.print renders 7 glyphs per half-row.
static const int LED_ROW_CHARS = 7;

// Bound on the meta: text scan - a project wiring file's meta: block sits at
// the top, so this never has to walk the parts:/bridges: bulk.
static const int META_SCAN_MAX_LINES = 64;

// ============================================================================
// Small string helpers
// ============================================================================

// "/projects/555/wiring.yaml" -> "555"
static String projectDirOfPath(const String& path) {
    int last = path.lastIndexOf('/');
    if (last <= 0)
        return String("");
    int prev = path.lastIndexOf('/', last - 1);
    return path.substring(prev + 1, last);
}

// Value after the first ':' on an already-trimmed `key: value` line. A quoted
// value takes the quote pair (the same rule States.cpp's parseScalar uses).
static String scalarAfterColon(const String& trimmedLine) {
    int colon = trimmedLine.indexOf(':');
    if (colon < 0)
        return String("");
    String v = trimmedLine.substring(colon + 1);
    v.trim();
    if (v.length() >= 2 && v.charAt(0) == '"') {
        int close = v.indexOf('"', 1);
        if (close > 0)
            return v.substring(1, close);
        return v.substring(1);
    }
    return v;
}

// Truncate defensively for the 7-char LED half-row (dir names are <=7 by
// convention, but nothing enforces it on a user-created directory).
static String ledFit(const String& s) {
    return (s.length() > LED_ROW_CHARS) ? s.substring(0, LED_ROW_CHARS) : s;
}

// Everything injected into the `_jl_project` dict literal is a filesystem name
// or a meta: value, i.e. attacker-adjacent only in the "Kevin typo'd a quote"
// sense - but an embedded '"' or newline would turn the injected line into a
// syntax error (or worse, an extra statement) in front of the user's script.
// Drop anything that isn't printable ASCII, plus quotes and backslashes.
static String pyStringSafe(const String& s) {
    String out;
    out.reserve(s.length());
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\')
            continue;
        out += c;
    }
    return out;
}

// ============================================================================
// meta: scan and project/variant listing
// ============================================================================

bool readProjectMeta(const String& yamlPath, ProjectMeta& out) {
    out.dir = projectDirOfPath(yamlPath);
    out.title = out.dir;
    out.summary = "";
    out.variant = "";
    out.script = "";

    File f = safeFileOpen(yamlPath.c_str(), "r");
    if (!f)
        return false;

    bool inMeta = false;
    int lines = 0;
    while (f.available() && lines < META_SCAN_MAX_LINES) {
        String line = f.readStringUntil('\n');
        lines++;
        if (line.length() == 0)
            continue;
        // Capture indentation BEFORE trimming - same indent-hardening rule the
        // slot parser uses to decide what is a top-level section header.
        bool indented = (line.charAt(0) == ' ' || line.charAt(0) == '\t');
        String t = line;
        t.trim();
        if (t.length() == 0 || t.startsWith("#"))
            continue;

        if (!indented) {
            if (t.startsWith("meta:")) {
                inMeta = true;
                continue;
            }
            if (inMeta)
                break;  // meta: block ended - nothing else here to read
            continue;
        }
        if (!inMeta)
            continue;

        if (t.startsWith("title:"))
            out.title = scalarAfterColon(t);
        else if (t.startsWith("summary:"))
            out.summary = scalarAfterColon(t);
        else if (t.startsWith("variant:"))
            out.variant = scalarAfterColon(t);
        else if (t.startsWith("script:"))
            out.script = scalarAfterColon(t);
    }
    safeFileClose(f, false);

    if (out.title.length() == 0)
        out.title = out.dir;
    return true;
}

int listProjects(ProjectMeta* out, int maxOut) {
    if (out == nullptr || maxOut <= 0)
        return 0;
    if (!safeFileExists(PROJECTS_DIR))
        return 0;

    // Directory iteration follows FileManager::refreshListing
    // (FilesystemStuff.cpp:685-760): FatFS.openDir under a Core-1 pause, and
    // NOTHING else inside the walk - the safeFile*/meta reads below take the
    // fs mutex themselves and must not nest inside the paused window.
    String dirs[PROJECTS_MAX];
    int found = 0;
    bool was_paused = pauseCore2ForFlash(100);
    Dir d = FatFS.openDir(PROJECTS_DIR);
    while (d.next() && found < PROJECTS_MAX && found < maxOut) {
        if (!d.isDirectory())
            continue;
        String name = d.fileName();
        if (name.length() == 0 || name.startsWith("."))
            continue;
        dirs[found++] = name;
    }
    unpauseCore2ForFlash(was_paused);

    // Alphabetical so the picker order is stable across reboots (FatFS returns
    // directory order, which follows creation).
    for (int i = 1; i < found; i++) {
        String key = dirs[i];
        int j = i - 1;
        while (j >= 0 && dirs[j] > key) {
            dirs[j + 1] = dirs[j];
            j--;
        }
        dirs[j + 1] = key;
    }

    int count = 0;
    for (int i = 0; i < found; i++) {
        String wiring = String(PROJECTS_DIR) + "/" + dirs[i] + "/wiring.yaml";
        if (!safeFileExists(wiring.c_str()))
            continue;  // a directory without a default wiring isn't a project
        if (!readProjectMeta(wiring, out[count]))
            continue;
        out[count].dir = dirs[i];
        count++;
    }
    return count;
}

// Every wiring*.yaml in a project directory, plain "wiring.yaml" first and the
// rest alphabetical. Returns the count written to outNames[].
static int listVariantFiles(const String& projectPath, String* outNames, int maxOut) {
    if (outNames == nullptr || maxOut <= 0)
        return 0;

    int found = 0;
    bool was_paused = pauseCore2ForFlash(100);
    Dir d = FatFS.openDir(projectPath);
    while (d.next() && found < maxOut) {
        if (d.isDirectory())
            continue;
        String name = d.fileName();
        String lower = name;
        lower.toLowerCase();
        if (!lower.startsWith("wiring") || !lower.endsWith(".yaml"))
            continue;
        outNames[found++] = name;
    }
    unpauseCore2ForFlash(was_paused);

    for (int i = 1; i < found; i++) {
        String key = outNames[i];
        int j = i - 1;
        while (j >= 0 && outNames[j] > key) {
            outNames[j + 1] = outNames[j];
            j--;
        }
        outNames[j + 1] = key;
    }
    // Default variant first regardless of alphabetical order.
    for (int i = 1; i < found; i++) {
        if (outNames[i] == "wiring.yaml") {
            String key = outNames[i];
            for (int j = i; j > 0; j--)
                outNames[j] = outNames[j - 1];
            outNames[0] = key;
            break;
        }
    }
    return found;
}

// Cheap text scan for an un-indented `parts:` or `guide:` line - the "is this
// a guided project?" test the guided seam is gated on. Deliberately not the
// full parser: task 6 re-reads the file itself.
static bool wiringHasGuideSections(const String& wiringPath) {
    File f = safeFileOpen(wiringPath.c_str(), "r");
    if (!f)
        return false;
    bool found = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() == 0)
            continue;
        if (line.charAt(0) == ' ' || line.charAt(0) == '\t')
            continue;
        String t = line;
        t.trim();
        if (t.startsWith("parts:") || t.startsWith("guide:")) {
            found = true;
            break;
        }
    }
    safeFileClose(f, false);
    return found;
}

// Forward declarations for the guided flow (implemented below the picker
// helpers they use).
static bool guidedFlowInner(const String& wiringPath, int destSlot);

bool runGuidedProject(const String& dir, const String& wiringPath) {
    (void)dir;
    return guidedFlowInner(wiringPath, -1);   // -1 = interactive pickers
}

bool runGuidedProjectTo(const String& wiringPath, int destSlot) {
    return guidedFlowInner(wiringPath, destSlot);
}

// ============================================================================
// UI helpers
// ============================================================================

// OLED + terminal message. b.print is left alone here: these fire either
// before the picker raises inClickMenu or after it dropped it, so the matrix
// is showing nets and shouldn't be stomped.
static void notify(const String& oledText, const String& terminalText, int holdMs) {
    Serial.println(terminalText);
    Serial.flush();
    if (oled.oledConnected) {
        oled.clearPrintShow(oledText, 2, true, true, true);
    }
    if (holdMs > 0)
        delay(holdMs);
}

static void drawPickerItem(const char* header, const String& ledLabel,
                           const String& title, const String& summary) {
    // LED matrix: header on the top half, item on the bottom half (the
    // FileManager click-menu layout). No-op on OG.
    b.clear();
    b.print(ledFit(String(header)).c_str(), PICKER_HEADER_COLOR, 0xFFFFFF, 0, 0, 1);
    b.print(ledFit(ledLabel).c_str(), PICKER_ITEM_COLOR, 0xFFFFFF, 0, 1, 1);
    requestLedShow(2);

    if (oled.oledConnected) {
        if (summary.length() > 0) {
            oled.resetMultiLineSmallText();
            oled.showMultiLineSmallText((title + "\n" + summary).c_str());
        } else {
            oled.clearPrintShow(title, 2, true, true, true);
        }
    }

    // Terminal mirror (the only feedback when neither OLED nor matrix is up).
    Serial.print("\r  ");
    Serial.print(header);
    Serial.print(": ");
    Serial.print(title);
    Serial.print("                    \r");
    Serial.flush();
}

// Encoder picker shared by the project / variant / slot pickers. Returns the
// chosen index, or -1 when the user cancelled (encoder hold or a serial byte).
//
// Owns inClickMenu for its duration (see the file header) and resets the
// button state machine on entry so the click that launched the app - or the
// click that made the previous selection - can't fall through into this loop.
//
// machineTag is printed once on entry as `<TAG> n=<count>`: one grep-able line
// per picker so a headless caller can see the picker open and how many entries
// it holds (test_projects.py phase 6(d) drives the launcher on that line).
// startIdx pre-selects an entry (the guided destination picker defaults to
// the first empty slot); out of range falls back to 0.
static int runPicker(const char* machineTag, const char* header,
                     const String* ledLabels, const String* titles,
                     const String* summaries, int count, int startIdx = 0) {
    if (count <= 0)
        return -1;

    int idx = (startIdx >= 0 && startIdx < count) ? startIdx : 0;
    int result = -1;
    bool needsDraw = true;
    unsigned long lastShowRequest = 0;

    // Sensitivity: one item per detent. Every picker-shaped consumer of
    // encoderDirectionState owns this global for its duration and puts it back
    // (yesNoMenu sets 8, Menus.cpp:2469; the node picker and the history scrub
    // save/restore, Menus.cpp:5705-5712). The default happens to be 8, but a
    // value editor that leaked 3/4/12 - or an entry that didn't come through
    // getMenuSelection, like run_app("Projects") - would otherwise land here.
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;

    Serial.print("\r\n");
    Serial.print(machineTag);
    Serial.print(" n=");
    Serial.println(count);
    Serial.flush();

    while (true) {
        if (needsDraw) {
            drawPickerItem(header, ledLabels[idx], titles[idx],
                           summaries ? summaries[idx] : String(""));
            lastShowRequest = millis();
            needsDraw = false;
        }

        jOS.serviceInner();
        rotaryEncoderButtonStuff();

        // Re-assert the LED show request: Core 2's end-of-frame compare-and-swap
        // can swallow one issued mid-frame (Menus.cpp's menuShowKeepalive).
        if (millis() - lastShowRequest >= 250) {
            requestLedShow(2);
            lastShowRequest = millis();
        }

        if (encoderButtonState == HELD) {
            result = -1;
            break;
        }
        // Serial byte cancels, the same way it does on every other picker
        // screen (Menus.cpp:1992, :2646 - "encoder hold, serial byte, or probe
        // button"). Consumed here: this returns into an app context, not a
        // command reader, so a byte left in the buffer would land on the
        // single-char command handler after the app exits.
        if (Serial.available() > 0) {
            Serial.read();
            result = -1;
            break;
        }
        if (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) {
            encoderButtonState = IDLE;
            result = idx;
            break;
        }
        if (encoderDirectionState == UP) {
            encoderDirectionState = NONE;
            idx = (idx + 1) % count;
            needsDraw = true;
        } else if (encoderDirectionState == DOWN) {
            encoderDirectionState = NONE;
            idx = (idx - 1 + count) % count;
            needsDraw = true;
        }

        delayMicroseconds(1000);
    }

    // Leave menu-render mode, put the breadboard back, hand the encoder
    // sensitivity back to whoever set it.
    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    requestLedShow(-1);
    Serial.println();
    return result;
}

// After a script ends the user is often still holding the clickwheel (holding
// is how you interrupt one - Python_Proper.cpp:5016 raises KeyboardInterrupt).
// Wait for the button to come back to rest so the keep-prompt doesn't read
// that same hold as "cancel" the instant it opens.
static void waitForButtonRest(unsigned long timeoutMs) {
    unsigned long start = millis();
    unsigned long restSince = 0;
    while (millis() - start < timeoutMs) {
        jOS.serviceInner();
        rotaryEncoderButtonStuff();
        bool resting = (encoderButtonState != HELD && encoderButtonState != PRESSED &&
                        encoderButtonState != LONG_HELD && encoderButtonState != MEDIUM_HELD);
        if (resting) {
            if (restSince == 0)
                restSince = millis();
            else if (millis() - restSince > 150)
                break;
        } else {
            restSince = 0;
        }
        delayMicroseconds(1000);
    }
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;
}

// Slot picker for the keep-flow: 0-7 with an occupied indicator. -1 = cancel.
static int pickDestinationSlot(void) {
    // static for the same reason the launcher's arrays are: label arrays are
    // Strings, and this runs at the bottom of the deepest menu call path.
    static String ledLabels[NUM_SLOTS];
    static String titles[NUM_SLOTS];
    static String summaries[NUM_SLOTS];

    SlotManager& mgr = SlotManager::getInstance();
    Serial.println("\n\r  Save this circuit to which slot?");
    for (int i = 0; i < NUM_SLOTS; i++) {
        bool used = mgr.slotExists(i);
        ledLabels[i] = String(i) + (used ? " used" : " free");
        titles[i] = "Slot " + String(i);
        summaries[i] = used ? "overwrites saved slot" : "empty slot";
    }
    return runPicker("SLOTS", "Slot", ledLabels, titles, summaries, NUM_SLOTS);
}

// ============================================================================
// Guided flow (task 6): destination slot + resume, then guideRun
// ============================================================================
// NO temp slots anywhere in this path - the guide builds DIRECTLY into a
// destination slot (DESIGN_GUIDED_PLACEMENT.md §7, Kevin's decision), which
// is why the seam is called before enterTemporarySlot in the launcher.
// Every return from guidedFlowInner is `true`: once the wiring is known to
// be guided, the guided flow owns the interaction - including cancels at its
// own pickers (returning false there would drop the user into the non-guided
// temp-slot path they never asked for).

// Cheap text scan of /slots/slotN.yaml for a guideProgress line bound to
// this wiring. toYAML emits it on line 3 (right after version/sourceOfTruth),
// so a short bounded read is enough. Returns the slot, with the saved step in
// stepOut; -1 when no slot resumes this project.
static int findGuideProgressSlot(const String& wiringPath, int& stepOut) {
    stepOut = 0;
    String needle = "\"" + wiringPath + "\"";
    for (int n = 0; n < NUM_SLOTS; n++) {
        String path = "/slots/slot" + String(n) + ".yaml";
        File f = safeFileOpen(path.c_str(), "r");
        if (!f)
            continue;
        int lines = 0;
        int found = -1;
        while (f.available() && lines < 20) {
            String line = f.readStringUntil('\n');
            lines++;
            if (line.length() == 0)
                continue;
            if (line.charAt(0) == ' ' || line.charAt(0) == '\t')
                continue;   // guideProgress: is a top-level line
            String t = line;
            t.trim();
            if (!t.startsWith("guideProgress:"))
                continue;
            if (t.indexOf(needle) < 0)
                break;   // bound to a different project - this slot is out
            int stepIdx = t.indexOf("step:");
            if (stepIdx >= 0) {
                int endIdx = t.indexOf('}', stepIdx);
                if (endIdx < 0)
                    endIdx = t.length();
                String val = t.substring(stepIdx + 5, endIdx);
                val.trim();
                stepOut = val.toInt();
            }
            found = n;
            break;
        }
        safeFileClose(f, false);
        if (found >= 0)
            return found;
    }
    return -1;
}

// Destination picker for the guided flow: slots 0-7 with occupied indicator,
// default = first empty (modeled on the task-5 keep-prompt picker). -1 =
// cancel (encoder hold or serial byte - the guide's own keys start AFTER
// this picker returns, so a byte here is a cancel, not a guide key).
static int pickGuideDestinationSlot(void) {
    static String ledLabels[NUM_SLOTS];
    static String titles[NUM_SLOTS];
    static String summaries[NUM_SLOTS];

    SlotManager& mgr = SlotManager::getInstance();
    Serial.println("\n\r  Build this guided project into which slot?");
    int firstEmpty = 0;
    bool haveEmpty = false;
    for (int i = 0; i < NUM_SLOTS; i++) {
        bool used = mgr.slotExists(i);
        if (!used && !haveEmpty) {
            firstEmpty = i;
            haveEmpty = true;
        }
        ledLabels[i] = String(i) + (used ? " used" : " free");
        titles[i] = "Slot " + String(i);
        summaries[i] = used ? "overwrites saved slot" : "empty slot";
    }
    return runPicker("GSLOTS", "Build in", ledLabels, titles, summaries,
                     NUM_SLOTS, firstEmpty);
}

static bool guidedFlowInner(const String& wiringPath, int destSlot) {
    SlotManager& mgr = SlotManager::getInstance();
    String err;

    // --- resume scan -------------------------------------------------------
    int resumeStep = 0;
    int resumeSlot = findGuideProgressSlot(wiringPath, resumeStep);
    if (resumeSlot >= 0) {
        // Machine-parseable line BEFORE the menu, so a headless driver can
        // see the offer and answer it (yesNoMenu accepts serial y/n; any
        // other byte cancels - the HIL's 'q' path).
        Serial.print("\r\nGUIDE resume slot=");
        Serial.print(resumeSlot);
        Serial.print(" step=");
        Serial.println(resumeStep);
        Serial.flush();
        notify("Resume?\nstep " + String(resumeStep + 1),
               "\n\r  Guided build in progress (slot " + String(resumeSlot) +
                   ", step " + String(resumeStep + 1) +
                   "). y/click Yes = resume, n = restart, other = cancel",
               0);
        // The `z <path> <slot>` command line that can launch this flow ends
        // in \r\n; the line reader consumes through the \r and the \n stays
        // in the RAW Serial buffer. yesNoMenu treats any non-y/n byte as
        // cancel, so that leftover terminator would cancel the resume offer
        // before anyone saw it (bench-reproduced). Swallow line endings ONLY
        // - a real y/n/other answer already in flight must survive.
        delay(20);
        while (Serial.available() > 0) {
            int p = Serial.peek();
            if (p == '\r' || p == '\n') {
                Serial.read();
                continue;
            }
            break;
        }
        int r = yesNoMenu(20000);   // 1 = resume, 0 = restart, -1 = cancel
        b.clear();
        requestLedShow(-1);
        if (r == 1) {
            if (!mgr.loadSlot(resumeSlot, err)) {
                notify("Load\nfailed", "\n\r  Failed to load slot " +
                                           String(resumeSlot) + ": " + err, 2000);
                return true;
            }
            guideRun(wiringPath.c_str(), resumeStep);
            return true;
        }
        if (r == -1) {
            Serial.println("\r\nGUIDE cancelled");
            Serial.println("  Cancelled.");
            return true;
        }
        // r == 0: restart. Un-place everything, clear the binding, save the
        // cleaned slot, then run the fresh path below. (The fresh path's
        // loadSlotFromPath replaces the state wholesale anyway; this cleanup
        // is what makes a cancel-at-the-destination-picker leave the old
        // slot honest rather than half-built.)
        if (mgr.loadSlot(resumeSlot, err)) {
            JumperlessState& st = mgr.getActiveState();
            String rerr;
            for (int i = 0; i < st.parts.numParts && i < MAX_PARTS; i++) {
                if (st.parts.parts[i].placed)
                    removePartPlacement(st, i, rerr);
            }
            st.parts.guideSource[0] = '\0';
            st.parts.guideStep = 0;
            refreshConnections(-1);
            mgr.saveSlot(resumeSlot, err, /*skipValidation=*/true);
            Serial.println("\r\n  Restarted - previous progress cleared.");
        } else {
            Serial.println("\r\n  (could not load slot " + String(resumeSlot) +
                           " to clear it: " + err + ")");
        }
    }

    // --- destination slot --------------------------------------------------
    int dest = destSlot;
    if (dest < 0) {
        dest = pickGuideDestinationSlot();
        if (dest < 0) {
            // Cancel at the destination picker: nothing was modified (or the
            // restart cleanup above already saved a consistent slot). Still
            // `true` - the guided flow owns this wiring, see the block
            // comment above.
            Serial.println("\r\nGUIDE cancelled");
            Serial.println("  Cancelled.");
            return true;
        }
    } else if (dest >= NUM_SLOTS) {
        Serial.println("\r\nGUIDE error bad destination slot " + String(dest));
        return true;
    }

    // --- fresh start into the destination slot -----------------------------
    // Order per the task brief: setActiveSlot -> loadSlotFromPath -> force
    // rails 0V -> saveSlot -> guideRun. (loadSlotFromPath applies the file's
    // power: momentarily before the 0V force lands - noted in the report.)
    mgr.setActiveSlot(dest);
    if (!mgr.loadSlotFromPath(wiringPath, err)) {
        // We already own the interaction; falling back to the non-guided
        // path would just fail the same load again.
        notify("Load\nfailed", "\n\r  Failed to load " + wiringPath + ": " + err, 2000);
        Serial.println("\r\nGUIDE error load failed");
        return true;
    }
    guideForcePowerSafe();
    if (!mgr.saveSlot(dest, err, /*skipValidation=*/true)) {
        Serial.println("\r\n  (initial save to slot " + String(dest) +
                       " failed: " + err + " - continuing, commits will retry)");
    }
    Serial.println("\r\n  Building into slot " + String(dest) + ".");
    guideRun(wiringPath.c_str(), -1);
    return true;
}

// ============================================================================
// The launcher
// ============================================================================

void projectsAppLauncher(void) {
    SlotManager& mgr = SlotManager::getInstance();

    // TODO(task 8): initializeProjects() self-heal belongs here - it
    // (re)provisions /projects from the built-in projectFiles[] table, the way
    // initializeMicroPythonExamples does for /python_scripts. That function
    // doesn't exist yet, so v1 lists whatever is already on the filesystem.

    // static, not a stack local: PROJECTS_MAX * 5 Strings is a lot of app
    // stack for the deepest menu entry path (the same argument
    // pickPythonScriptFromClickMenu makes when it heap-allocates FileManager).
    // The launcher is not re-entrant, so a single instance is fine.
    static ProjectMeta projects[PROJECTS_MAX];

    Serial.println("\n\r=== Projects ===");
    Serial.println("Rotary: Navigate | Short Click: Select | Long Press or any key: Cancel");

    int count = listProjects(projects, PROJECTS_MAX);
    if (count <= 0) {
        // EXIT 1: nothing entered, nothing to unwind. This is also the only
        // path out of the launcher that needs no encoder input, which is what
        // test_projects.py phase 6(c) times to prove the apps[] row resolves -
        // it asserts >= 1400 ms, so don't shorten this hold without updating it.
        notify("No\nprojects", "\n\r  No projects found in " + String(PROJECTS_DIR), 1500);
        return;
    }

    static String ledLabels[PROJECTS_MAX];
    static String titles[PROJECTS_MAX];
    static String summaries[PROJECTS_MAX];
    for (int i = 0; i < count; i++) {
        ledLabels[i] = projects[i].dir;
        titles[i] = projects[i].title;
        summaries[i] = projects[i].summary;
        Serial.println("  " + projects[i].dir + "  -  " + projects[i].title);
    }

    int chosen = runPicker("PROJECTS", "Project", ledLabels, titles, summaries, count);
    if (chosen < 0) {
        // EXIT 2: cancelled before any slot state was touched.
        Serial.println("  Cancelled.");
        return;
    }

    ProjectMeta meta = projects[chosen];
    String projectPath = String(PROJECTS_DIR) + "/" + meta.dir;
    String wiringPath = projectPath + "/wiring.yaml";

    // --- variant picker (only when the project ships more than one wiring) ---
    String variantFiles[PROJECT_VARIANTS_MAX];
    int variantCount = listVariantFiles(projectPath, variantFiles, PROJECT_VARIANTS_MAX);
    if (variantCount > 1) {
        static String vLed[PROJECT_VARIANTS_MAX];
        static String vTitle[PROJECT_VARIANTS_MAX];
        static String vSummary[PROJECT_VARIANTS_MAX];
        static ProjectMeta vMeta[PROJECT_VARIANTS_MAX];
        for (int i = 0; i < variantCount; i++) {
            String path = projectPath + "/" + variantFiles[i];
            readProjectMeta(path, vMeta[i]);
            String label = vMeta[i].variant;
            if (label.length() == 0) {
                // No meta.variant: fall back to the filename's middle part,
                // "wiring.nano.yaml" -> "nano" ("wiring.yaml" -> "default").
                label = variantFiles[i];
                label.replace("wiring", "");
                label.replace(".yaml", "");
                label.replace(".", "");
                if (label.length() == 0)
                    label = "default";
                // Write the derived label BACK: it is what main.<variant>.py
                // resolution and `_jl_project["variant"]` read downstream, so a
                // wiring.<v>.yaml with no meta.variant would otherwise display
                // right and resolve wrong.
                vMeta[i].variant = label;
            }
            vLed[i] = label;
            vTitle[i] = label;
            vSummary[i] = vMeta[i].summary;
        }
        int v = runPicker("VARIANTS", "Variant", vLed, vTitle, vSummary, variantCount);
        if (v < 0) {
            // EXIT 3: cancelled before any slot state was touched.
            Serial.println("  Cancelled.");
            return;
        }
        wiringPath = projectPath + "/" + variantFiles[v];
        meta = vMeta[v];
        meta.dir = projects[chosen].dir;
    }

    // --- guided seam (task 6 owns everything past a `true` here) -------------
    if (wiringHasGuideSections(wiringPath) && runGuidedProject(meta.dir, wiringPath)) {
        // EXIT 4: the guided flow ran the whole show INCLUDING its own slot
        // choice - it is called before enterTemporarySlot precisely so there is
        // no latch of ours for it to trip over.
        return;
    }

    // --- non-guided path: temp slot 8 ---------------------------------------
    if (!mgr.enterTemporarySlot(8)) {
        // EXIT 5: someone else holds the latch (or slot 8 was refused). We do
        // NOT own it, so we must not exit it - that would restore the other
        // owner's original slot out from under them.
        notify("Slot busy", "\n\r  Could not enter the temporary slot (already in use).", 1500);
        return;
    }

    String err;
    if (!mgr.loadSlotFromPath(wiringPath, err)) {
        // EXIT 6: load failed - unwind the latch we just took.
        mgr.exitTemporarySlot();
        notify("Load\nfailed", "\n\r  Failed to load " + wiringPath + ": " + err, 2000);
        return;
    }
    // loadSlotFromPath already routed and applied the state; this second pass
    // is the brief's belt-and-braces refresh and doubles as the "put the nets
    // back on the matrix now that the picker released inClickMenu" redraw.
    refreshConnections(-1);

    Serial.println("\n\r  Loaded " + wiringPath);

    // --- resolve the companion script ---------------------------------------
    // main.<variant>.py if present, else meta.script, else main.py.
    String scriptPath;
    if (meta.variant.length() > 0) {
        String candidate = projectPath + "/main." + meta.variant + ".py";
        if (safeFileExists(candidate.c_str()))
            scriptPath = candidate;
    }
    if (scriptPath.length() == 0 && meta.script.length() > 0) {
        String candidate = meta.script.startsWith("/") ? meta.script
                                                       : (projectPath + "/" + meta.script);
        if (safeFileExists(candidate.c_str()))
            scriptPath = candidate;
    }
    if (scriptPath.length() == 0) {
        String candidate = projectPath + "/main.py";
        if (safeFileExists(candidate.c_str()))
            scriptPath = candidate;
    }

    if (scriptPath.length() == 0) {
        // No script is a legitimate project shape (wiring only) - the circuit
        // is live, so fall through to the keep-prompt rather than unwinding.
        Serial.println("  (no companion script - wiring only)");
    } else {
        String content;
        File f = safeFileOpen(scriptPath.c_str(), "r");
        if (!f) {
            Serial.println("  Failed to open " + scriptPath);
        } else {
            content = f.readString();
            safeFileClose(f, false);
        }

        if (content.length() == 0) {
            Serial.println("  Script is empty: " + scriptPath);
        } else {
            // Companion-script contract: the wiring is loaded and routed, temp
            // slot 8 is active, and `_jl_project` names where it all came from.
            String preamble = "_jl_project = {\"dir\": \"" + pyStringSafe(meta.dir) +
                              "\", \"variant\": \"" + pyStringSafe(meta.variant) +
                              "\", \"wiring\": \"" + pyStringSafe(wiringPath) + "\"}\n";
            content = preamble + content;

            // The screen-clear / stream / encoder-settle dance from
            // runPythonScriptFromPath (Apps.cpp:198). Replicated rather than
            // shared: that helper takes a PATH and reads the file itself, and
            // what we have to run is generated CONTENT.
            Serial.print("\033[2J\033[H");
            Serial.flush();
            delay(30);
            setGlobalStreamWithInterrupt(&Serial);
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            delay(400);

            Serial.println("\r\nRunning " + scriptPath + " ...\r\n");
            executePythonFileContent(content.c_str());
            Serial.println("\r\n--- script finished ---");
        }
    }

    // --- keep-prompt ---------------------------------------------------------
    waitForButtonRest(2000);  // the clickwheel hold that ended the script
    notify("Keep this\ncircuit?", "\n\r  Keep this circuit? (click Yes to save it to a slot)", 0);

    int keep = yesNoMenu(15000);  // 1 = yes, 0 = no, -1 = cancel/timeout
    if (keep == 1) {
        int slot = pickDestinationSlot();
        if (slot >= 0) {
            String saveErr;
            if (mgr.saveSlot(slot, saveErr)) {
                // EXIT 7 (the keep path), in exactly this order - see the file
                // header's save-order note.
                mgr.exitTemporarySlot(false);
                String loadErr;
                if (!mgr.loadSlot(slot, loadErr))
                    Serial.println("  Saved, but reloading slot " + String(slot) +
                                   " failed: " + loadErr);
                refreshConnections(-1);
                notify("Saved to\nslot " + String(slot),
                       "\n\r  Saved to slot " + String(slot), 1200);
                return;
            }
            // EXIT 8: save failed. Do NOT continue the keep sequence - that
            // would drop the circuit and load a stale slot. Fall through to the
            // decline path so the user's original slot comes back intact.
            Serial.println("  Save failed: " + saveErr);
        }
        // slot < 0: cancelled at the slot picker -> decline path below.
    }

    // EXIT 9: declined / timed out / cancelled at the slot picker / save
    // failed. Full restore: original slot tracking, state and hardware.
    mgr.exitTemporarySlot();
    notify("Restored", "\n\r  Not kept - restored the previous slot.", 900);
}
