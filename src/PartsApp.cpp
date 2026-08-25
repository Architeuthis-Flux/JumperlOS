// SPDX-License-Identifier: MIT
// Parts picker / placement app. Contract: PartsApp.h.
//
// Browse the flash parts DB (class -> part), tap the row where pin 1 goes,
// and get out of the way: the app exits on a successful placement and the
// ambient services own everything after (PartLabels blooms the pins,
// DisplayService routes a display's data pins when it sees one). The picker
// loop is runPicker's ownership discipline (ProjectsApp.cpp:683) rendering
// from rodata const char* instead of String.

#include "PartsApp.h"

#include "Commands.h"       // refreshConnections, requestLedShow
#include "Graphics.h"       // b.print - LED matrix text
#include "JumperlOS.h"      // jOS.serviceInner()
#include "Menus.h"          // inClickMenu
#include "PartDb.h"
#include "PartLabels.h"     // bloom nudge after a placement
#include "PartPlacement.h"  // applyPartPlacement, partGeometryOk
#include "Probing.h"        // probeButton, probing.getLastProbeReading
#include "RotaryEncoder.h"  // encoder state machine, rotaryDivider
#include "States.h"         // globalState
#include "config.h"         // jumperlessConfig.hardware.probe_revision
#include "oled.h"

int partsProbeButton(void) {
    int bPress = probeButton.getButtonPress(true);
    if (jumperlessConfig.hardware.probe_revision > 3) {
        if (bPress == 1) bPress = 2;
        else if (bPress == 2) bPress = 1;
    }
    return bPress;
}

// ============================================================================
// Picker plumbing
// ============================================================================

// FileManager's click-menu palette (the runPicker look).
static const uint32_t PARTS_HEADER_COLOR = 0x001008;
static const uint32_t PARTS_ITEM_COLOR = 0x100810;

// One shared scratch list for both picker levels. 96 covers the largest
// class with headroom (111 records TOTAL in the seed DB); a class that ever
// outgrows it is silently capped and the machine line's n= makes that
// visible long before a user notices.
#define PARTS_LIST_MAX 96
static const char* s_led[PARTS_LIST_MAX];
static const char* s_title[PARTS_LIST_MAX];
static const char* s_desc[PARTS_LIST_MAX];
static uint16_t s_rec[PARTS_LIST_MAX];

static void partsDrawItem(const char* header, const char* ledLabel,
                          const char* title, const char* desc) {
    b.clear();
    b.print(header, PARTS_HEADER_COLOR, 0xFFFFFF, 0, 0, 1);
    b.print(ledLabel, PARTS_ITEM_COLOR, 0xFFFFFF, 0, 1, 1);
    requestLedShow(2);

    if (oled.oledConnected) {
        char text[120];
        snprintf(text, sizeof(text), "%s\n%s", title, desc ? desc : "");
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText(text);
    }

    Serial.print("\r  ");
    Serial.print(header);
    Serial.print(": ");
    Serial.print(title);
    Serial.print("                    \r");
    Serial.flush();
}

