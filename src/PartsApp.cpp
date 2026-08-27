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
#include "NetManager.h"     // findNodeInNet - power-route conflict guard
#include "PartDb.h"
#include "PartLabels.h"     // bloom nudge after a placement
#include "PartPlacement.h"  // applyPartPlacement, partGeometryOk
#include "Probing.h"        // probeButton, probing.getLastProbeReading
#include "RotaryEncoder.h"  // encoder state machine, rotaryDivider
#include "LEDs.h"           // HsvToRaw - per-signal tap rainbow
#include "sensing/PartClassify.h"  // tap-time electrical identification
#include "sensing/PartMeasure.h"   // partScanCensus (Auto Scan)
#include "eyecandy/ReadingDisplay.h"  // measured values on the OLED
#include "displays/DisplayService.h"   // display liveness (Parts > Test)
#include "guiding/GuideScript.h"      // formatOhms
#include "Undo.h"           // UndoIngestGuard - placements are not undoable (yet)
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
static const uint32_t PARTS_TAP_DONE_COLOR = 0x000C04;   // rows already tapped
static const uint32_t PARTS_TAP_FLASH_COLOR = 0x0A2A08;  // the just-accepted tap
static const uint32_t PARTS_TAP_IGNORED_COLOR = 0x200404; // tap on an already-used row
// The standing transistor-role colors (Kevin's ruling): E red, B yellow,
// C blue - the same three everywhere a result paints the board.
static const uint32_t PARTS_ROLE_E_COLOR = 0x2A0000;
static const uint32_t PARTS_ROLE_B_COLOR = 0x201400;
static const uint32_t PARTS_ROLE_C_COLOR = 0x00062A;

// Every prompted signal gets its own hue - even undefined ones default to a
// rainbow (Kevin's ruling). Dim like the rest of the palette: these sit next
// to live net colors.
static uint32_t partsTapHue(int idx, int total, bool bright) {
    if (total < 1) total = 1;
    hsvColor h;
    h.h = (uint8_t)((idx * 255) / total);
    h.s = 235;
    h.v = bright ? 90 : 26;
    return HsvToRaw(h);
}

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
    // Rows already tapped for this part stay marked, each signal in its own
    // hue (Kevin's ruling: every accepted tap needs breadboard confirmation,
    // and the pins get a rainbow even when their names are undefined).
    for (int u = 0; u < nUsed; u++) {
        b.lightUpNode(usedRows[u], (total > 0) ? partsTapHue(u, total, false)
                                               : PARTS_TAP_DONE_COLOR);
    }
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
    // Accept on a lift-then-touch TRANSITION, not a wall clock (fixcheck #2:
    // a parked probe re-emits its row every 500 ms forever, so NO deadtime
    // can outwait it - it self-accepted the prompt 100 ms late instead).
    // 700 ms without an emission means the tip really left the board (longer
    // than justReadProbe's duplicate window - PartLabels' LBL_LIFT_MS
    // reasoning), and a prompt that OPENS with the probe in the air arms
    // almost immediately. Once armed, a fresh tap is accepted on its FIRST
    // emission - fast taps are not swallowed anymore. The serial twin
    // (typed rows) is deliberate and skips the gate.
    unsigned long openMs = millis();
    unsigned long lastEmitMs = 0;   // 0 = no emission seen since open
    bool armed = false;
    bool liftHinted = false;

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
                bool rowUsed = false;
                for (int u = 0; u < nUsed; u++) {
                    if (usedRows[u] == row) { rowUsed = true; break; }
                }
                if (rowUsed) {
                    Serial.print("  row ");
                    Serial.print(row);
                    Serial.println(" is already one of this part's pins");
                    nDigits = 0;
                    digits[0] = '\0';
                } else if (row >= 1 && row <= 60) {
                    b.lightUpNode(row, PARTS_TAP_FLASH_COLOR);
                    requestLedShow(2);
                    delay(180);   // the confirmation flash lands before the
                                  // next prompt repaints
                    return row;
                }
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
            bool wasArmed = armed;
            lastEmitMs = millis();
            armed = false;             // the next accept needs another lift
            bool used = false;
            for (int u = 0; u < nUsed; u++) {
                if (usedRows[u] == reading) { used = true; break; }
            }
            if (wasArmed && !used) {
                uint32_t flash = (total > 0) ? partsTapHue(step - 1, total, true)
                                             : PARTS_TAP_FLASH_COLOR;
                b.lightUpNode(reading, flash);
                requestLedShow(2);
                delay(180);   // visible confirmation before the next prompt
                return reading;
            }
            if (used) {
                // A held probe re-emits its row every 500 ms - flash and
                // nudge once per distinct offending row, not per emission.
                if (reading != lastNudgedRow) {
                    lastNudgedRow = reading;
                    b.lightUpNode(reading, PARTS_TAP_IGNORED_COLOR);
                    requestLedShow(2);
                    Serial.print("  row ");
                    Serial.print(reading);
                    Serial.println(" is already one of this part's pins");
                }
            } else if (!liftHinted) {
                // The probe was already resting here when the prompt opened -
                // that emission is not a tap. Say so once instead of silence.
                liftHinted = true;
                Serial.println("  (lift the probe, then tap the row you want)");
            }
        }
        if (!armed) {
            if (lastEmitMs == 0) {
                if (millis() - openMs >= 80) armed = true;    // opened in the air
            } else if (millis() - lastEmitMs >= 700) {
                armed = true;                                  // really lifted
            }
        }
        delayMicroseconds(1000);
    }
}

