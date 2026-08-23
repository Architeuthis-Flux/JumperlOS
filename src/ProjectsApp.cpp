// SPDX-License-Identifier: MIT
// Guides launcher - the "Guides" TOP-LEVEL menu row (named "Projects"
// through wave 2; the directory is still /projects) that
// turns /projects/<dir>/ into a running circuit. Design:
// CodeDocs/DESIGN_PROJECTS_SUBSYSTEM.md §1, reworked by
// .superpowers/sdd/projects-wave-2-bench-notes/design-launcher.md.
//
// Flow (JL_PROJECT_RUN_HISTORY = 0, the default - ONE run file per project):
//   listProjects -> project picker
//   -> <dir>_run.yaml absent? variant picker (only when >1 genuine
//      wiring*.yaml) -> projectBeginRun copies the wiring into it. No prompt.
//   -> <dir>_run.yaml present, guided build MID-FLIGHT in it? ONE prompt:
//      resume / start fresh (overwrites from the template) / cancel.
//   -> <dir>_run.yaml present otherwise (finished guide, non-guided, plain
//      state)? SILENTLY reopen it. No prompt. Keeping a run is Slots>save-to.
//   -> guided?  guideRun on the CANONICAL wiring path (never the run file)
//      not guided? run main[.variant].py with `_jl_project` injected
//   -> the run file stays the active context when the launcher returns.
//
// Flow (JL_PROJECT_RUN_HISTORY = 1 - the wave-2 numbered scheme, kept
// compiling so the behaviour can come back on a build flag):
//   ... -> scanRunFiles: runs exist? load-latest / start-new prompt
//   -> start-new: variant picker -> <dir>_<N+1>.yaml
//   -> load-latest: open <dir>_<maxN>.yaml, variant resolved from runSource
//
// ONE LATCH runs through this file now: Menus::inClickMenu. Core 1 only
// renders b.print() text while it is 1 (main.cpp:1817) and only renders NETS
// while it is 0 (main.cpp:1746) - and doMenuAction clears it before running an
// app (Menus.cpp:463). So the pickers raise it for their own duration and drop
// it again (plus b.clear() + requestLedShow(-1), the
// pickPythonScriptFromClickMenu exit idiom) BEFORE the project's nets need to
// show.
//
// The SECOND latch is gone. The non-guided path used to borrow temp slot 8
// and end in a keep-prompt plus a destination-slot picker; the run file IS the
// persistence now, so the temp-slot enter/exit pair, the slot picker and the
// whole keep sequence were deleted from this file - the temp-slot latch is not
// touched here at all any more, by name or by call. (The machinery itself
// survives for the calibration apps: Apps.cpp and SelfTest.cpp still use it.)
//
// CANCEL CONTRACT (design-launcher §2.2, the exit table): no state is touched
// until every prompt has been answered, so any cancel before the run file
// loads is a pure return. After it loads there is nothing to restore -
// persistence is the feature, exactly like clicking a YAML in Files.

#include "ProjectsApp.h"

#include "Commands.h"         // requestLedShow, refreshConnections
#include "FileCache.h"        // fileCacheFlushNowAll (context switch)
#include "FilesystemStuff.h"  // safeFile*, FatFS Dir walk
#include "Graphics.h"         // b.print (no-op on OG)
#include "GuidedFlow.h"       // guideRun / GuideRunResult
#include "JumperlOS.h"        // jOS.serviceInner()
#include "Menus.h"            // inClickMenu, yesNoMenu
#include "Peripherals.h"      // setTopRail / setBotRail / setDac*voltage (rail restore)
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

// Copy chunk for copyFileRaw. File::readString() is reserved for scripts:
// wiring files run to several KB and the chunked loop is cheaper on heap.
static const size_t RUNFILE_COPY_CHUNK = 512;

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

