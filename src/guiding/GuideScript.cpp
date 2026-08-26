// SPDX-License-Identifier: MIT
// Guide DATA layer: the guide:/power: parser, step synthesis, and the small
// naming/default helpers. Extracted verbatim from GuidedFlow.cpp (A-M1 of the
// Guides-Simplification plan) so the ambient services and the on-demand check
// command can depend on guide DATA without linking the modal runner.
// Format authority: States.h header comment; DESIGN_GUIDED_PLACEMENT.md §2/§5.

#include "GuideScript.h"

#include "FilesystemStuff.h"   // safeFileOpen / safeFileClose
#include "States.h"            // globalState (parts table), MAX_PARTS

// States.cpp's node-name resolution - the exact helper bridges: and parts:
// parsing use, so guide steps speak the same node vocabulary.
extern int parseNodeName(const String& nodeName);
extern bool parseBoolean(const String& val, bool& success);

const char* guideCheckName(GuideCheck c) {
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

// ---------------------------------------------------------------------------
// guide: parsing (streamed line-by-line)
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

// Default check for a part that named none, by part class (§5.2).
GuideCheck guideDefaultCheckForPart(const PartDefinition& p) {
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
    // `enforce: false` -> measure and report, do not judge. Note the negation:
    // the struct field is bandAdvisory (see GuideScript.h for why it is stored
    // that way round).
    v = guideFlowField(fields, "enforce:");
    if (v.length() > 0) {
        bool ok;
        bool bval = parseBoolean(v, ok);
        if (ok) st.bandAdvisory = !bval;
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
    // `enforce:` waives a band that was DERIVED from a part's value:, which is
    // only continuity and vf. On every other check the min/max IS the author's
    // own statement of intent, and waiving it would leave a step that measures
    // and then does nothing with the answer. Say so at parse time rather than
    // shipping a field that silently no-ops (H3's unsatisfiable-gate ruling).
    if (st.bandAdvisory && st.check != GuideCheck::CONTINUITY &&
        st.check != GuideCheck::VF) {
        err += "guide step " + String(out.numSteps + 1) + ": enforce: only "
               "applies to continuity/vf (ignored on " +
               String(guideCheckName(st.check)) + "); ";
        st.bandAdvisory = false;
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