// Any-order tap collection: N taps, any rows, each confirmed in its own
// hue. Returns N, or -1 (back) / -2 (exit) straight from the prompt.
static int partsTapAnyRows(const PartDbRecord& rec, int* rowsOut, int nWanted) {
    int n = 0;
    while (n < nWanted) {
        int row = partsTapForRow(rec, "any leg", n + 1, nWanted, rowsOut, n);
        if (row < 0) return row;
        rowsOut[n++] = row;
        Serial.print("\r\nPARTPICK tap#");
        Serial.print(n);
        Serial.print(" row=");
        Serial.println(row);
    }
    return n;
}

// Tap-time electrical identification (Kevin's ask: "tap any 3 pins on a
// transistor and our part detection should figure it out"). Runs the
// PartClassify session on the tapped rows and permutes rows[] into DB pin
// order by matching identified roles (E/B/C, A/K, S/G/D) against the DB
// pin names. Returns:
//   2 = nonpolar part, tap order is already fine (res still carries the
//       measured value - a resistor's ohms, a cap's detect)
//   1 = identified and mapped; rows[] is now in DB pin order
//   0 = could not verify (refused rows, unexpected type, no role match) -
//       caller places sorted-taps-as-DB-order and warns
static int partsIdentifyAndOrder(const PartDbRecord& rec, const PartDbPinout& po,
                                 int* rows, int nRows, PartResult* resOut) {
    PartResult res = (nRows == 3) ? identifyThreeLead(rows[0], rows[1], rows[2])
                                  : identifyTwoLead(rows[0], rows[1]);
    if (resOut) *resOut = res;

    bool polar = (rec.partClass == PARTDB_CLASS_TRANSISTOR) ||
                 (rec.partClass == PARTDB_CLASS_DISCRETE &&
                  (rec.subClass == PARTDB_SUB_DISCRETE_LED ||
                   rec.subClass == PARTDB_SUB_DISCRETE_DIODE ||
                   rec.subClass == PARTDB_SUB_DISCRETE_POT));
    if (!polar) return 2;              // any order IS the order
    if (res.status != 0) return 0;

    // roles -> DB pin names, each tapped row spent exactly once
    int mapped[MAX_PART_PINS];
    bool spent[3] = { false, false, false };
    int nPins = po.numPins;
    if (nPins > nRows) return 0;
    for (int j = 0; j < nPins; j++) {
        int found = -1;
        for (int t = 0; t < res.nRows && t < 3; t++) {
            if (spent[t]) continue;
            if (strcmp(pinRoleName(res.roles[t]), po.pins[j].name) == 0) {
                found = t;
                break;
            }
        }
        if (found < 0) return 0;
        spent[found] = true;
        mapped[j] = rows[found];
    }
    for (int j = 0; j < nPins; j++) rows[j] = mapped[j];
    return 1;
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
static bool partsCommitPlacement(const PartDbRecord& rec, const int* rows, int nRows,
                                 uint8_t pinsUnverified = 0, float measuredOhms = 0.0f) {
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
    // static scratch: both RAM-only fields set EVERY pass, never inherited
    tmp.pinsUnverified = pinsUnverified;
    tmp.measuredOhms = measuredOhms;

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

    // Power routes at placement (Kevin's ruling, 2026-08-25 - supersedes the
    // design-phase "the user wires power" contract): a power-class pin
    // bridges to the rail on its half, a gnd-class pin to GND. Rails still
    // obey the user, so nothing is hot until the rails are. A row whose net
    // already holds the OPPOSING special node is left unrouted - bridging it
    // would short rail to GND through our own bridge; PartLabels' warning
    // (VCC_TO_GND / GND_TO_HOT) tells the user instead.
    auto rowNetHas = [](int row, int specialNode) {
        int netNum = findNodeInNet(row);
        if (netNum <= 0 || netNum >= MAX_NETS) return false;
        const netStruct& net = globalState.connections.nets[netNum];
        if (net.number != netNum) return false;
        for (int n = 0; n < MAX_NODES && net.nodes[n] != 0; n++) {
            if (net.nodes[n] == specialNode) return true;
        }
        return false;
    };
    for (int j = 0; j < tmp.numPins && j < MAX_PART_PINS; j++) {
        PartPin& pin = tmp.pins[j];
        if (pin.connect >= 0) continue;   // an explicit DB binding wins
        int node = partPinNode(tmp, pin);
        if (node < 1 || node > 60) continue;
        // A row whose net ALREADY holds any power node is left alone: the
        // user wired it (adopting it would delete their wire when the part
        // is removed - sweep finding), it conflicts (our bridge would short
        // rail to GND or rail to rail), or it's a duplicate. Either way the
        // right auto-route is none; PartLabels' warnings judge the result.
        bool rowPowered = rowNetHas(node, GND) || rowNetHas(node, TOP_RAIL) ||
                          rowNetHas(node, BOTTOM_RAIL);
        if (pin.pinClass == 1) {
            // Rails only for pins the DB marks role VCC (sweep finding,
            // confirmed): pinClass alone also covers regulator OUTPUTS
            // (LM317/L7805/AMS1117 VOUT) and 405x VEE - bridging those ties
            // VIN to VOUT through the rail or feeds VEE positive. A power-
            // class pin with role NONE stays unrouted; the user wires it.
            if (j >= po.numPins || po.pins[j].role != PARTDB_ROLE_VCC) continue;
            if (rowPowered) continue;
            pin.connect = (node <= 30) ? TOP_RAIL : BOTTOM_RAIL;
        } else if (pin.pinClass == 2) {
            if (rowPowered) continue;
            pin.connect = GND;
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
    // Placements are NOT undoable for now (sweep finding: the bridges landed
    // in the undo stream but the parts-table halves didn't, so undo/redo
    // desynced the two and undone power bridges resurrected on reboot).
    // Suppress recording; a labeled parts-aware transaction is future work.
    UndoIngestGuard undoGuard;
    String err;
    int added = applyPartPlacement(st, idx, err);
    // err TEXT with a non-negative count = addConnection refusals (bridge
    // table full) - the same defect class the sweep fixed in routeDataPins:
    // treating those as success committed a part whose bridges never existed.
    // More reachable now that placement adds power bridges.
    if (added < 0 || err.length() > 0) {
        String reason = err;   // removePartPlacement reuses err - keep the message
        if (added >= 0) removePartPlacement(st, idx, err);
        st.parts.numParts--;   // roll the append back
        Serial.println("\r\nPARTDB place refused reason=\"" + reason + "\"");
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

    // Same non-undoable contract as placement (see partsCommitPlacement):
    // undoing half of a clear resurrected bridges for parts that no longer
    // existed in the table.
    UndoIngestGuard undoGuard;
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
            // Transistors and 2-lead discretes take ANY-ORDER taps and the
            // electrical identification sorts out which leg is which
            // (Kevin's ask, 2026-08-26). Everything else (SIP modules, pots)
            // taps every signal by name and the taps are the geometry
            // (Kevin's ruling, 2026-08-25).
            const PartDbPinout& po = *partdbPinoutOf(rec);
            int rows[MAX_PART_PINS];
            int nRows = 0;
            uint8_t unverified = 0;
            float measOhms = 0.0f;
            int nSignals = po.numPins;
            if (nSignals > MAX_PART_PINS) nSignals = MAX_PART_PINS;
            bool canIdentify =
                (rec.partClass == PARTDB_CLASS_TRANSISTOR && nSignals == 3) ||
                (rec.partClass == PARTDB_CLASS_DISCRETE && nSignals == 2) ||
                (rec.partClass == PARTDB_CLASS_DISCRETE && nSignals == 3 &&
                 rec.subClass == PARTDB_SUB_DISCRETE_POT);
            if (po.footprint == PARTDB_FOOT_DIP) {
                int row = partsTapForRow(rec);
                if (row == -2) goto done;
                if (row == -1) continue;   // back to the part list
                rows[nRows++] = row;
            } else if (canIdentify) {
                int got = partsTapAnyRows(rec, rows, nSignals);
                if (got == -2) goto done;
                if (got == -1) continue;
                nRows = nSignals;
                if (oled.oledConnected) {
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText("checking the part...");
                }
                PartResult res;
                int idOk = partsIdentifyAndOrder(rec, po, rows, nRows, &res);
                if (idOk == 0) {
                    // Couldn't confirm which leg is which: place anyway with
                    // the sorted taps as the DB pin order, wear the warning
                    // (Kevin's ruling: place with warning, never refuse).
                    unverified = 1;
                    for (int a = 1; a < nRows; a++)       // tiny insertion sort
                        for (int b2 = a; b2 > 0 && rows[b2] < rows[b2 - 1]; b2--) {
                            int t = rows[b2]; rows[b2] = rows[b2 - 1]; rows[b2 - 1] = t;
                        }
                }
                if (res.type == PartType::RESISTOR) measOhms = res.value;
                // say what was measured - accepted or not, the user hears it
                Serial.print("\r\nPARTID type=");
                Serial.print(partTypeName(res.type));
                Serial.print(" conf=");
                Serial.print(res.confidence, 2);
                if (res.value != 0.0f) {
                    Serial.print(" value=");
                    Serial.print(res.value, 3);
                }
                if (res.value2 != 0.0f) {
                    Serial.print(" value2=");
                    Serial.print(res.value2, 1);
                }
                if (res.status != 0) {
                    Serial.print(" status=");
                    Serial.print((int)res.status);
                }
                if (res.lifted > 0) {
                    Serial.print(" lifted=");
                    Serial.print((int)res.lifted);
                }
                Serial.println(unverified ? " pins=assumed" : " pins=verified");
                if (oled.oledConnected) {
                    char toast[64];
                    if (idOk == 1) {
                        snprintf(toast, sizeof(toast), "%s ok\n%s",
                                 partTypeName(res.type),
                                 (res.type == PartType::LED)
                                     ? partLedColorGuess(res.value) : "pins mapped");
                    } else if (idOk == 2 && res.type == PartType::RESISTOR) {
                        snprintf(toast, sizeof(toast), "%.0f ohm", (double)res.value);
                    } else if (idOk == 2) {
                        snprintf(toast, sizeof(toast), "placed");
                    } else {
                        snprintf(toast, sizeof(toast), "couldn't\nverify pins");
                    }
                    oled.clearPrintShow(toast, 2, true, true, true);
                    delay(700);
                }
            } else {
                bool cancelled = false;
                for (int s = 0; s < nSignals; s++) {
                    int row = partsTapForRow(rec, po.pins[s].name, s + 1,
                                             nSignals, rows, nRows);
                    if (row == -2) goto done;
                    if (row == -1) { cancelled = true; break; }
                    rows[nRows++] = row;
                    Serial.print("\r\nPARTPICK sig=");
                    Serial.print(po.pins[s].name);
                    Serial.print(" row=");
                    Serial.println(row);
                }
                if (cancelled) continue;   // back to the part list
            }
            if (partsCommitPlacement(rec, rows, nRows, unverified, measOhms)) {
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

// Hold the current OLED content until the user presses something: probe
// button or encoder click/hold continue, a serial byte exits the whole app
// (the picker convention). Returns 0 = continue, -2 = exit.
static int partsWaitForPress(void) {
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
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
            while (encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
                   encoderButtonState == LONG_HELD) {
                jOS.serviceInner();
                rotaryEncoderButtonStuff();
                delayMicroseconds(1000);
            }
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            return 0;
        }
        if (partsProbeButton() != 0) return 0;
        if (Serial.available() > 0) {
            (void)Serial.read();
            return -2;
        }
        delayMicroseconds(1000);
    }
}

// The transistor result card (Kevin's layout):
//     PNP          2N3906
//     17   18   19
//     E    B    C
//     hFE 401    Vbe 0.61V
// No header bar - and the breadboard drops its text and paints each pin's
// full row in the standing role colors while the card is up.
static void partsShowBjtResult(const PartDefinition& p, const PartResult& res) {
    // sort the three (row, role) pairs by row so columns read left-to-right
    int rowsS[3];
    PinRole rolesS[3];
    for (int i = 0; i < 3; i++) {
        rowsS[i] = (int)res.rows[i];
        rolesS[i] = res.roles[i];
    }
    for (int a = 1; a < 3; a++)
        for (int b2 = a; b2 > 0 && rowsS[b2] < rowsS[b2 - 1]; b2--) {
            int tr = rowsS[b2]; rowsS[b2] = rowsS[b2 - 1]; rowsS[b2 - 1] = tr;
            PinRole tp = rolesS[b2]; rolesS[b2] = rolesS[b2 - 1]; rolesS[b2 - 1] = tp;
        }

    if (oled.oledConnected) {
        char rowLine[24], roleLine[24], hfeBuf[16], vbeBuf[16];
        snprintf(rowLine, sizeof(rowLine), "%-5d%-5d%d", rowsS[0], rowsS[1], rowsS[2]);
        snprintf(roleLine, sizeof(roleLine), "%-5s%-5s%s",
                 pinRoleName(rolesS[0]), pinRoleName(rolesS[1]), pinRoleName(rolesS[2]));
        snprintf(hfeBuf, sizeof(hfeBuf), "hFE %.0f", (double)res.value2);
        snprintf(vbeBuf, sizeof(vbeBuf), "Vbe %.2fV", (double)res.value);
        const char* typeStr = (res.type == PartType::BJT_PNP) ? "PNP" : "NPN";
        const int16_t f = 12;  // Andale Mono 5pt - four rows fit 32px
        OledTextRow rows[4] = {};
        rows[0].segs[0] = {typeStr, f, OLED_ALIGN_INHERIT};
        rows[0].segs[1] = {p.name, f, OLED_ALIGN_RIGHT};
        rows[0].segCount = 2;
        rows[0].align = OLED_ALIGN_LEFT;
        rows[1].segs[0] = {rowLine, f, OLED_ALIGN_INHERIT};
        rows[1].segCount = 1;
        rows[1].align = OLED_ALIGN_LEFT;
        rows[2].segs[0] = {roleLine, f, OLED_ALIGN_INHERIT};
        rows[2].segCount = 1;
        rows[2].align = OLED_ALIGN_LEFT;
        rows[3].segs[0] = {hfeBuf, f, OLED_ALIGN_INHERIT};
        rows[3].segs[1] = {vbeBuf, f, OLED_ALIGN_RIGHT};
        rows[3].segCount = 2;
        rows[3].align = OLED_ALIGN_LEFT;
        for (int i = 0; i < 4; i++) rows[i].fixedH = 7;
        oled.clearPrintShowRich(rows, 4, 1, true, true, true);
    }

    // breadboard: text off, each pin's whole row in its role color
    b.clear();
    for (int i = 0; i < 3; i++) {
        uint32_t c = (rolesS[i] == PinRole::E)   ? PARTS_ROLE_E_COLOR
                     : (rolesS[i] == PinRole::B) ? PARTS_ROLE_B_COLOR
                                                 : PARTS_ROLE_C_COLOR;
        int pr = nodeToPrintRow(rowsS[i]);
        if (pr >= 0) b.printRawRow(0b00011111, pr, c, 0xffffff);
    }
    requestLedShow(2);
}

// ============================================================================
// Test Part - re-measure a placed part in place
// ============================================================================
// Menu: Parts > Test. Picks a placed part, runs the PartClassify session on
// its pin rows, and reports what the part electrically IS right now - hFE,
// Vbe, Vf (with an LED color guess), ohms. A clean identification that
// matches the placement clears the pins_unverified warning; wiring on the
// part's rows refuses gently (never energize user wiring). 2-3 leg parts
// only for now - DIPs and modules need the vector runner (phase 2).
void partsTestLauncher(void) {
    if (globalState.parts.numParts <= 0) {
        Serial.println("\r\nPARTID test n=0 (no parts placed)");
        if (oled.oledConnected)
            oled.clearPrintShow("no parts\nplaced", 2, true, true, true);
        delay(900);
        return;
    }

    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    int pick = 0;
    while (true) {
        int n = 0;
        for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS &&
                        n < PARTS_LIST_MAX; i++) {
            const PartDefinition& p = globalState.parts.parts[i];
            if (!p.placed) continue;
            s_led[n] = p.name;
            s_title[n] = p.name;
            s_desc[n] = p.typeStr[0] ? p.typeStr : "part";
            s_rec[n] = (uint16_t)i;
            n++;
        }
        if (n == 0) break;
        int sel = partsPicker("test", "Test", n, pick);
        if (sel == -1) break;        // hold = leave
        if (sel == -2) break;        // serial byte = leave
        pick = sel;
        PartDefinition& p = globalState.parts.parts[s_rec[sel]];

        // Display parts: the ambient service IS the test - it beacons,
        // inits and flushes frames to this exact panel all day.
        if (partdbResolveDriver(p) != nullptr) {
            uint32_t frames = 0;
            int alive = DisplayService::getInstance().aliveStateFor(p.name, &frames);
            Serial.print("\r\nPARTID test part=");
            Serial.print(p.name);
            Serial.print(" display=");
            Serial.println(alive == 1 ? "alive" : (alive == 0 ? "quiet" : "unbound"));
            char l1[24], l2[24];
            if (alive == 1) {
                snprintf(l1, sizeof(l1), "alive");
                snprintf(l2, sizeof(l2), "%lu frames", (unsigned long)frames);
            } else if (alive == 0) {
                snprintf(l1, sizeof(l1), "not answering");
                snprintf(l2, sizeof(l2), "check power");
            } else {
                snprintf(l1, sizeof(l1), "not bound");
                snprintf(l2, sizeof(l2), "another display?");
            }
            ReadingDisplay::show(p.name, p.baseRow, l1, l2);
            if (partsWaitForPress() == -2) break;
            continue;
        }

        // the part's electrical footprint: every pin that resolves to a row,
        // with the power pins remembered for the DIP presence test
        int rows[3];
        int nRows = 0;
        bool tooMany = false;
        int gndRow = -1, vccRow = -1, sigRow = -1;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            if (p.pins[j].pinClass == 3) continue;   // nc
            int node = partPinNode(p, p.pins[j]);
            if (node < 1 || node > 60) continue;
            if (p.pins[j].pinClass == 2 && gndRow < 0) gndRow = node;
            if (p.pins[j].pinClass == 1 && vccRow < 0) vccRow = node;
            if (p.pins[j].pinClass == 0 && sigRow < 0) sigRow = node;
            if (nRows >= 3) { tooMany = true; continue; }  // keep scanning for gnd/vcc
            rows[nRows++] = node;
        }
        if (tooMany || nRows < 2) {
            // DIP logic/analog: an unpowered chip's ESD network is a diode
            // from the GND pin toward everything and from everything toward
            // VCC. Real logic needs the phase-2 vector runner; this answers
            // "is a chip actually seated here" - through the same session,
            // so its power wires get briefly lifted and restored too.
            if (p.footprint == 1 && gndRow >= 1 && vccRow >= 1 &&
                gndRow != vccRow && sigRow != gndRow && sigRow != vccRow) {
                if (oled.oledConnected) {
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText("testing...");
                }
                PartResult res = (sigRow >= 1)
                                     ? identifyThreeLead(gndRow, sigRow, vccRow)
                                     : identifyTwoLead(gndRow, vccRow);
                Serial.print("\r\nPARTID test part=");
                Serial.print(p.name);
                Serial.print(" esd_gnd_sig=");
                Serial.print(res.jmap[0][1], 2);
                Serial.print(" esd_gnd_vcc=");
                Serial.print(res.jmap[0][2], 2);
                Serial.print(" esd_sig_vcc=");
                Serial.print(res.jmap[1][2], 2);
                if (res.status != 0) { Serial.print(" status="); Serial.print((int)res.status); }
                if (res.lifted > 0) { Serial.print(" lifted="); Serial.print((int)res.lifted); }
                Serial.println();
                char l1[24], l2[24] = "";
                if (res.status != 0) {
                    snprintf(l1, sizeof(l1), (res.status == -3) ? "too wired" : "busy");
                    snprintf(l2, sizeof(l2), (res.status == -3) ? "to test" : "try again");
                } else {
                    float vf = 9.9f;
                    if (res.nRows == 3) {
                        if (res.jmap[0][1] < 1.1f && res.jmap[0][1] < vf) vf = res.jmap[0][1];
                        if (res.jmap[0][2] < 1.1f && res.jmap[0][2] < vf) vf = res.jmap[0][2];
                        if (res.jmap[1][2] < 1.1f && res.jmap[1][2] < vf) vf = res.jmap[1][2];
                    } else if (res.type == PartType::DIODE || res.type == PartType::ZENER) {
                        vf = res.value;
                    }
                    if (vf < 1.1f) {
                        snprintf(l1, sizeof(l1), "chip present");
                        snprintf(l2, sizeof(l2), "ESD %.2fV", (double)vf);
                        Serial.println("  protection diodes answer - a chip is seated (logic itself needs the vector runner)");
                    } else {
                        snprintf(l1, sizeof(l1), "no ESD reply");
                        snprintf(l2, sizeof(l2), "seated?");
                        Serial.println("  no protection-diode response on the probed pins - is the chip seated?");
                    }
                }
                ReadingDisplay::show(p.name, p.baseRow, l1, l2[0] ? l2 : nullptr);
                if (partsWaitForPress() == -2) break;
                continue;
            }
            Serial.println("\r\nPARTID test unsupported for this part yet");
            if (oled.oledConnected)
                oled.clearPrintShow("can't test\nthis one yet", 2, true, true, true);
            delay(900);
            continue;
        }

        if (oled.oledConnected) {
            oled.resetMultiLineSmallText();
            oled.showMultiLineSmallText("testing...");
        }
        PartResult res = (nRows == 3)
                             ? identifyThreeLead(rows[0], rows[1], rows[2])
                             : identifyTwoLead(rows[0], rows[1]);

        // terminal: the same machine grammar as placement
        Serial.print("\r\nPARTID test part=");
        Serial.print(p.name);
        Serial.print(" type=");
        Serial.print(partTypeName(res.type));
        Serial.print(" conf=");
        Serial.print(res.confidence, 2);
        if (res.value != 0.0f) { Serial.print(" value=");  Serial.print(res.value, 3); }
        if (res.value2 != 0.0f) { Serial.print(" value2="); Serial.print(res.value2, 1); }
        if (res.status != 0) { Serial.print(" status="); Serial.print((int)res.status); }
        Serial.println();

        char line1[24] = "";
        char line2[24] = "";
        bool bjtCard = false;
        if (res.lifted > 0) {
            Serial.print("  briefly unwired ");
            Serial.print((int)res.lifted);
            Serial.println(res.lifted == 1 ? " wire to test - it's back"
                                           : " wires to test - they're back");
        }
        if (res.status == -3) {
            snprintf(line1, sizeof(line1), "too wired");
            snprintf(line2, sizeof(line2), "to test");
            Serial.println("  more wiring than a brief unwire can hold - clear a few connections first");
        } else if (res.status != 0) {
            snprintf(line1, sizeof(line1), "busy");
            Serial.println("  measurement machinery is busy - try again in a moment");
        } else {
            switch (res.type) {
                case PartType::BJT_PNP:
                case PartType::BJT_NPN:
                    bjtCard = true;   // the dedicated card, no header bar
                    break;
                case PartType::LED:
                    snprintf(line1, sizeof(line1), "LED %.10s", partLedColorGuess(res.value));
                    snprintf(line2, sizeof(line2), "Vf %.2fV", (double)res.value);
                    break;
                case PartType::DIODE:
                    snprintf(line1, sizeof(line1), "diode");
                    snprintf(line2, sizeof(line2), "Vf %.2fV", (double)res.value);
                    break;
                case PartType::ZENER:
                    snprintf(line1, sizeof(line1), "zener");
                    snprintf(line2, sizeof(line2), "Vz %.1fV", (double)res.value2);
                    break;
                case PartType::RESISTOR: {
                    char ohms[12];
                    formatOhms(res.value, ohms, sizeof(ohms));
                    snprintf(line1, sizeof(line1), "%s", ohms);
                    snprintf(line2, sizeof(line2), "measured");
                    p.measuredOhms = res.value;
                    break;
                }
                case PartType::POT: {
                    char ohms[12];
                    formatOhms(res.value, ohms, sizeof(ohms));
                    snprintf(line1, sizeof(line1), "pot %s", ohms);
                    snprintf(line2, sizeof(line2), "wiper %.0f%%",
                             (double)(res.value2 * 100.0f));
                    p.measuredOhms = res.value;
                    break;
                }
                case PartType::CAPACITOR:
                    snprintf(line1, sizeof(line1), "capacitor");
                    break;
                case PartType::NFET:
                case PartType::PFET:
                    snprintf(line1, sizeof(line1), "%cFET",
                             (res.type == PartType::NFET) ? 'N' : 'P');
                    snprintf(line2, sizeof(line2), "Vf(body) %.2fV", (double)res.value);
                    break;
                case PartType::EMPTY:
                    snprintf(line1, sizeof(line1), "nothing");
                    snprintf(line2, sizeof(line2), "conducting");
                    break;
                default:
                    snprintf(line1, sizeof(line1), "unclear");
                    break;
            }
            // a clean identification whose roles sit exactly where the
            // placement put them = the pins are verified
            if (res.status == 0 && !res.degraded && res.confidence >= 0.8f) {
                bool rolesMatch = true;
                for (int t = 0; t < res.nRows; t++) {
                    const char* roleName = pinRoleName(res.roles[t]);
                    if (strcmp(roleName, "LEAD") == 0) continue;  // nonpolar
                    bool found = false;
                    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                        if (partPinNode(p, p.pins[j]) == (int)res.rows[t] &&
                            strcmp(p.pins[j].name, roleName) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { rolesMatch = false; break; }
                }
                if (rolesMatch && p.pinsUnverified) {
                    p.pinsUnverified = 0;
                    partLabels.requestRun();
                    Serial.println("  pins verified - warning cleared");
                } else if (!rolesMatch) {
                    Serial.println("  measured roles don't sit where this part was placed - check its legs");
                }
            }
        }
        if (bjtCard) {
            partsShowBjtResult(p, res);
        } else {
            ReadingDisplay::show(p.name, p.baseRow, line1[0] ? line1 : nullptr,
                                 line2[0] ? line2 : nullptr);
        }
        // the reading stays up until the user says they've read it
        if (partsWaitForPress() == -2) break;
    }

    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    requestLedShow(-1);
    Serial.println();
    oled.showJogo32h();
}

// ============================================================================
// Auto Scan - sweep the whole board and say what's on it
// ============================================================================
// Menu: Parts > Auto. Census-pokes every free row (charge-share, ~2s),
// clusters the hits into spans, then identifies the 2-3 leg spans with the
// same machinery placement uses. Wired rows belong to the user's netlist
// and are left alone; anything bigger than 3 legs is reported as presence
// (the phase-2 vector runner will name chips). Abortable at every step -
// probe button, encoder, or any serial byte.

static bool partsAutoAborted = false;
static bool partsAutoAbortCheck(void) {
    if (partsAutoAborted) return true;
    jOS.serviceInner();
    rotaryEncoderButtonStuff();
    if (encoderButtonState == HELD ||
        (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED)) {
        partsAutoAborted = true;
    } else if (partsProbeButton() != 0) {
        partsAutoAborted = true;
    } else if (Serial.available() > 0) {
        (void)Serial.read();
        partsAutoAborted = true;
    }
    return partsAutoAborted;
}

// The placed part (if any) with a pin on this row, for "already yours" tags.
static const char* partsPlacedPartOnRow(int row) {
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++)
            if (partPinNode(p, p.pins[j]) == row) return p.name;
    }
    return nullptr;
}

void partsAutoLauncher(void) {
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;
    partsAutoAborted = false;

    b.clear();
    b.print("SCAN", PARTS_HEADER_COLOR, 0xFFFFFF, 0, 0, 1);
    requestLedShow(2);
    if (oled.oledConnected) {
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText("scanning the board...\n(any press stops)");
    }
    Serial.println("\r\nPARTSCAN auto begin");
    unsigned long scanT0 = millis();

    static uint8_t flags[61];
    static float v0[61], v1[61];
    int found = partScanCensus(flags, v0, v1, partsAutoAbortCheck);
    if (found < 0) {
        Serial.println("PARTSCAN auto busy - try again in a moment");
        if (oled.oledConnected)
            oled.clearPrintShow("scan busy\ntry again", 2, true, true, true);
        delay(900);
        goto adone;
    }
    if (partsAutoAborted) {
        Serial.println("PARTSCAN auto aborted");
        goto adone;
    }
    {
        // second pass: isolated junction parts are invisible to the poke
        // (they pre-charge through their own junctions) - sweep adjacent
        // free pairs at 1V and see what conducts
        if (oled.oledConnected) {
            oled.resetMultiLineSmallText();
            oled.showMultiLineSmallText("scanning pairs...\n(any press stops)");
        }
        int pairHits = partScanPairSweep(flags, partsAutoAbortCheck);
        if (pairHits > 0) found += pairHits;
    }
    if (partsAutoAborted) {
        Serial.println("PARTSCAN auto aborted");
        goto adone;
    }

    {
        // the census, row by row, for the curious (and for tuning)
        Serial.print("PARTSCAN census hits=");
        Serial.println(found);
        for (int r = 1; r <= 60; r++) {
            if (flags[r] != 1 && flags[r] != 5) continue;
            Serial.print(flags[r] == 1 ? "  row " : "  row+ ");
            Serial.print(r);
            Serial.print(" v0=");
            Serial.print(v0[r], 2);
            Serial.print(" v1=");
            Serial.println(v1[r], 2);
        }

        // cluster hits into spans per half; a 1-row gap stays inside the
        // span (a PNP base row can hold its charge while E and C slump)
        int spanStart[12], spanEnd[12];
        int nSpans = 0;
        for (int half = 0; half < 2; half++) {
            int lo = half ? 31 : 1, hi = half ? 58 : 28;
            int cur = -1, last = -1;
            uint8_t curKind = 0;
            for (int r = lo; r <= hi + 1; r++) {
                bool hit = (r <= hi) && (flags[r] == 1 || flags[r] == 5);
                // two different parts can abut on neighboring rows - a
                // hold-signature cluster (active pins, flag 1) and a
                // pair-conduction cluster (passive junctions, flag 5) are
                // different animals, so a kind change closes the span
                // (bench: the 7400 at 11-16 and the 2N3906 at 17-19)
                if (hit && cur >= 0 && flags[r] != curKind) {
                    if (nSpans < 12) {
                        spanStart[nSpans] = cur;
                        spanEnd[nSpans] = last;
                        nSpans++;
                    }
                    cur = -1;
                }
                if (hit) {
                    if (cur < 0) { cur = r; curKind = flags[r]; }
                    last = r;
                } else if (cur >= 0 && r > last + 1) {
                    // gap of 2+ (or the end): close the span
                    if (nSpans < 12) {
                        spanStart[nSpans] = cur;
                        spanEnd[nSpans] = last;
                        nSpans++;
                    }
                    cur = -1;
                }
            }
        }

        Serial.print("PARTSCAN spans=");
        Serial.println(nSpans);
        int shown = 0;
        char summary[96] = "";
        size_t sumLen = 0;
        for (int sp = 0; sp < nSpans && !partsAutoAborted; sp++) {
            int a = spanStart[sp], z = spanEnd[sp];
            int width = z - a + 1;
            const char* owner = nullptr;
            for (int r = a; r <= z && owner == nullptr; r++)
                owner = partsPlacedPartOnRow(r);

            char line[64] = "";
            if (width == 1) {
                // a lone hit: one leg of something bigger, or noise - say so
                snprintf(line, sizeof(line), "row %d: one leg of something?", a);
            } else if (width <= 3 && owner == nullptr) {
                Serial.print("  checking rows ");
                Serial.print(a);
                Serial.print("-");
                Serial.print(z);
                Serial.println("...");
                Serial.flush();
                if (oled.oledConnected) {
                    char t[48];
                    snprintf(t, sizeof(t), "rows %d-%d:\nchecking...", a, z);
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText(t);
                }
                if (partsAutoAbortCheck()) break;
                PartResult res = (width == 3) ? identifyThreeLead(a, a + 1, z)
                                              : identifyTwoLead(a, z);
                if (width == 2 && (res.type == PartType::DIODE ||
                                   res.type == PartType::ZENER)) {
                    // two junction-legs might be a transistor missing its
                    // quiet third pin - try a free neighbor on either side
                    int half2Lo = (a <= 28) ? 1 : 31;
                    int half2Hi = (a <= 28) ? 28 : 58;
                    int extra = (z + 1 <= half2Hi && flags[z + 1] == 0) ? z + 1
                                : (a - 1 >= half2Lo && flags[a - 1] == 0) ? a - 1
                                                                          : -1;
                    if (extra > 0 && !partsAutoAbortCheck()) {
                        PartResult res3 = (extra > z) ? identifyThreeLead(a, z, extra)
                                                      : identifyThreeLead(extra, a, z);
                        if (res3.status == 0 &&
                            (res3.type == PartType::BJT_PNP ||
                             res3.type == PartType::BJT_NPN ||
                             res3.type == PartType::NFET ||
                             res3.type == PartType::PFET)) {
                            res = res3;
                            z = (extra > z) ? extra : z;
                            a = (extra < a) ? extra : a;
                        }
                    }
                }
                if (res.status == 0 && res.type != PartType::EMPTY &&
                    res.type != PartType::UNKNOWN) {
                    char detail[24] = "";
                    if (res.type == PartType::RESISTOR || res.type == PartType::POT)
                        formatOhms(res.value, detail, sizeof(detail));
                    else if (res.value != 0.0f)
                        snprintf(detail, sizeof(detail), "%.2fV", (double)res.value);
                    snprintf(line, sizeof(line), "rows %d-%d: %s %s", a, z,
                             partTypeName(res.type), detail);
                    // paint the span - BJTs get the standing role colors
                    if (res.type == PartType::BJT_PNP || res.type == PartType::BJT_NPN) {
                        for (int t = 0; t < res.nRows; t++) {
                            uint32_t c = (res.roles[t] == PinRole::E)   ? PARTS_ROLE_E_COLOR
                                         : (res.roles[t] == PinRole::B) ? PARTS_ROLE_B_COLOR
                                                                        : PARTS_ROLE_C_COLOR;
                            int pr = nodeToPrintRow((int)res.rows[t]);
                            if (pr >= 0) b.printRawRow(0b00011111, pr, c, 0xffffff);
                        }
                    } else {
                        for (int r = a; r <= z; r++) {
                            int pr = nodeToPrintRow(r);
                            if (pr >= 0) b.printRawRow(0b00011111, pr, partsTapHue(sp, nSpans, false), 0xffffff);
                        }
                    }
                    requestLedShow(2);
                } else {
                    snprintf(line, sizeof(line), "rows %d-%d: something (unclear)", a, z);
                }
            } else if (owner != nullptr) {
                snprintf(line, sizeof(line), "rows %d-%d: %s (placed)", a, z, owner);
            } else {
                snprintf(line, sizeof(line), "rows %d-%d: %d legs (a chip?)", a, z, width);
                for (int r = a; r <= z; r++) {
                    int pr = nodeToPrintRow(r);
                    if (pr >= 0) b.printRawRow(0b00011111, pr, partsTapHue(sp, nSpans, false), 0xffffff);
                }
                requestLedShow(2);
            }
            Serial.print("\r\n  ");
            Serial.println(line);
            if (shown < 2 && sumLen + strlen(line) + 2 < sizeof(summary)) {
                sumLen += (size_t)snprintf(summary + sumLen, sizeof(summary) - sumLen,
                                           "%s%s", shown ? "\n" : "", line);
                shown++;
            }
        }

        if (partsAutoAborted) {
            Serial.println("PARTSCAN auto aborted");
        } else {
            Serial.print("PARTSCAN auto done in ");
            Serial.print((millis() - scanT0) / 1000);
            Serial.println("s (add parts via Parts > Place)");
            if (oled.oledConnected) {
                if (nSpans == 0) {
                    oled.clearPrintShow("board looks\nempty", 2, true, true, true);
                } else {
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText(summary);
                }
            }
            partsWaitForPress();
        }
    }

adone:
    partsAutoAborted = false;
    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    requestLedShow(-1);
    Serial.println();
    oled.showJogo32h();
}