// Encoder picker over the s_* arrays. Returns the chosen index, -1 on
// HELD (back one level), -2 on a serial byte (exit the app - the runPicker
// convention: a byte left unconsumed would land on the single-char handler).
static int partsPicker(const char* levelTag, const char* header, int count,
                       int startIdx) {
    if (count <= 0) return -1;
    int idx = (startIdx >= 0 && startIdx < count) ? startIdx : 0;
    bool needsDraw = true;
    unsigned long lastShowRequest = 0;

    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;

    Serial.print("\r\nPARTPICK level=");
    Serial.print(levelTag);
    Serial.print(" n=");
    Serial.println(count);
    Serial.flush();

    while (true) {
        if (needsDraw) {
            partsDrawItem(header, s_led[idx], s_title[idx], s_desc[idx]);
            lastShowRequest = millis();
            needsDraw = false;
        }

        jOS.serviceInner();
        rotaryEncoderButtonStuff();

        // Core 2's end-of-frame compare-and-swap can swallow a show request
        // issued mid-frame (Menus.cpp's menuShowKeepalive) - re-assert.
        if (millis() - lastShowRequest >= 250) {
            requestLedShow(2);
            lastShowRequest = millis();
        }

        if (encoderButtonState == HELD) {
            // Wait out the hold so the release can't echo into the next level.
            while (encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
                   encoderButtonState == LONG_HELD) {
                jOS.serviceInner();
                rotaryEncoderButtonStuff();
                delayMicroseconds(1000);
            }
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            return -1;
        }
        if (Serial.available() > 0) {
            Serial.read();
            return -2;
        }
        if (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) {
            encoderButtonState = IDLE;
            return idx;
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
}

// ============================================================================
// Tap-to-place
// ============================================================================

// Wait for a probe tap on a breadboard row. Returns the row (1-60),
// -1 on cancel-back (click, hold, or probe REMOVE), -2 on exit-app.
// Serial twin: digits + enter = the row; any other byte = exit.
//
// signal == nullptr is the DIP anchor flow ("tap row for pin 1"). A signal
// name means the per-signal flow (Kevin's ruling: SIP modules and passives
// have no standard header order, so the user taps every signal by name);
// usedRows are this part's already-tapped rows, refused with a nudge.
static int partsTapForRow(const PartDbRecord& rec, const char* signal = nullptr,
                          int step = 0, int total = 0,
                          const int* usedRows = nullptr, int nUsed = 0) {
    b.clear();
    b.print(rec.ledName, PARTS_HEADER_COLOR, 0xFFFFFF, 0, 0, 1);
    char lineBuf[16];
    if (signal != nullptr) {
        if (strlen(signal) <= 3) snprintf(lineBuf, sizeof(lineBuf), "TAP %s", signal);
        else snprintf(lineBuf, sizeof(lineBuf), "%.7s", signal);
    } else {
        snprintf(lineBuf, sizeof(lineBuf), "TAP ROW");
    }
    b.print(lineBuf, PARTS_ITEM_COLOR, 0xFFFFFF, 0, 1, 1);
    requestLedShow(2);
    if (oled.oledConnected) {
        char text[96];
        if (signal != nullptr) {
            snprintf(text, sizeof(text), "%s\nTap %s (%d/%d)\n(click = back)",
                     rec.displayName, signal, step, total);
        } else {
            snprintf(text, sizeof(text), "%s\nTap row for pin 1\n(click = back)",
                     rec.displayName);
        }
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText(text);
    }
    Serial.print("\r\nPARTPICK tap part=");
    Serial.print(rec.id);
    if (signal != nullptr) {
        Serial.print(" sig=");
        Serial.print(signal);
    }
    Serial.println(" (tap the row; type row + enter; click = back)");
    Serial.flush();

    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;

    char digits[4] = {0};
    int nDigits = 0;
    int lastNudgedRow = -1;
    unsigned long lastShowRequest = millis();

    while (true) {
        jOS.serviceInner();
        rotaryEncoderButtonStuff();
        if (millis() - lastShowRequest >= 250) {
            requestLedShow(2);
            lastShowRequest = millis();
        }

        if (encoderButtonState == HELD ||
            (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED)) {
            // Wait the hold out (the picker's own discipline) so one long
            // press can't cascade a second cancel into the level above.
            while (encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
                   encoderButtonState == LONG_HELD) {
                jOS.serviceInner();
                rotaryEncoderButtonStuff();
                delayMicroseconds(1000);
            }
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            return -1;
        }
        int bPress = partsProbeButton();
        if (bPress == 2) return -1;   // REMOVE = back

        if (Serial.available() > 0) {
            char c = (char)Serial.read();
            if (c >= '0' && c <= '9' && nDigits < 3) {
                digits[nDigits++] = c;
                digits[nDigits] = '\0';
            } else if ((c == '\r' || c == '\n') && nDigits > 0) {
                int row = atoi(digits);
                if (row >= 1 && row <= 60) return row;
                Serial.println("  row must be 1-60");
                nDigits = 0;
                digits[0] = '\0';
            } else if (c != '\r' && c != '\n') {
                return -2;
            }
        }

        // Direct synchronous read - the Probing SERVICE is not in the inner
        // set, so its cached getLastProbeReading() goes stale inside a modal
        // loop; justReadProbe(true) is what the menus' own pickers poll
        // (Menus.cpp:3027, the voltage-select precedent). ONE accepted
        // reading IS the event - the contract debounces internally (a new
        // row needs two consecutive decodes; repeats of a held row are
        // rate-limited to one per 500 ms with -1 between, so any
        // wait-for-N-stable-reads scheme here can never fire).
        int reading = probing.justReadProbe(true);
        if (reading >= 1 && reading <= 60) {
            bool used = false;
            for (int u = 0; u < nUsed; u++) {
                if (usedRows[u] == reading) { used = true; break; }
            }
            if (!used) return reading;
            // A held probe re-emits its row every 500 ms - nudge once per
            // distinct offending row, not per emission.
            if (reading != lastNudgedRow) {
                lastNudgedRow = reading;
                Serial.print("  row ");
                Serial.print(reading);
                Serial.println(" is already one of this part's pins");
            }
        }
        delayMicroseconds(1000);
    }
}

// DB record + tapped rows -> a placed part in the live state.
//
// nRows == 1 is the DIP anchor flow: the tap means "pin 1 goes HERE" and the
// anchor mapping absorbs the half the footprint can't anchor on (a DIP tapped
// on the top half anchors one column lower; +30 = same column, bottom half -
// States.h geometry: DIP baseRow must be 31-60).
//
// nRows == numPins is the per-signal flow (Kevin's ruling: SIP modules and
// passives have no standard header order, so the taps ARE the truth):
//   SIP    -> baseRow = the lowest tapped row, every pin gets offset =
//             tappedRow - baseRow (the PartPin field that wins over footprint
//             math). Any distinct same-half rows are legal - a module on fly
//             wires places as honestly as one plugged straight in.
//   axial2 -> the two taps must share a column across the center line (the
//             only legal axial shape); whichever signal was tapped on the top
//             half becomes footprint pin 1, so polarity follows the taps.
static bool partsCommitPlacement(const PartDbRecord& rec, const int* rows, int nRows) {
    JumperlessState& st = globalState;
    if (st.parts.numParts >= MAX_PARTS) {
        Serial.print("\r\nPARTDB place refused reason=\"parts table full (");
        Serial.print(MAX_PARTS);
        Serial.println(")\"");
        if (oled.oledConnected)
            oled.clearPrintShow("Parts\nfull", 2, true, true, true);
        return false;
    }

    const PartDbPinout& po = *partdbPinoutOf(rec);
    static PartDefinition tmp;   // ~600 B - keep it off the core-0 stack
    partdbInstantiate(rec, tmp);

    if (nRows == 1) {
        int baseRow = rows[0];
        if (po.footprint == PARTDB_FOOT_DIP && baseRow <= 30) baseRow += 30;
        tmp.baseRow = (int16_t)baseRow;
    } else if (po.footprint == PARTDB_FOOT_AXIAL2 && nRows == 2) {
        int top = (rows[0] <= 30) ? rows[0] : rows[1];
        int bottom = (rows[0] <= 30) ? rows[1] : rows[0];
        if (top > 30 || bottom <= 30 || bottom != top + 30) {
            Serial.println("\r\nPARTDB place refused reason=\"an axial part's "
                           "ends must share a column (one top, one bottom)\"");
            if (oled.oledConnected)
                oled.clearPrintShow("Ends must\nshare a column", 2, true, true, true);
            return false;
        }
        tmp.baseRow = (int16_t)top;
        for (int j = 0; j < tmp.numPins && j < 2; j++) {
            tmp.pins[j].pinNumber = (rows[j] <= 30) ? 1 : 2;
        }
    } else {
        // SIP per-signal: all taps in one half (offsets are same-side).
        bool topHalf = rows[0] <= 30;
        for (int j = 1; j < nRows; j++) {
            if ((rows[j] <= 30) != topHalf) {
                Serial.println("\r\nPARTDB place refused reason=\"all pins "
                               "must be in the same half of the board\"");
                if (oled.oledConnected)
                    oled.clearPrintShow("Pins must\nshare a half", 2, true, true, true);
                return false;
            }
        }
        int baseRow = rows[0];
        for (int j = 1; j < nRows; j++) {
            if (rows[j] < baseRow) baseRow = rows[j];
        }
        tmp.baseRow = (int16_t)baseRow;
        for (int j = 0; j < nRows && j < tmp.numPins; j++) {
            tmp.pins[j].offset = (int8_t)(rows[j] - baseRow);
        }
    }

    // Unique name: NE555, NE555_2, ... (findByName is the serializer's own
    // identity check, so a name it can't see is free). MAX_PARTS is 16, so
    // suffixes through _16 always suffice; refuse rather than commit a
    // silent duplicate if the impossible happens (sweep finding: the old
    // cap at _9 duplicated the 10th same-record placement).
    if (st.parts.findByName(tmp.name) >= 0) {
        char base[16];
        strncpy(base, tmp.name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        if (strlen(base) > 12) base[12] = '\0';   // room for "_16"
        bool named = false;
        for (int suffix = 2; suffix <= MAX_PARTS; suffix++) {
            snprintf(tmp.name, sizeof(tmp.name), "%s_%d", base, suffix);
            if (st.parts.findByName(tmp.name) < 0) { named = true; break; }
        }
        if (!named) {
            Serial.println("\r\nPARTDB place refused reason=\"name collision\"");
            return false;
        }
    }

    char reason[96] = {0};
    if (!partGeometryOk(tmp, reason, sizeof(reason))) {
        Serial.print("\r\nPARTDB place refused reason=\"");
        Serial.print(reason);
        Serial.println("\"");
        if (oled.oledConnected) {
            char text[120];
            snprintf(text, sizeof(text), "%s\n%s", rec.displayName, reason);
            oled.resetMultiLineSmallText();
            oled.showMultiLineSmallText(text);
            delay(1200);
        }
        return false;
    }

    int idx = st.parts.numParts;
    st.parts.parts[idx] = tmp;
    st.parts.numParts++;
    String err;
    if (applyPartPlacement(st, idx, err) < 0) {
        st.parts.numParts--;   // roll the append back - nothing was applied
        Serial.println("\r\nPARTDB place refused reason=\"" + err + "\"");
        return false;
    }
    st.markDirty();
    refreshConnections(-1);
    partLabels.requestRun();   // bloom now, not at the next 20 ms tick

    Serial.print("\r\nPARTDB place ok=");
    Serial.print(st.parts.parts[idx].name);
    Serial.print(" row=");
    Serial.println(tmp.baseRow);
    Serial.flush();
    if (oled.oledConnected) {
        char text[64];
        snprintf(text, sizeof(text), "%s\nrow %d", st.parts.parts[idx].name,
                 (int)tmp.baseRow);
        oled.clearPrintShow(text, 2, true, true, true);
        delay(800);
    }
    return true;
}

// ============================================================================
// The app
// ============================================================================

static const struct {
    uint8_t cls;
    const char* led;    // <= 7 glyphs (LED half-row)
    const char* title;
    const char* desc;
} kPartClasses[] = {
    { PARTDB_CLASS_LOGIC,      "Logic",   "Logic",       "7400 / 4000 ICs" },
    { PARTDB_CLASS_ANALOG,     "Analog",  "Analog",      "555 opamps regs" },
    { PARTDB_CLASS_DISCRETE,   "Discret", "Discrete",    "R C LED diode" },
    { PARTDB_CLASS_TRANSISTOR, "Transis", "Transistors", "BJT / MOSFET" },
    { PARTDB_CLASS_DISPLAY,    "Display", "Displays",    "OLED LCD LED" },
    { PARTDB_CLASS_MODULE,     "Modules", "Modules",     "sensors + I2C" },
};
static const int kNumPartClasses =
    sizeof(kPartClasses) / sizeof(kPartClasses[0]);

// Does the class hold at least one placeable record? Deliberately does NOT
// touch the s_* arrays: the class-level menu build calls this while those
// arrays hold the CLASS labels, and the fill version below would clobber
// them with record names (the first Parts open would have listed
// "MPU6050 / BME280 / ..." as the classes).
static bool partsClassHasPlaceable(uint8_t cls) {
    uint16_t count = 0;
    const uint16_t* slice = partdbClassSlice(cls, &count);
    for (uint16_t i = 0; i < count; i++) {
        if (partdbPlaceableHere(partdb_records[slice[i]])) return true;
    }
    return false;
}

// Fill s_* with the class's placeable records; returns the count.
static int partsBuildClassList(uint8_t cls) {
    uint16_t count = 0;
    const uint16_t* slice = partdbClassSlice(cls, &count);
    int n = 0;
    for (uint16_t i = 0; i < count && n < PARTS_LIST_MAX; i++) {
        const PartDbRecord& r = partdb_records[slice[i]];
        if (!partdbPlaceableHere(r)) continue;   // OG gate + oversize filter
        s_led[n] = r.ledName;
        s_title[n] = r.displayName;
        s_desc[n] = r.desc;
        s_rec[n] = slice[i];
        n++;
    }
    return n;
}

// B-M4 slice: clear every part off the table (their bridges come down via
// removePartPlacement - the invariant path), CONNECT-confirmed. Returns
// true when parts were cleared.
static bool partsClearAll(void) {
    JumperlessState& st = globalState;
    if (st.parts.numParts <= 0) return false;

    if (oled.oledConnected) {
        char text[96];
        snprintf(text, sizeof(text), "Clear %d part%s?\nCONNECT = yes\nclick = no",
                 st.parts.numParts, st.parts.numParts == 1 ? "" : "s");
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText(text);
    }
    Serial.print("\r\nPARTS clear confirm n=");
    Serial.println(st.parts.numParts);
    Serial.flush();
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    while (true) {
        jOS.serviceInner();
        rotaryEncoderButtonStuff();
        int bPress = partsProbeButton();
        if (bPress == 1) break;        // CONNECT = yes
        if (bPress == 2) return false; // REMOVE = no
        if (encoderButtonState == HELD ||
            (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED)) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            return false;
        }
        if (Serial.available() > 0) {
            char c = (char)Serial.read();
            if (c != 'y' && c != 'Y') return false;
            break;                     // serial twin: y = yes
        }
        delayMicroseconds(1000);
    }

    String err;
    for (int i = st.parts.numParts - 1; i >= 0; i--) {
        if (st.parts.parts[i].placed) removePartPlacement(st, i, err);
    }
    int n = st.parts.numParts;
    st.parts.numParts = 0;
    // Guide progress goes with the parts (mirrors parts.clear()'s scope):
    // a stamped guideSource with an empty table left the StepViewer armed
    // for vanished parts - dangling emphasis indices (sweep finding).
    st.parts.guideSource[0] = '\0';
    st.parts.guideStep = 0;
    st.parts.guideTotal = 0;
    st.markDirty();
    refreshConnections(-1);
    partLabels.requestRun();
    Serial.print("\r\nPARTS cleared n=");
    Serial.println(n);
    Serial.flush();
    if (oled.oledConnected)
        oled.clearPrintShow("Parts\ncleared", 2, true, true, true);
    return true;
}

void partsAppLauncher(void) {
    // Own the render mode for the whole session (menus render one item at a
    // time; core 1 suppresses net paint while inClickMenu). runPicker's
    // save/restore discipline for the divider.
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    int classIdx = 0;
    while (true) {
        // Class level: only classes that actually hold placeable records,
        // plus a trailing Clear row while any part is on the table.
        int nClasses = 0;
        static uint8_t classOf[PARTDB_NUM_CLASSES + 1];
        for (int i = 0; i < kNumPartClasses; i++) {
            if (!partsClassHasPlaceable(kPartClasses[i].cls)) continue;
            s_led[nClasses] = kPartClasses[i].led;
            s_title[nClasses] = kPartClasses[i].title;
            s_desc[nClasses] = kPartClasses[i].desc;
            classOf[nClasses] = kPartClasses[i].cls;
            nClasses++;
        }
        int clearIdx = -1;
        if (globalState.parts.numParts > 0) {
            clearIdx = nClasses;
            s_led[nClasses] = "Clear";
            s_title[nClasses] = "Clear parts";
            s_desc[nClasses] = "remove every part";
            classOf[nClasses] = 0xFF;
            nClasses++;
        }
        int pick = partsPicker("class", "Parts", nClasses, classIdx);
        if (pick < 0) break;   // hold or serial byte at the top level = exit
        classIdx = pick;
        if (pick == clearIdx) {
            if (partsClearAll()) break;   // cleared: exit, board is ambient
            continue;                     // declined: back to the class list
        }
        uint8_t cls = classOf[pick];
        const char* clsTitle = s_title[pick];

        int partIdx = 0;
        while (true) {
            int nParts = partsBuildClassList(cls);
            if (nParts <= 0) break;
            int p = partsPicker("part", clsTitle, nParts, partIdx);
            if (p == -1) break;       // hold = back to classes
            if (p == -2) goto done;   // serial byte = exit the app
            partIdx = p;
            const PartDbRecord& rec = partdb_records[s_rec[p]];

            // DIP crosses the center line - one orientation, one anchor tap.
            // Everything else (SIP modules, passives) has no standard header
            // order, so every listed signal is tapped by name and the taps
            // are the geometry (Kevin's ruling, 2026-08-25).
            const PartDbPinout& po = *partdbPinoutOf(rec);
            int rows[MAX_PART_PINS];
            int nRows = 0;
            if (po.footprint == PARTDB_FOOT_DIP) {
                int row = partsTapForRow(rec);
                if (row == -2) goto done;
                if (row == -1) continue;   // back to the part list
                rows[nRows++] = row;
            } else {
                int nSignals = po.numPins;
                if (nSignals > MAX_PART_PINS) nSignals = MAX_PART_PINS;
                bool cancelled = false;
                for (int s = 0; s < nSignals; s++) {
                    int row = partsTapForRow(rec, po.pins[s].name, s + 1,
                                             nSignals, rows, nRows);
                    if (row == -2) goto done;
                    if (row == -1) { cancelled = true; break; }
                    rows[nRows++] = row;
                    Serial.print("PARTPICK sig=");
                    Serial.print(po.pins[s].name);
                    Serial.print(" row=");
                    Serial.println(row);
                }
                if (cancelled) continue;   // back to the part list
            }
            if (partsCommitPlacement(rec, rows, nRows)) {
                // Placed: the app's job is over - exit and let the ambient
                // services own it (labels bloom, DisplayService routes a
                // display's data pins the moment it polls).
                goto done;
            }
            // Refusal: stay on the part list for another try.
        }
    }
done:
    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    requestLedShow(-1);
    Serial.println();
    // The steps screen (if armed) or the logo - never a stale picker frame.
    oled.showJogo32h();
}