// "/projects/555/wiring.yaml" -> "wiring.yaml"
static String baseNameOfPath(const String& path) {
    int last = path.lastIndexOf('/');
    return (last < 0) ? path : path.substring(last + 1);
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

static bool allDigits(const String& s) {
    if (s.length() == 0)
        return false;
    for (unsigned int i = 0; i < s.length(); i++) {
        if (s.charAt(i) < '0' || s.charAt(i) > '9')
            return false;
    }
    return true;
}

// Is `name` this directory's run-file pattern, `<dir>_<1..4 digits>.yaml`?
// nOut receives the run number when it matches. The suffix test is
// case-insensitive; the prefix must match `<dir>_` EXACTLY, which is what
// keeps scanRunFiles from ever eating a variant (design §3).
static bool runFileNumber(const String& name, const String& dir, int& nOut) {
    nOut = 0;
    String prefix = dir + "_";
    if (!name.startsWith(prefix))
        return false;
    String lower = name;
    lower.toLowerCase();
    if (!lower.endsWith(".yaml"))
        return false;
    int midStart = (int)prefix.length();
    int midEnd = (int)name.length() - 5;   // strlen(".yaml")
    if (midEnd <= midStart)
        return false;
    String mid = name.substring(midStart, midEnd);
    // 1-4 digits. A 5+ digit file is deliberately IGNORED by the scan (and so
    // can never be "latest") - the cap is what keeps the name <= dir+5 chars.
    if (mid.length() > 4 || !allDigits(mid))
        return false;
    long n = mid.toInt();
    if (String(n) != mid)                  // no leading zeros
        return false;
    if (n < 1 || n > PROJECT_RUN_MAX_N)
        return false;
    nOut = (int)n;
    return true;
}

// Is `name` this directory's SINGLE run file, `<dir>_run.yaml`? Same shape as
// runFileNumber's test - exact `<dir>_` prefix, case-insensitive extension -
// so the two exclusions behave identically on both sides of the namespace.
//
// COLLISION PROOF, both directions:
//   - vs. the numbered pattern: the middle here is the literal "run", which
//     allDigits() rejects, so no `<dir>_run.yaml` is ever a numbered run file
//     and no `<dir>_<N>.yaml` is ever this one. A project directory called
//     `Xrun` is not a counterexample: its single run file is `Xrun_run.yaml`,
//     whose middle is still "run", and its numbered files are `Xrun_<N>.yaml`.
//   - vs. the variant glob `wiring*.yaml`: `<dir>_run.yaml` starts with the
//     directory name, so it can only start with "wiring" when the DIRECTORY
//     does - which projectBeginRun refuses outright (and which is also what
//     would make SlotManager::isTemplatePath() false-positive on it).
//     listVariantFiles skips it anyway, belt and braces.
static bool isSingleRunFileName(const String& name, const String& dir) {
    String want = dir + PROJECT_RUN_SUFFIX;
    if (!name.startsWith(want))
        return false;
    String lower = name;
    lower.toLowerCase();
    return lower.length() == want.length() + 5 && lower.endsWith(".yaml");
}

String projectRunFilePath(const String& dir) {
    return String(PROJECTS_DIR) + "/" + dir + "/" + dir + PROJECT_RUN_SUFFIX ".yaml";
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
    // TRUNCATE-BEFORE-SORT, deliberately: the bound stops the WALK, and the
    // alphabetical sort below only orders what survived it. With more than
    // PROJECTS_MAX project directories on the board, the ones kept are the
    // first PROJECTS_MAX in FatFS directory order (which follows creation),
    // NOT the alphabetically-first PROJECTS_MAX - so adding a 13th project
    // hides a directory that sorts early rather than one that sorts late.
    // Sorting first would need the whole directory in RAM, which is what
    // this fixed String[PROJECTS_MAX] exists to avoid inside the Core-1
    // pause. Raise PROJECTS_MAX (ProjectsApp.h) if that ever bites.
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
//
// EXCLUDES *_original* : initializeProjects(true) parks this build's default
// beside a user-edited file as wiring_original.yaml / wiring_original1.yaml
// (FilesystemStuff.cpp, the forced-refresh branch). Those match "wiring*.yaml"
// but are backup copies, not variants - listing one would offer the user a
// bogus picker entry that loads the firmware default OVER the edit they were
// deliberately given a backup of.
//
// ALSO EXCLUDES this directory's RUN FILES - BOTH spellings, `<dir>_run.yaml`
// and `<dir>_<N>.yaml`. The namespaces are disjoint unless the project
// directory is itself named `wiring*` (where `wiring_run.yaml` /
// `wiring_1.yaml` match both patterns); projectBeginRun refuses to create
// runs in such a directory, and this is the matching guard on the variant
// side (design-launcher §3, belt and braces). The `_run` half is checked in
// both compile modes: an old numbered board can be reflashed to either, and a
// leftover file must never surface as a variant.
static int listVariantFiles(const String& projectPath, const String& dir,
                            String* outNames, int maxOut) {
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
        if (lower.indexOf("_original") >= 0)
            continue;
        int runN = 0;
        if (runFileNumber(name, dir, runN) || isSingleRunFileName(name, dir))
            continue;   // a run file in a `wiring*`-named project dir
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
// a guided project?" test. Deliberately not the full parser: the guide runtime
// re-reads the file itself.
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

// ============================================================================
// Run files (design-launcher §1)
// ============================================================================

int projectScanRunFiles(const String& projectPath, const String& dir, int& countOut) {
    countOut = 0;
    int maxN = 0;

    // The listVariantFiles walk verbatim: FatFS.openDir under a Core-1 pause
    // and NOTHING else inside it.
    bool was_paused = pauseCore2ForFlash(100);
    Dir d = FatFS.openDir(projectPath);
    while (d.next()) {
        if (d.isDirectory())
            continue;
        int n = 0;
        if (!runFileNumber(d.fileName(), dir, n))
            continue;
        countOut++;
        if (n > maxN)
            maxN = n;
    }
    unpauseCore2ForFlash(was_paused);
    return maxN;
}

#if JL_PROJECT_RUN_HISTORY
static String runFilePath(const String& projectPath, const String& dir, int n) {
    return projectPath + "/" + dir + "_" + String(n) + ".yaml";
}
#endif

// Verbatim byte copy, chunked. Deletes a partial destination on any failure -
// exit D's "partial file deleted" half.
static bool copyFileRaw(const String& src, const String& dst, String& err) {
    File in = safeFileOpen(src.c_str(), "r");
    if (!in) {
        err = "cannot read " + src;
        return false;
    }
    File out = safeFileOpen(dst.c_str(), "w");
    if (!out) {
        safeFileClose(in, false);
        err = "cannot create " + dst;
        return false;
    }

    static uint8_t buf[RUNFILE_COPY_CHUNK];
    bool ok = true;
    while (in.available()) {
        size_t got = in.read(buf, RUNFILE_COPY_CHUNK);
        if (got == 0)
            break;
        if (out.write(buf, got) != got) {
            ok = false;
            err = "short write to " + dst;
            break;
        }
    }
    safeFileClose(in, false);
    safeFileClose(out, true);
    if (!ok)
        safeFileDelete(dst.c_str());
    return ok;
}

// True when the manager sits in the deliberate NO-ACTIVE-CONTEXT terminal
// state (task-4 report, fix round 2 / NEW-3): activeSlotNumber == -1 with an
// empty path, nothing routed, nothing powered beyond defaults, nothing
// auto-savable. For a FILE context this is the DETERMINISTIC outcome of
// re-loading a path whose YAML is invalid (a host MSC edit over the active run
// file lands here every time - the restore re-reads the same bad file and
// arbitrary paths have no .bak mirror). Not an error to fight; a state to
// name.
static bool noActiveContext(SlotManager& mgr) {
    return mgr.getActiveSlot() == -1 && mgr.getActiveSlotPath()[0] == '\0';
}

static void reportIfNoActiveContext(SlotManager& mgr) {
    if (!noActiveContext(mgr))
        return;
    Serial.println("\r\n  No active context - the previous one could not be "
                   "restored either. Nothing is routed or powered.");
    Serial.println("  Load a slot ('<0') or click a YAML in Files to continue.");
    Serial.flush();
}

// Flush a dirty OUTGOING context to its OWN file before it is replaced. Done
// at the CALL SITE, never inside loadSlotFromPath - forcing a save inside the
// API is exactly what the boot firstLoop guard exists to prevent
// (FilesystemStuff.cpp's FileManager click path makes the same argument).
static void flushActiveContextIfDirty(SlotManager& mgr) {
    if (!mgr.getActiveState().isDirty())
        return;
    String saveErr;
    if (!mgr.saveActiveSlot(saveErr)) {
        // A read-only template context refuses here by design (task 4's
        // single write door). Say so and carry on - the incoming context is
        // about to replace it anyway.
        Serial.println("\n\r  (could not save the current context: " + saveErr + ")");
    }
}

bool projectBeginRun(const String& dir, const String& templatePath,
                     String& runPathOut, String& err, bool deferPower) {
    runPathOut = "";

    // Namespace refusal (design §3 / task-4 review Q4). PREFIX, not equality:
    // SlotManager::isTemplatePath() false-positives on
    // /projects/wiringX/wiringX_1.yaml, so a run file in a `wiring*`-named
    // directory would be treated as a read-only template and never save. The
    // `slot*` half keeps run files out of a name that reads as a slot file.
    String lowerDir = dir;
    lowerDir.toLowerCase();
    if (dir.length() == 0 || lowerDir.startsWith("wiring") || lowerDir.startsWith("slot")) {
        err = "project directory '" + dir +
              "' cannot hold run files (names starting with wiring/slot are reserved)";
        return false;
    }

    if (!safeFileExists(templatePath.c_str())) {
        err = "no such wiring: " + templatePath;
        return false;
    }

    String projectPath = String(PROJECTS_DIR) + "/" + dir;

    // The only thing the two modes disagree about is WHICH FILE gets written;
    // everything below - the flush, the copy, the validate-by-load, the one
    // retry, the runSource stamp - is shared, which is the point of picking
    // the path here rather than forking the function.
#if JL_PROJECT_RUN_HISTORY
    int count = 0;
    int maxN = projectScanRunFiles(projectPath, dir, count);
    if (maxN >= PROJECT_RUN_MAX_N) {
        err = "run number cap reached for " + dir + " - delete old runs";
        return false;
    }
    String runPath = runFilePath(projectPath, dir, maxN + 1);
#else
    // Single-file mode: ONE name, and this call is BOTH "create it" and
    // "start fresh over it". An existing file is overwritten deliberately -
    // the caller only gets here after the mid-flight prompt was answered `n`,
    // or when there was nothing unfinished to protect.
    String runPath = projectRunFilePath(dir);
#endif

    SlotManager& mgr = SlotManager::getInstance();

    flushActiveContextIfDirty(mgr);
    // Big-event flush of the OUTGOING context's cache before its state is
    // replaced (the loadSlot precedent, States.cpp:2918).
    fileCacheFlushNowAll("project_begin_run");

    // Copy + validate-by-load, one retry. A load failure deletes the file we
    // just wrote and tries again; the second failure leaves the previous
    // context untouched, because loadSlotFromPath is atomic on BOTH open and
    // parse failure.
    //
    // NOTE what "the file we just wrote" means in SINGLE-FILE mode: design
    // §6.1's rule used to be "never delete something we did not create", and
    // here it reads "created OR OVERWROTE in this session". A start-fresh
    // lands on an existing <dir>_run.yaml, so a double failure deletes a file
    // that was the user's a moment ago - but its old bytes were already gone
    // at the FIRST copy, and the user consented to that at the mid-flight
    // prompt (or there was nothing unfinished to protect). Leaving the corpse
    // would only leave an unloadable file behind. ProjectsApp.h's header
    // comment states the same thing.
    for (int attempt = 0; attempt < 2; attempt++) {
        String cerr;
        if (!copyFileRaw(templatePath, runPath, cerr)) {
            err = cerr;
            if (attempt == 1)
                return false;
            continue;
        }
        String lerr;
        // Re-armed per attempt: loadSlotFromPath latches and clears the flag,
        // so a second try would otherwise energize what the first deferred.
        slotLoadDeferPowerApply = deferPower;
        if (mgr.loadSlotFromPath(runPath, lerr)) {
            // Stamp the variant provenance the path itself cannot encode, and
            // persist it right away so a reboot resolves the same script.
            JumperlessState& st = mgr.getActiveState();
            strncpy(st.runSource, templatePath.c_str(), sizeof(st.runSource) - 1);
            st.runSource[sizeof(st.runSource) - 1] = '\0';
            st.markDirty();
            String serr;
            if (!mgr.saveActiveSlot(serr, /*skipValidation=*/true)) {
                Serial.println("\r\n  (could not stamp runSource in " + runPath +
                               ": " + serr + ")");
            }
            runPathOut = runPath;
            return true;
        }
        err = lerr;
        safeFileDelete(runPath.c_str());
    }
    reportIfNoActiveContext(mgr);
    return false;
}

// Open an existing run file as the active context. Same atomicity guarantee,
// and the same deferPower contract as projectBeginRun above.
static bool projectOpenRunFile(const String& runPath, String& err,
                               bool deferPower = false) {
    SlotManager& mgr = SlotManager::getInstance();
    flushActiveContextIfDirty(mgr);
    fileCacheFlushNowAll("project_open_run");
    slotLoadDeferPowerApply = deferPower;
    if (mgr.loadSlotFromPath(runPath, err)) {
        return true;
    }
    reportIfNoActiveContext(mgr);
    return false;
}

// Is a guided build MID-FLIGHT in this run file? The single-file launcher's
// one prompt hangs off this, and it has to be answerable WITHOUT loading:
// the prompt offers `cancel`, and a cancel must leave the previous context
// untouched (the exit table's row B). So it is a header-only text scan -
// SlotManager::scanGuideProgressFile, which shares its flow-map reader with
// fromYAML's parse arm.
//
// `of:` (the step total, written by the guide runtime alongside the step) is
// what makes this decidable at all: numSteps cannot be recomputed here,
// because guideParse resolves `part:` names - and synthesizes auto steps -
// against the LIVE parts table, which at this moment still holds the previous
// context. A file that does not carry `of:` therefore reads as "unknown", and
// unknown means NOT mid-flight: reopen it silently and let the existing
// resume gates in runOpenedRunFile do exactly what they always did. Both
// error directions are benign - under-detect resumes (nothing is overwritten,
// which is the whole risk), over-detect costs one prompt whose default
// (y/click) is resume - and guideRun's ALREADY_COMPLETE clamp is still the
// backstop against a file that lies.
static bool runFileMidFlight(const String& runPath, int& stepOut, int& totalOut) {
    stepOut = 0;
    totalOut = 0;
    String src;
    if (!SlotManager::scanGuideProgressFile(runPath.c_str(), &src, &stepOut, &totalOut))
        return false;
    if (src.length() == 0 || totalOut <= 0)
        return false;
    if (stepOut < 0)
        stepOut = 0;
    return stepOut < totalOut;
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

// Encoder picker shared by the project / variant pickers. Returns the chosen
// index, or -1 when the user cancelled (encoder hold or a serial byte).
//
// Owns inClickMenu for its duration (see the file header) and resets the
// button state machine on entry so the click that launched the app - or the
// click that made the previous selection - can't fall through into this loop.
//
// machineTag is printed once on entry as `<TAG> n=<count>`: one grep-able line
// per picker so a headless caller can see the picker open and how many entries
// it holds (test_projects.py phase 6(d) drives the launcher on that line).
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
    // getMenuSelection, like run_app("Guides") - would otherwise land here.
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

// After a script or a guide ends the user is often still holding the clickwheel
// (holding is how you interrupt a script - Python_Proper.cpp:5016 raises
// KeyboardInterrupt). Wait for the button to come back to rest so the next
// prompt doesn't read that same hold as "cancel" the instant it opens.
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

// yesNoMenu treats ANY non-y/n byte as cancel, and a bare line terminator IS
// such a byte: the `z ...\r\n` command line leaves its '\n' in the RAW Serial
// buffer, a MicroPython input() consumes through the '\r' and leaves the '\n',
// and a stray Enter tapped while a script streamed sits there too. Either one
// would silently cancel a prompt before anyone saw it (bench-reproduced).
// Swallow LINE ENDINGS ONLY - a real y/n/other answer already in flight must
// survive.
static void drainLineEndings(void) {
    delay(20);
    while (Serial.available() > 0) {
        int p = Serial.peek();
        if (p == '\r' || p == '\n') {
            Serial.read();
            continue;
        }
        break;
    }
}

// ============================================================================
// The shared inner flow: run file -> guide -> script
// ============================================================================

// Everything the flow needs to know about the run it is about to perform.
struct RunContext {
    String dir;          // project directory name
    String projectPath;  // /projects/<dir>
    String runPath;      // the project's run file (now the active context)
    String wiringPath;   // the CANONICAL wiring this run came from (runSource)
    ProjectMeta meta;    // meta: of wiringPath (variant/script resolution)
    bool interactive;    // encoder/terminal session (offers prompts) vs headless
    bool noScript;       // headless `noscript`
};

// main.<variant>.py -> meta.script -> main.py. "" when the project ships no
// script, which is a legitimate project shape (wiring only).
static String resolveScriptPath(const RunContext& rc) {
    if (rc.meta.variant.length() > 0) {
        String candidate = rc.projectPath + "/main." + rc.meta.variant + ".py";
        if (safeFileExists(candidate.c_str()))
            return candidate;
    }
    if (rc.meta.script.length() > 0) {
        String candidate = rc.meta.script.startsWith("/")
                               ? rc.meta.script
                               : (rc.projectPath + "/" + rc.meta.script);
        if (safeFileExists(candidate.c_str()))
            return candidate;
    }
    String candidate = rc.projectPath + "/main.py";
    if (safeFileExists(candidate.c_str()))
        return candidate;
    return String("");
}

static void runCompanionScript(const RunContext& rc, const String& scriptPath) {
    File f = safeFileOpen(scriptPath.c_str(), "r");
    if (!f) {
        Serial.println("  Failed to open " + scriptPath);
        return;
    }
    size_t scriptBytes = f.size();
    // Close it right here: MicroPython opens the same path itself below, and
    // nothing in this function ever reads a byte through this handle.
    safeFileClose(f, false);

    // Companion-script contract: the wiring is loaded and routed, the RUN FILE
    // is the active context, and `_jl_project` names where it all came from.
    // "wiring" keeps its canonical-wiring-path value (provenance, unchanged
    // semantics); "run" is new - it is the state file the script's own
    // mutations will be saved into.
    String preamble = "_jl_project = {\"dir\": \"" + pyStringSafe(rc.dir) +
                      "\", \"variant\": \"" + pyStringSafe(rc.meta.variant) +
                      "\", \"wiring\": \"" + pyStringSafe(rc.wiringPath) +
                      "\", \"run\": \"" + pyStringSafe(rc.runPath) + "\"}\n";

    // THE SOURCE NEVER COMES INTO RAM. What we hand MicroPython is a ~250-byte
    // command - the preamble, then `execfile("<path>")` - and MicroPython's
    // lexer streams the script off the filesystem itself. Both statements are
    // one exec, so `_jl_project` is in the same globals the script then reads,
    // and executePythonFileContent's session bookkeeping still runs exactly
    // once around the whole thing.
    //
    // The obvious spelling - `content = f.readString(); content = preamble +
    // content;` - needed TWO full-size Strings alive at once on an Arduino
    // heap that settles around 19 KB free here, so the peak was ~2x the
    // script. Worse, it failed SILENTLY: Arduino's operator+ returns an
    // invalidated String when the allocation fails, the assignment left
    // `content` EMPTY, executePythonFileContent's `!*src` guard returned false
    // without printing, and the terminal showed "Running ... " followed
    // straight by "--- script finished ---". The emptiness check also sat
    // BEFORE the concatenation, so nothing could catch it. Measured on the
    // bench (wave 3): 4.5 KB ran, 6.4 KB ran, 6.6 KB was silent - and the
    // shipped eeprom script is 6441 bytes. Fixing it to ONE reserved buffer
    // was not enough either: a 12.5 KB reserve still failed mid-session with
    // 25 KB free, because that heap has no contiguous block that size.
    // Streaming is the only shape with no ARDUINO-side ceiling at all.
    //
    // ONE BOUND SURVIVES, and it is not this file's to fix: MicroPython's
    // COMPILER still needs heap proportional to the source. Measured on the
    // bench - ~12 KB compiles on a freshly booted board (~39 KB free) and
    // raises MemoryError on the ~25 KB left deep into a long session. That is
    // why test_projects' i2cscrn drive resets the board before it runs a
    // 12 KB companion script, and why the bundled scripts are kept lean. If
    // you are here to change how scripts are launched: the source no longer
    // costs Arduino heap, but its SIZE still costs MicroPython heap, and the
    // failure is a loud traceback rather than the old silence.
    if (scriptBytes == 0) {
        Serial.println("  Script is empty: " + scriptPath);
        return;
    }
    String content = preamble + "execfile(\"" + pyStringSafe(scriptPath) + "\")\n";
    if (content.length() == 0) {
        // Belt and braces: even this little String can fail, and silence is
        // exactly the failure mode this whole block exists to kill.
        Serial.println("  Out of memory launching " + scriptPath + " (" +
                       String(rp2040.getFreeHeap() / 1024) +
                       " KB heap free). Nothing was run.");
        return;
    }

    // The screen-clear / stream / encoder-settle dance from
    // runPythonScriptFromPath (Apps.cpp:198). Replicated rather than shared:
    // that helper runs a bare path, and what we have to run is a path WITH the
    // `_jl_project` preamble in front of it.
    Serial.print("\033[2J\033[H");
    Serial.flush();
    delay(30);
    setGlobalStreamWithInterrupt(&Serial);
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    delay(400);

    Serial.println("\r\nRunning " + scriptPath + " ...\r\n");
    // The return is checked, not discarded: executePythonFileContent answers
    // false WITHOUT printing when initMicroPythonQuiet() fails
    // (Python_Proper.cpp - an mpAllocHeap() failure), which is the last branch
    // that can still reproduce the exact silence this function was rewritten
    // to kill. mpAllocHeap prints its own ladder failures, so this is a
    // backstop rather than the only witness - but "every failure branch
    // prints" has to mean every one.
    if (!executePythonFileContent(content.c_str())) {
        Serial.println("\r\n  (MicroPython would not start - nothing was run)");
    }
    Serial.println("\r\n--- script finished ---");
}

// Save the run file and print the closing one-liner (exit rows G and H).
static void finishRun(const RunContext& rc) {
    SlotManager& mgr = SlotManager::getInstance();
    String err;
    if (!mgr.saveActiveSlot(err, /*skipValidation=*/true)) {
        Serial.println("\r\n  (could not save " + rc.runPath + ": " + err + ")");
    }
    Serial.println("\r\n  Run saved to " + baseNameOfPath(rc.runPath) +
                   " (now your active circuit).");
    Serial.flush();
}

// The companion script, offered or run. `afterGuide` selects Kevin's OFFER
// (exit H) over the unconditional run non-guided launches get.
//
// TWO machine lines on EVERY path, in this order, so the grammar is greppable
// whether or not a script exists and whether or not it ran:
//   SCRIPT offer=<resolved path>|none
//   SCRIPT action=run|skip
// `nothingBuilt` is the guided gate (fix round 1): a build whose every step
// was skipped reaches DONE and returns COMPLETED - nothing is left unfinished -
// but no bridge was ever placed, and running the companion script against a
// circuit nobody assembled is worse than not running it. The run file is still
// saved and announced (the persistence half of exits G/H is unconditional);
// only the offer and the run are suppressed, and no SCRIPT lines are printed,
// so a headless driver can tell this case from `offer=none`.
static void runOrOfferScript(const RunContext& rc, bool afterGuide,
                             bool nothingBuilt = false) {
    if (nothingBuilt) {
        Serial.println("\r\n  (nothing was built - no script offer)");
        Serial.flush();
        finishRun(rc);
        return;
    }

    String scriptPath = resolveScriptPath(rc);

    // Announce the resolution BEFORE any prompt: a headless driver watches for
    // this line and then answers the offer that follows it.
    Serial.println("\r\nSCRIPT offer=" +
                   (scriptPath.length() ? scriptPath : String("none")));
    Serial.flush();

    bool run = (scriptPath.length() > 0) && !rc.noScript;

    if (run && afterGuide && rc.interactive) {
        // Kevin's binding addition: the companion script is OPTIONAL after a
        // guided build. Decline/timeout leaves the run file active and the
        // rails exactly where the guide left them (a completed 555 keeps
        // blinking on hardware alone). Encoder/terminal-interactive only -
        // headless guided-complete runs the script unless `noscript`, because
        // headless has to be deterministic.
        waitForButtonRest(1500);
        notify("Run\nmain.py?",
               "\n\r  Build complete. Run " + baseNameOfPath(scriptPath) +
                   "? y/click = run, n/other/timeout(15s) = done",
               0);
        drainLineEndings();
        int r = yesNoMenu(15000, /*startOption=*/1);   // 1 = yes, 0/-1 = done
        b.clear();
        requestLedShow(-1);
        run = (r == 1);
    }

    Serial.println(run ? "SCRIPT action=run" : "SCRIPT action=skip");
    Serial.flush();

    if (run) {
        runCompanionScript(rc, scriptPath);
        // The clickwheel hold that ended the script must not fall through.
        waitForButtonRest(2000);
    } else if (scriptPath.length() == 0) {
        // No script is a legitimate project shape (wiring only) - the circuit
        // is live and the run file keeps it.
        Serial.println("  (no companion script - wiring only)");
    }
    finishRun(rc);
}

// ---------------------------------------------------------------------------
// The rails across a guided launch (guide-UX design §4, task 7)
// ---------------------------------------------------------------------------
//
// THE POINT: launching a guided project must not disturb the user's bench.
// The rails they had set stay live until the guide's INIT parks everything at
// 0 V, and they come back on the way out unless the project's own power_on
// step took ownership.
//
// CAPTURED at the top of each launcher entry, from LIVE globalState.power -
// the last moment the user's pre-project values still exist, because the very
// next thing either entry does is load a run file over them.
static GuideRunPower preGuidePower;

static void captureUserPower(void) {
    preGuidePower.haveCaptured = true;
    preGuidePower.topRail    = globalState.power.topRail;
    preGuidePower.bottomRail = globalState.power.bottomRail;
    preGuidePower.dac0       = globalState.power.dac0;
    preGuidePower.dac1       = globalState.power.dac1;
    preGuidePower.applied    = false;
}

// save=0 IS DELIBERATE and load-bearing: the destination run file must keep
// the safe 0 V that guideForcePowerSafe(save=1) wrote into it, so a half-built
// project re-opened later still comes up unpowered. Live rails and the file
// diverge on purpose here; the divergence heals the next time the project's
// own power values are committed.
static void restoreUserPower(void) {
    if (!preGuidePower.haveCaptured) return;
    setTopRail(preGuidePower.topRail, 0, 0);
    setBotRail(preGuidePower.bottomRail, 0, 0);
    setDac0voltage(preGuidePower.dac0, 0, 0);
    setDac1voltage(preGuidePower.dac1, 0, 0);
}

// The guide + script half, shared by every entry point. `guideSource` is the
// CANONICAL wiring path (never the run file: the run file loses its `guide:`
// section on the first save, so a guide parsed from it would work once and
// never resume). resumeStep < 0 = fresh.
static void runGuideThenScript(const RunContext& rc, const String& guideSource,
                               int resumeStep) {
    if (resumeStep >= 0) {
        Serial.print("\r\nGUIDE resume file=");
        Serial.print(rc.runPath);
        Serial.print(" step=");
        Serial.println(resumeStep);
        Serial.flush();
    }

    int builtSteps = 0;
    GuideRunResult gr = guideRun(guideSource.c_str(), resumeStep, &preGuidePower,
                                 &builtSteps);
    // Only a session that ran can report "nothing was built". The no-session
    // results below are plain context loads (see the report's §4.1) and keep
    // their script.
    bool nothingBuilt = false;
    switch (gr) {
        case GuideRunResult::QUIT:
        case GuideRunResult::COMPLETED:
            // A SESSION RAN, so the rails ruling governs: power_on applied the
            // project's values -> they are the correct final state and nothing
            // is restored; it never ran -> the user's captured bench comes
            // back. The exit tail has already NAMED whichever it is.
            if (!preGuidePower.applied) restoreUserPower();
            // Exit F additionally means: nothing else to do here. The run file
            // is active with guideProgress at the quit step and resume works
            // next launch.
            if (gr == GuideRunResult::QUIT) return;
            nothingBuilt = (builtSteps < 1);
            break;
        case GuideRunResult::PARSE_FAILED:
            Serial.println("\r\n  guide source missing: " + guideSource +
                           " - start a new run to rebuild");
            applyStatePowerToHardware();
            break;
        case GuideRunResult::NOTHING_TO_DO:
        case GuideRunResult::ALREADY_COMPLETE:
        default:
            // NO SESSION RAN (parse failure, nothing to build, or a finished
            // build being re-opened), so nothing ever took the rails over from
            // the file. This is a plain context load and task 4's guarantee
            // applies: the run file's own power goes to the hardware, which
            // the guided load deliberately deferred. Restoring the user's
            // bench here instead would leave a completed project sitting
            // unpowered while its script runs - see the report's deviation.
            applyStatePowerToHardware();
            break;
    }
    runOrOfferScript(rc, /*afterGuide=*/true, nothingBuilt);
}

// The whole flow once the run file is open and RUNFILE has been printed.
// `freshWiring` is set on the start-new path (the chosen variant); on the
// load-latest path the guide source comes from the loaded state instead.
static void runOpenedRunFile(RunContext& rc, bool fresh) {
    SlotManager& mgr = SlotManager::getInstance();
    JumperlessState& st = mgr.getActiveState();

    // loadSlotFromPath already routed and applied the state; this second pass
    // is the belt-and-braces refresh and doubles as the "put the nets back on
    // the matrix now that the picker released inClickMenu" redraw.
    refreshConnections(-1);

    // --- the guided-ness decision, made BEFORE any power lands -------------
    // The launcher's run-file load ran with its rail/DAC apply DEFERRED
    // (projectBeginRun / projectOpenRunFile's deferPower), because on a guided
    // launch the project's `power:` must never reach the rails ahead of its
    // own power_on step. Everything below therefore has to settle guided-ness
    // first and then either hand the rails to the guide or complete the load.
    //
    // On the FRESH path the wiring still carries `guide:`. On the load-latest
    // path wiringHasGuideSections() cannot help - the run file lost that
    // section on its first save - so there are two ways in:
    //   1. guideProgress survived -> RESUME at the saved step;
    //   2. no progress, but runSource names a wiring that IS guided -> a fresh
    //      guide on this run file. (Deviation from design §1.4, argued in the
    //      task-5 report: the strict "guideSource non-empty" gate makes a run
    //      that was quit before its FIRST commit permanently non-guided, which
    //      would run main.py against a circuit nobody built.)
    String guideSource;
    int resumeStep = -1;
    if (fresh) {
        if (wiringHasGuideSections(rc.wiringPath))
            guideSource = rc.wiringPath;
    } else if (st.parts.guideSource[0] != '\0') {
        guideSource = String(st.parts.guideSource);
        resumeStep = st.parts.guideStep;
        if (resumeStep < 0)
            resumeStep = 0;
    } else if (rc.wiringPath.length() > 0 && wiringHasGuideSections(rc.wiringPath)) {
        guideSource = rc.wiringPath;
    }

    if (guideSource.length() > 0) {
        runGuideThenScript(rc, guideSource, resumeStep);
        return;
    }

    // Not guided: complete the deferred load right here, which is task 4's
    // apply-power-on-load guarantee arriving a few milliseconds later than it
    // used to and otherwise unchanged.
    applyStatePowerToHardware();
    runOrOfferScript(rc, /*afterGuide=*/false);
}

// Fill rc.meta / rc.wiringPath from the loaded state's runSource. Empty or
// dangling runSource falls back to the DEFAULT WIRING with a one-line warning
// (design §1.2) - that wiring is then both the guide source and the variant
// resolution, so the script collapses to main.py as a consequence.
static void resolveVariantFromRunSource(RunContext& rc) {
    SlotManager& mgr = SlotManager::getInstance();
    JumperlessState& st = mgr.getActiveState();
    String src(st.runSource);

    if (src.length() > 0 && safeFileExists(src.c_str())) {
        rc.wiringPath = src;
        readProjectMeta(src, rc.meta);
        rc.meta.dir = rc.dir;
        return;
    }
    if (src.length() > 0) {
        // "the default wiring", not "main.py": the fallback below also sets
        // rc.wiringPath to the project's wiring.yaml, which the load-latest
        // guided arm will happily use as a GUIDE source. Only the variant
        // script resolution collapses to main.py.
        Serial.println("  runSource missing: " + src +
                       " - falling back to the default wiring");
    }
    String fallback = rc.projectPath + "/wiring.yaml";
    rc.wiringPath = safeFileExists(fallback.c_str()) ? fallback : String("");
    rc.meta = ProjectMeta();
    rc.meta.dir = rc.dir;
    if (rc.wiringPath.length() > 0) {
        readProjectMeta(rc.wiringPath, rc.meta);
        rc.meta.dir = rc.dir;
    }
    // A dangling runSource must not resolve a variant script.
    rc.meta.variant = "";
}

static void printRunFileLine(const String& path, const char* action) {
    Serial.print("\r\nRUNFILE path=");
    Serial.print(path);
    Serial.print(" action=");
    Serial.println(action);
    Serial.flush();
}

// ============================================================================
// Interactive entry: the launcher's inner flow for one chosen project
// ============================================================================

// What the run-file decision came to. REOPEN carries the path to open; FRESH
// means "copy the (possibly picked) wiring in"; CANCEL means nothing was
// touched and nothing will be.
enum class RunFileChoice : uint8_t { CANCEL, REOPEN, FRESH };

// The run-file decision, prompts included. Split out because it is the ONE
// place the two compile modes genuinely differ, and burying a 60-line #if in
// the middle of projectRunInteractive made both halves unreadable.
static RunFileChoice chooseRunFile(RunContext& rc, const String& dir,
                                   String& reopenPath) {
    reopenPath = "";

#if JL_PROJECT_RUN_HISTORY
    int runCount = 0;
    int maxN = projectScanRunFiles(rc.projectPath, dir, runCount);

    // --- the merged load-latest / start-new prompt (design §1.3) ------------
    // The run scan comes BEFORE the variant picker, deliberately: run files
    // are not partitioned by variant, so "load latest" must not be offered
    // after a variant choice it might contradict. Loading resolves its variant
    // from runSource instead.
    if (maxN < 1)
        return RunFileChoice::FRESH;

    String latest = runFilePath(rc.projectPath, dir, maxN);
    Serial.print("\r\nRUNS n=");
    Serial.print(runCount);
    Serial.print(" latest=");
    Serial.println(latest);
    Serial.flush();

    notify("Load run " + String(maxN) + "?\nNo = new run",
           "\n\r  " + dir + ": " + String(runCount) + " previous run" +
               (runCount == 1 ? "" : "s") + ". y/click Yes = load latest (" +
               baseNameOfPath(latest) + "), n = start new (" +
               String(dir) + "_" + String(maxN + 1) + ".yaml), other = cancel",
           0);
    drainLineEndings();
    int r = yesNoMenu(20000, /*startOption=*/1);
    b.clear();
    requestLedShow(-1);
    if (r < 0) {
        // EXIT B: cancel or timeout at the load/new prompt. Nothing was
        // touched.
        Serial.println("  Cancelled.");
        return RunFileChoice::CANCEL;
    }

    if (runCount >= PROJECT_RUN_PILEUP_HINT) {
        Serial.println("  (" + String(runCount) + " run files in " + rc.projectPath +
                       " - old runs can be deleted from Files)");
    }
    if (r == 1) {
        reopenPath = latest;
        return RunFileChoice::REOPEN;
    }
    return RunFileChoice::FRESH;
#else
    // SINGLE-FILE MODE. One name, silently reused; the ONLY prompt is the one
    // that protects an unfinished guided build from being overwritten.
    (void)rc;   // no directory scan here - the name is known
    String runPath = projectRunFilePath(dir);
    if (!safeFileExists(runPath.c_str())) {
        // Nothing to reuse and nothing to protect: create and go, no prompt.
        // No RUNS line either - there are no runs to count.
        return RunFileChoice::FRESH;
    }

    // RUNS stays truthful: in this mode there is exactly one, and it is both
    // the latest and the only.
    Serial.print("\r\nRUNS n=1 latest=");
    Serial.println(runPath);
    Serial.flush();

    int step = 0, total = 0;
    if (!runFileMidFlight(runPath, step, total)) {
        // Finished guide, non-guided, or plain state -> SILENTLY reopen.
        reopenPath = runPath;
        return RunFileChoice::REOPEN;
    }

    notify("Resume build?\nNo = start over",
           "\n\r  " + dir + ": an unfinished guided build is in " +
               baseNameOfPath(runPath) + " (step " + String(step + 1) + " of " +
               String(total) + "). y/click Yes = resume, n = start fresh "
               "(OVERWRITES it from the project wiring), other = cancel",
           0);
    drainLineEndings();
    int r = yesNoMenu(20000, /*startOption=*/1);
    b.clear();
    requestLedShow(-1);
    if (r < 0) {
        // EXIT B, unchanged: cancel or timeout before anything was touched.
        Serial.println("  Cancelled.");
        return RunFileChoice::CANCEL;
    }
    if (r == 1) {
        reopenPath = runPath;
        return RunFileChoice::REOPEN;
    }
    Serial.println("  (starting fresh - " + baseNameOfPath(runPath) +
                   " is rewritten from the project wiring)");
    return RunFileChoice::FRESH;
#endif
}

// `forcedWiring` != "" pins the variant (the Files-browser click on a specific
// wiring*.yaml); the variant picker is then skipped entirely.
static void projectRunInteractive(const String& dir, const String& forcedWiring,
                                  const ProjectMeta& listedMeta) {
    // Before anything is loaded: the user's own rails are still live in
    // globalState.power, and this is the only moment they are.
    captureUserPower();

    RunContext rc;
    rc.dir = dir;
    rc.projectPath = String(PROJECTS_DIR) + "/" + dir;
    rc.interactive = true;
    rc.noScript = false;
    rc.meta = listedMeta;
    rc.meta.dir = dir;

    String runPath;
    RunFileChoice choice = chooseRunFile(rc, dir, runPath);
    if (choice == RunFileChoice::CANCEL)
        return;   // EXIT B: nothing touched, nothing to restore

    if (choice == RunFileChoice::REOPEN) {
        // A Files-browser click names a VARIANT, and a reopen does not honour
        // it - the run file's own runSource decides, exactly as on the
        // headless load path. Say so rather than ignoring it silently; this
        // is the one cell of the mode matrix where the door the user came
        // through has less say than the file they land in.
        if (forcedWiring.length() > 0) {
            Serial.println("  (variant taken from runSource; the clicked file was "
                           "not used - start fresh to change variant)");
        }
        String err;
        if (!projectOpenRunFile(runPath, err, /*deferPower=*/true)) {
            // EXIT E: both the load and (for a parse failure) the restore are
            // handled inside loadSlotFromPath. The previous context is still
            // active unless the terminal state was reported above.
            notify("Load\nfailed", "\n\r  Failed to load " + runPath + ": " + err, 2000);
            Serial.println("  (start a new run to rebuild from the project wiring)");
            return;
        }
        rc.runPath = runPath;
        printRunFileLine(runPath, "load");
        resolveVariantFromRunSource(rc);
        runOpenedRunFile(rc, /*fresh=*/false);
        return;
    }

    // --- start fresh: variant picker, then write the run file ---------------
    // In single-file mode this OVERWRITES <dir>_run.yaml; in numbered mode it
    // allocates <dir>_<N+1>.yaml. Either way the wiring choice happens here,
    // after the run-file decision, because a reopen would have ignored it.
    String wiringPath = forcedWiring;
    if (wiringPath.length() == 0) {
        wiringPath = rc.projectPath + "/wiring.yaml";
        String variantFiles[PROJECT_VARIANTS_MAX];
        int variantCount = listVariantFiles(rc.projectPath, dir, variantFiles,
                                            PROJECT_VARIANTS_MAX);
        if (variantCount > 1) {
            static String vLed[PROJECT_VARIANTS_MAX];
            static String vTitle[PROJECT_VARIANTS_MAX];
            static String vSummary[PROJECT_VARIANTS_MAX];
            static ProjectMeta vMeta[PROJECT_VARIANTS_MAX];
            for (int i = 0; i < variantCount; i++) {
                String path = rc.projectPath + "/" + variantFiles[i];
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
                // EXIT C: cancelled before any state was touched.
                Serial.println("  Cancelled.");
                return;
            }
            wiringPath = rc.projectPath + "/" + variantFiles[v];
            rc.meta = vMeta[v];
            rc.meta.dir = dir;
        }
    } else {
        readProjectMeta(wiringPath, rc.meta);
        rc.meta.dir = dir;
    }

    String err;
    if (!projectBeginRun(dir, wiringPath, runPath, err, /*deferPower=*/true)) {
        // EXIT D / E: the copy or the load failed (the partial file is already
        // deleted, one retry already spent). Previous context untouched.
        notify("Run file\nfailed", "\n\r  Could not start a run of " + dir + ": " + err,
               2000);
        return;
    }
    rc.runPath = runPath;
    rc.wiringPath = wiringPath;
    printRunFileLine(runPath, "new");
    runOpenedRunFile(rc, /*fresh=*/true);
}

void projectRunFromTemplate(const String& wiringPath) {
    String dir = projectDirOfPath(wiringPath);
    ProjectMeta meta;
    readProjectMeta(wiringPath, meta);
    meta.dir = dir;
    projectRunInteractive(dir, wiringPath, meta);
}

// ============================================================================
// Headless entries (the `z` command, MicroPython load_project)
// ============================================================================

// Split "<project>" into (dir, templatePath). A '/' anywhere makes it a path.
static bool resolveProjectArg(const String& project, String& dirOut,
                              String& templateOut) {
    if (project.indexOf('/') >= 0) {
        dirOut = projectDirOfPath(project);
        templateOut = project;
    } else {
        dirOut = project;
        templateOut = String(PROJECTS_DIR) + "/" + project + "/wiring.yaml";
    }
    return dirOut.length() > 0 && templateOut.length() > 0;
}

bool runProjectHeadless(const String& project, ProjectRunMode mode, int runN,
                        bool noScript) {
    String dir, templatePath;
    if (!resolveProjectArg(project, dir, templatePath)) {
        Serial.println("PROJECT error cannot resolve '" + project + "'");
        return false;
    }

    // Same capture point as the interactive entry, for the same reason - the
    // headless door (`z`, and load_project's name form via the launcher) must
    // give the bench back too.
    captureUserPower();

    RunContext rc;
    rc.dir = dir;
    rc.projectPath = String(PROJECTS_DIR) + "/" + dir;
    rc.interactive = false;
    rc.noScript = noScript;

#if JL_PROJECT_RUN_HISTORY
    int runCount = 0;
    int maxN = projectScanRunFiles(rc.projectPath, dir, runCount);
    bool haveRun = (maxN >= 1);
#else
    (void)runN;
    String singlePath = projectRunFilePath(dir);
    bool haveRun = safeFileExists(singlePath.c_str());

    // `run=<N>` is the OTHER mode's grammar. Name the mode rather than
    // guessing what the caller meant - a scripted driver that asks for a
    // specific numbered run on a single-file build has a stale assumption,
    // and silently opening <dir>_run.yaml instead would hide it. This line is
    // also the suites' mode probe.
    if (mode == ProjectRunMode::RUN_N) {
        Serial.println("PROJECT error run=<N> needs a JL_PROJECT_RUN_HISTORY build - "
                       "this build keeps ONE run file per project (" + dir +
                       PROJECT_RUN_SUFFIX ".yaml)");
        return false;
    }
#endif

    // DEFAULT: reuse the existing run file, else new - the launcher's own
    // defaults WITHOUT the prompt (headless has to be deterministic, so a
    // mid-flight guided build RESUMES here instead of asking).
    ProjectRunMode effective = mode;
    if (effective == ProjectRunMode::DEFAULT)
        effective = haveRun ? ProjectRunMode::LOAD : ProjectRunMode::NEW;

    // `z <wiring path>` with an EXPLICIT load/run=<N> is refused by the parser
    // (SingleCharCommands.cpp) precisely because the path names a variant the
    // run file's own runSource would override. The bare form reaches the same
    // place silently whenever a run already exists, so say so instead: the path
    // argument selected nothing, and `... new` is the spelling that honours it.
    if (mode == ProjectRunMode::DEFAULT && effective == ProjectRunMode::LOAD &&
        project.indexOf('/') >= 0) {
        Serial.println("  (variant taken from runSource; the path argument was not used)");
    }

    if (effective == ProjectRunMode::LOAD || effective == ProjectRunMode::RUN_N) {
#if JL_PROJECT_RUN_HISTORY
        int wantN = (effective == ProjectRunMode::LOAD) ? maxN : runN;
        Serial.print("\r\nRUNS n=");
        Serial.print(runCount);
        if (maxN >= 1) {
            Serial.print(" latest=");
            Serial.print(runFilePath(rc.projectPath, dir, maxN));
        }
        Serial.println();
        Serial.flush();
        if (wantN < 1) {
            Serial.println("PROJECT error no run files for " + dir);
            return false;
        }
        String runPath = runFilePath(rc.projectPath, dir, wantN);
#else
        Serial.print("\r\nRUNS n=");
        Serial.print(haveRun ? 1 : 0);
        if (haveRun) {
            Serial.print(" latest=");
            Serial.print(singlePath);
        }
        Serial.println();
        Serial.flush();
        if (!haveRun) {
            Serial.println("PROJECT error no run files for " + dir);
            return false;
        }
        String runPath = singlePath;
#endif
        if (!safeFileExists(runPath.c_str())) {
            Serial.println("PROJECT error no such run file: " + runPath);
            return false;
        }
        String err;
        if (!projectOpenRunFile(runPath, err, /*deferPower=*/true)) {
            Serial.println("PROJECT error load failed: " + err);
            return false;
        }
        rc.runPath = runPath;
        printRunFileLine(runPath, "load");
        resolveVariantFromRunSource(rc);
        runOpenedRunFile(rc, /*fresh=*/false);
        return true;
    }

    // NEW
    String runPath, err;
    if (!projectBeginRun(dir, templatePath, runPath, err, /*deferPower=*/true)) {
        Serial.println("PROJECT error " + err);
        return false;
    }
    rc.runPath = runPath;
    rc.wiringPath = templatePath;
    readProjectMeta(templatePath, rc.meta);
    rc.meta.dir = dir;
    printRunFileLine(runPath, "new");
    runOpenedRunFile(rc, /*fresh=*/true);
    return true;
}

bool projectOpenLatestOrNew(const String& project, String& runPathOut) {
    runPathOut = "";
    String dir, templatePath;
    if (!resolveProjectArg(project, dir, templatePath)) {
        Serial.println("load_project: cannot resolve '" + project + "'");
        return false;
    }
    String projectPath = String(PROJECTS_DIR) + "/" + dir;
#if JL_PROJECT_RUN_HISTORY
    int runCount = 0;
    int maxN = projectScanRunFiles(projectPath, dir, runCount);
    bool haveRun = (maxN >= 1);
    String existing = haveRun ? runFilePath(projectPath, dir, maxN) : String("");
#else
    String existing = projectRunFilePath(dir);
    bool haveRun = safeFileExists(existing.c_str());
#endif

    if (haveRun) {
        String runPath = existing;
        String err;
        if (!projectOpenRunFile(runPath, err)) {
            Serial.println("load_project failed: " + err);
            return false;
        }
        runPathOut = runPath;
        printRunFileLine(runPath, "load");
        return true;
    }

    String runPath, err;
    if (!projectBeginRun(dir, templatePath, runPath, err)) {
        Serial.println("load_project failed: " + err);
        return false;
    }
    runPathOut = runPath;
    printRunFileLine(runPath, "new");
    return true;
}

// ============================================================================
// The launcher
// ============================================================================

void projectsAppLauncher(void) {
    // Self-heal before listing: (re)creates any missing built-in project file
    // from projectFiles[], the way initializeMicroPythonExamples does for
    // /python_scripts. Unforced, so it is an existence check per file and a
    // silent return when everything is already there - and it only ever
    // CREATES, so hand-made projects on the board are untouched.
    initializeProjects();

    // static, not a stack local: PROJECTS_MAX * 5 Strings is a lot of app
    // stack for the deepest menu entry path (the same argument
    // pickPythonScriptFromClickMenu makes when it heap-allocates FileManager).
    // The launcher is not re-entrant, so a single instance is fine.
    static ProjectMeta projects[PROJECTS_MAX];

    Serial.println("\n\r=== Guides ===");
    Serial.println("Rotary: Navigate | Short Click: Select | Long Press or any key: Cancel");

    int count = listProjects(projects, PROJECTS_MAX);
    if (count <= 0) {
        // EXIT 1: nothing entered, nothing to unwind. Since task 8 this is a
        // provisioning-failure path, not a normal one: initializeProjects()
        // above puts the built-in projects back before we get here, so an
        // empty list means the writes themselves failed (full/locked FS).
        notify("No\nguides", "\n\r  No guides found in " + String(PROJECTS_DIR), 1500);
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

    // "PROJECTS" is the MACHINE tag (`PROJECTS n=<count>`, frozen grammar -
    // test_projects drives the launcher on it). "Guide" is the human header
    // painted on the LED matrix, the OLED and the terminal mirror.
    int chosen = runPicker("PROJECTS", "Guide", ledLabels, titles, summaries, count);
    if (chosen < 0) {
        // EXIT A: cancelled before any state was touched.
        Serial.println("  Cancelled.");
        return;
    }

    projectRunInteractive(projects[chosen].dir, String(""), projects[chosen]);
}
