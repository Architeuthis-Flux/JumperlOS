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

// Wait for a probe tap on a breadboard row. Fresh-tap discipline
// (PartLabels' listen pattern): the reading must first DECAY (probe at
// rest) before a run of stable identical readings counts - a stale reading
// from before the prompt can never place a part. Returns the row (1-60),
// -1 on cancel-back (click, hold, or probe REMOVE), -2 on exit-app.
// Serial twin: digits + enter = the row; any other byte = exit.
static int partsTapForRow(const PartDbRecord& rec) {
    b.clear();
    b.print(rec.ledName, PARTS_HEADER_COLOR, 0xFFFFFF, 0, 0, 1);
    b.print("TAP ROW", PARTS_ITEM_COLOR, 0xFFFFFF, 0, 1, 1);
    requestLedShow(2);
    if (oled.oledConnected) {
        char text[96];
        snprintf(text, sizeof(text), "%s\nTap row for pin 1\n(click = back)",
                 rec.displayName);
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText(text);
    }
    Serial.print("\r\nPARTPICK tap part=");
    Serial.print(rec.id);
    Serial.println(" (tap row for pin 1; type row + enter; click = back)");
    Serial.flush();

    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;

    bool probeRested = false;
    int lastReading = -1;
    int stableCount = 0;
    char digits[4] = {0};
    int nDigits = 0;
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
        // (Menus.cpp:3027, the voltage-select precedent).
        int reading = probing.justReadProbe(true);
        if (!probeRested) {
            // A touch that predates the prompt must not place a part -
            // require the probe at rest before listening.
            if (reading <= 0) probeRested = true;
        } else if (reading >= 1 && reading <= 60) {
            if (reading == lastReading) stableCount++;
            else { lastReading = reading; stableCount = 1; }
            // ~25 ms of the same row at this loop's ~1 kHz cadence.
            if (stableCount >= 25) return reading;
        } else {
            stableCount = 0;
            lastReading = -1;
        }
        delayMicroseconds(1000);
    }
}

// DB record + tapped row -> a placed part in the live state. The tap means
// "pin 1 goes HERE", and the anchor mapping absorbs the half the footprint
// can't anchor on: a DIP tapped on the top half anchors one column lower
// (+30 = same column, bottom half - States.h geometry: DIP baseRow must be
// 31-60); an axial tapped on the bottom half anchors its column's top hole
// (-30). SIP anchors where you tap.
static bool partsCommitPlacement(const PartDbRecord& rec, int tappedRow) {
    JumperlessState& st = globalState;
    if (st.parts.numParts >= MAX_PARTS) {
        Serial.print("\r\nPARTDB place refused reason=\"parts table full (");
        Serial.print(MAX_PARTS);
        Serial.println(")\"");
        if (oled.oledConnected)
            oled.clearPrintShow("Parts\nfull", 2, true, true, true);
        return false;
    }

    int baseRow = tappedRow;
    const PartDbPinout& po = *partdbPinoutOf(rec);
    if (po.footprint == PARTDB_FOOT_DIP && tappedRow <= 30) baseRow = tappedRow + 30;
    if (po.footprint == PARTDB_FOOT_AXIAL2 && tappedRow > 30) baseRow = tappedRow - 30;

    static PartDefinition tmp;   // ~600 B - keep it off the core-0 stack
    partdbInstantiate(rec, tmp);
    tmp.baseRow = (int16_t)baseRow;

    // Unique name: NE555, NE555_2, ... (findByName is the serializer's own
    // identity check, so a name it can't see is free).
    if (st.parts.findByName(tmp.name) >= 0) {
        char base[16];
        strncpy(base, tmp.name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        if (strlen(base) > 13) base[13] = '\0';   // room for "_9"
        for (int suffix = 2; suffix <= 9; suffix++) {
            snprintf(tmp.name, sizeof(tmp.name), "%s_%d", base, suffix);
            if (st.parts.findByName(tmp.name) < 0) break;
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
    Serial.println(baseRow);
    Serial.flush();
    if (oled.oledConnected) {
        char text[64];
        snprintf(text, sizeof(text), "%s\nrow %d", st.parts.parts[idx].name, baseRow);
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

void partsAppLauncher(void) {
    // Own the render mode for the whole session (menus render one item at a
    // time; core 1 suppresses net paint while inClickMenu). runPicker's
    // save/restore discipline for the divider.
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    int classIdx = 0;
    while (true) {
        // Class level: only classes that actually hold placeable records.
        int nClasses = 0;
        static uint8_t classOf[PARTDB_NUM_CLASSES];
        for (int i = 0; i < kNumPartClasses; i++) {
            if (!partsClassHasPlaceable(kPartClasses[i].cls)) continue;
            s_led[nClasses] = kPartClasses[i].led;
            s_title[nClasses] = kPartClasses[i].title;
            s_desc[nClasses] = kPartClasses[i].desc;
            classOf[nClasses] = kPartClasses[i].cls;
            nClasses++;
        }
        int pick = partsPicker("class", "Parts", nClasses, classIdx);
        if (pick < 0) break;   // hold or serial byte at the top level = exit
        classIdx = pick;
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

            int row = partsTapForRow(rec);
            if (row == -2) goto done;
            if (row == -1) continue;   // back to the part list
            if (partsCommitPlacement(rec, row)) {
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
