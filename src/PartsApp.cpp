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
#include "remembering/FileParsing.h"  // add/removeBridgeFromState (scan board lift)
#include "routing/InfraPaths.h"       // infraIsBridge - never lift infra's own
#include "eyecandy/ReadingDisplay.h"  // measured values on the OLED
#include "eyecandy/Colors.h"           // termColorLikeLed - serial wears LED hues
#include "eyecandy/Highlighting.h"    // highlightingInvalidatePartFocus
#include "displays/DisplayService.h"   // display liveness (Parts > Test)
#include "guiding/GuideScript.h"      // formatOhms
#include "Undo.h"           // UndoIngestGuard - placements are not undoable (yet)
#include "States.h"         // globalState
#include "config.h"         // jumperlessConfig.hardware.probe_revision
#include "oled.h"
#include "Peripherals.h"    // INA1 (the power-up watchdog), setDac0voltage
#include <Wire.h>           // Wire1 - the chip-cluster I2C probe
#include "hardware/gpio.h"  // pad reads + function bookkeeping for the probe

// The canonical part removal (bridges, net names, undo guard, refresh) -
// commit's replace-on-identity reuses it. Lives in JumperlessMicroPythonAPI.
extern "C" int jl_remove_part(const char* name);

// Forward declarations for the identify surfaces: partsTestLauncher sits
// ABOVE the scan machinery it borrows (cluster rails, the Tier-3 runner) -
// definitions live with their siblings further down.
struct ClusterPower {
    int gndRow = -1;
    int vddRow = -1;
    int sig[6] = {0};
    int nSig = 0;
};
struct ChipIdentify {
    int nTried = 0;
    int nPass = 0;
    VectorIdentifyResult res[8];
    char fp[MAX_PART_PINS + 1];
};
static bool partsFindClusterPower(const int* rows, int nRows,
                                  ClusterPower* out);
static bool partsCornerRails(int lo, int hi, ClusterPower* out);
static int partsChipPinRow(int baseRow, int nPins, bool rotated, int pin);
static void partsIdentifyChip(int baseRow, int width, int* gndRow, int* vddRow,
                              ChipIdentify* out);
static int partsConfirmOne(const char* verb, const char* what,
                           const char* detail, uint32_t rgb = 0);
static bool partsAutoAborted = false;   // the scan-flow abort latch
                                        // (partsAutoAbortCheck sets it)
// The canonical routable-GPIO setters (extern "C", same file): index 1-8,
// dir 0 = output, 1 = input. The found-display wiring drives these.
extern "C" void jl_gpio_set(int pin, int value);
extern "C" void jl_gpio_set_dir(int pin, int direction);
// The card paints the OLED outside the live-reading pipeline; this drops
// that pipeline's dedupe key and reprint guards so the next reading draws
// (Highlighting.cpp - the scroll-back-doesn't-update fix, 08:38).
extern "C" void highlightingNoteExternalOledPaint(void);

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
// The diode counterpart, same warm-to-cool read: current enters at RED (A)
// and leaves at BLUE (K).
static const uint32_t PARTS_ROLE_A_COLOR = 0x2A0000;
static const uint32_t PARTS_ROLE_K_COLOR = 0x00062A;

// ---- the scan's one color language (Kevin, 2026-08-29: "rainbowy vibe
// while still being informative") ----
// Each verdict wears ONE hue, on the board and in the terminal alike
// (termColorLikeLed folds the same RGB into xterm-256), so the LEDs and
// the serial log tell the same story. LED values are board-dim; the
// terminal brightens the hue before quantizing.
static uint32_t partsLedGuessColor(float vf) {
    // partLedColorGuess's bands, as paint: the row wears the color the
    // LED would glow
    if (vf < 1.9f)  return 0x280000;   // infrared reads as deep red
    if (vf < 2.1f)  return 0x300000;   // red
    if (vf < 2.25f) return 0x2A1600;   // orange/yellow
    if (vf < 2.5f)  return 0x043000;   // green
    if (vf < 2.9f)  return 0x261408;   // the overlapping band - amber
    if (vf < 3.6f)  return 0x08142E;   // blue/white
    return 0x1A0030;                   // violet/UV
}
static uint32_t partsTypeColor(PartType t, float value) {
    switch (t) {
        case PartType::RESISTOR:
        case PartType::POT:           return 0x2C2200;  // amber
        case PartType::CAPACITOR:     return 0x00182E;  // deep blue
        case PartType::DIODE:         return 0x2E0E00;  // orange
        case PartType::ZENER:         return 0x1C0030;  // violet
        case PartType::LED:           return partsLedGuessColor(value);
        case PartType::BJT_PNP:
        case PartType::BJT_NPN:       return 0x2C0016;  // magenta
        case PartType::NFET:
        case PartType::PFET:          return 0x00281C;  // teal
        case PartType::SHORT_CIRCUIT: return 0x300004;  // red
        case PartType::EMPTY:         return 0x000000;
        default:                      return 0x0E0E0E;  // gray: unclear
    }
}
static uint32_t partsBrighten(uint32_t c) {
    uint32_t r = ((c >> 16) & 0xFF) * 2, g = ((c >> 8) & 0xFF) * 2,
             b2 = (c & 0xFF) * 2;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b2 > 255) b2 = 255;
    return (r << 16) | (g << 8) | b2;
}
// Row color for a finding's paint: junction legs keep the standing role
// colors (E/B/C, A/K), everything else wears its type hue, and an LED's
// rows glow the color the part would - anode brightened so polarity reads.
static uint32_t partsResultRowColor(const PartResult& res, int t) {
    switch (res.roles[t]) {
        case PinRole::E: return PARTS_ROLE_E_COLOR;
        case PinRole::B: return PARTS_ROLE_B_COLOR;
        case PinRole::C: return PARTS_ROLE_C_COLOR;
        case PinRole::A: return (res.type == PartType::LED)
                             ? partsBrighten(partsLedGuessColor(res.value))
                             : PARTS_ROLE_A_COLOR;
        case PinRole::K: return (res.type == PartType::LED)
                             ? partsLedGuessColor(res.value)
                             : PARTS_ROLE_K_COLOR;
        default: return partsTypeColor(res.type, res.value);
    }
}
static void partsPaintRow(int row, uint32_t c) {
    int pr = nodeToPrintRow(row);
    if (pr >= 0) b.printRawRow(0b00011111, pr, c, 0xffffff);
}
// "Under the meter": the rows a measurement is touching RIGHT NOW, in
// white (Kevin, 2026-08-30: "when you're running the part checking, the
// LEDs should also show what's being done"). The verdict's own color
// overwrites it when the reading lands.
static const uint32_t PARTS_METER_COLOR = 0x0C0C0E;
static int16_t s_meterRows[3] = {-1, -1, -1};
static void partsMeterViz2(int a, int b2) {
    s_meterRows[0] = (int16_t)a;
    s_meterRows[1] = (int16_t)b2;
    s_meterRows[2] = -1;
    partsPaintRow(a, PARTS_METER_COLOR);
    partsPaintRow(b2, PARTS_METER_COLOR);
    requestLedShow(2);
}
static void partsMeterViz3(int a, int b2, int c) {
    s_meterRows[0] = (int16_t)a;
    s_meterRows[1] = (int16_t)b2;
    s_meterRows[2] = (int16_t)c;
    partsPaintRow(a, PARTS_METER_COLOR);
    partsPaintRow(b2, PARTS_METER_COLOR);
    partsPaintRow(c, PARTS_METER_COLOR);
    requestLedShow(2);
}
// The liveness shimmer (partScanActivityHook): whenever a measurement is
// waiting on hardware - an INA conversion, a decay watch, a drain - the
// rows under the meter cycle a slow dim rainbow. Self-rate-limited to
// ~90ms a frame; a session that never waits never shimmers, and a board
// that IS waiting visibly breathes instead of looking hung (Kevin,
// 2026-08-30: "never seem like it's frozen for more than a quarter of a
// second").
static void partsMeterPulse(void) {
    static unsigned long lastMs = 0;
    unsigned long now = millis();
    if (now - lastMs < 90) return;
    lastMs = now;
    bool any = false;
    for (int k = 0; k < 3; k++) {
        int r = s_meterRows[k];
        if (r < 1 || r > 60) continue;
        hsvColor h;
        h.h = (uint8_t)((now / 12 + k * 40) & 0xFF);
        h.s = 200;
        h.v = 20;
        partsPaintRow(r, HsvToRaw(h));
        any = true;
    }
    if (any) requestLedShow(2);
}
// serial shorthands - all no-ops when terminal colors are disabled
static void partsTermRgb(uint32_t rgb) { termColorLikeLed(rgb, &Serial); }
static void partsTermReset() { changeTerminalColor(-1, false, &Serial, true); }
static void partsTermGood() { partsTermRgb(0x00E020); }
static void partsTermBad()  { partsTermRgb(0xE02010); }
static void partsTermDim()  { changeTerminalColor(244, false, &Serial, true); }
// the clamp-verdict palette: what the fingerprint paints on the pins AND
// the color each fp letter prints in - one key for both
static uint32_t partsFpColor(char c) {
    switch (c) {
        case 'G': return 0x002808;   // junction to GND (the TTL normal)
        case 'V': return 0x16002C;   // junction to VDD
        case 'B': return 0x001C1A;   // clamps both ways (CMOS-style)
        case 'T': return 0x2C1000;   // hard tie - a strapped pin
        case '-': return 0x0A0A0C;   // the rails themselves
        case 'N': return 0x050505;   // open - nothing conducts
        default:  return 0x0E0E0E;   // x: unprobed
    }
}
// clamp-fingerprint progress (partScanClampFingerprint's states) painted
// onto the chip's own pins as they are measured
static void partsFpViz(int row, int state) {
    if (state == 0) {   // this pin is the one under the meter now
        s_meterRows[0] = (int16_t)row;
        s_meterRows[1] = -1;
        s_meterRows[2] = -1;
    }
    static const char kStateLetter[7] = {0, 'N', 'G', 'V', 'B', 'T', '-'};
    if (state == 0) partsPaintRow(row, 0x141414);          // under the meter
    else if (state >= 1 && state <= 6)
        partsPaintRow(row, partsFpColor(kStateLetter[state]));
    requestLedShow(2);
}

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
                } else {
                    // else, not fall-through: the rowUsed branch above used
                    // to ALSO land here and print a contradictory second
                    // error (audit, 2026-08-27)
                    Serial.println("  row must be 1-60");
                    nDigits = 0;
                    digits[0] = '\0';
                }
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
// nRows == 1 is the DIP anchor flow: the tap means "pin 1 goes HERE",
// whichever half that is - a bottom-half anchor is the normal orientation,
// a TOP-half anchor is the same chip rotated 180 degrees (pin 1 top-right;
// States.h geometry, Kevin's ruling 2026-08-28). The two-tap flow validated
// the orientation before this is called, so nothing here second-guesses it.
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

// WHICH rail a VCC pin reaches for. This used to be pure geometry - the rail
// on the pin's own half - which is a coin toss dressed up as a rule: Kevin's
// 7447 is bipolar TTL, its VCC landed on row 40, and it got BOTTOM_RAIL,
// which was 5.00V that evening and 3.37V the same afternoon. Same placement,
// two different chips.
//
// So ask the board. A rail under ~1V is off and tells us nothing, and the
// only supply figure the DB carries today is the vector set's `supply` (the
// 5v/3v3 declaration authored per record) - honest but sparse, so geometry
// stays the tie-break and a board with matching rails places exactly as it
// used to. Nothing is hot until the user's rails are, so the worst case here
// is still "the wrong rail is off".
static int partsRailForVcc(const PartDbRecord& rec, int node) {
    int nearRail = (node <= 30) ? TOP_RAIL : BOTTOM_RAIL;
    int farRail = (node <= 30) ? BOTTOM_RAIL : TOP_RAIL;
    auto railVolts = [](int rail) {
        return (rail == TOP_RAIL) ? globalState.power.topRail
                                  : globalState.power.bottomRail;
    };
    float want = 0.0f;   // 0 = the record never says
    const PartDbVectorSet* vs = partdbVectorSetOf(rec);
    if (vs != nullptr) {
        if (vs->supply == PARTDB_VEC_SUPPLY_5V) want = 5.0f;
        else if (vs->supply == PARTDB_VEC_SUPPLY_3V3) want = 3.3f;
    }
    auto live = [&](int rail) { return railVolts(rail) >= 1.0f; };
    auto fits = [&](int rail) {
        if (want <= 0.0f) return true;
        float d = railVolts(rail) - want;
        return (d < 0.0f ? -d : d) <= 0.7f;
    };
    int pick = nearRail;
    if (live(nearRail) && fits(nearRail)) pick = nearRail;
    else if (live(farRail) && fits(farRail)) pick = farRail;
    else if (live(nearRail)) pick = nearRail;
    else if (live(farRail)) pick = farRail;
    if (want > 0.0f && (!live(pick) || !fits(pick))) {
        Serial.print("  note: ");
        Serial.print(rec.id);
        Serial.print(" wants ");
        Serial.print(want, 1);
        Serial.print("V and neither rail is there - VCC goes to the ");
        Serial.print(pick == TOP_RAIL ? "top" : "bottom");
        Serial.println(" rail anyway; set it before powering up");
    }
    return pick;
}

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
        // the tap IS pin 1's hole, either half: a top-half DIP anchor is
        // the rotated-180 encoding. The old `+30` here ASSUMED dot-bottom-
        // left and silently unrotated the chip.
        tmp.baseRow = (int16_t)rows[0];
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
    // bridges to a rail (partsRailForVcc picks WHICH), a gnd-class pin to
    // GND. Rails still obey the user, so nothing is hot until the rails are.
    // A row whose net already holds the OPPOSING special node is left
    // unrouted - bridging it would short rail to GND through our own bridge;
    // PartLabels' warning (VCC_TO_GND / GND_TO_HOT) tells the user instead.
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
            pin.connect = partsRailForVcc(rec, node);
        } else if (pin.pinClass == 2) {
            if (rowPowered) continue;
            pin.connect = GND;
        }
    }

    // Validate BEFORE the replace-on-identity removal below: geometry is
    // pure (no occupancy checks), and removing the old part first meant a
    // refused replacement left the user with NEITHER part (review finding).
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

    // Re-placing the same part in the same spot is an UPDATE, not a clone:
    // the bench accumulated 2N3906/_2/_3 at rows 17-19 and a stale 74153
    // shadowing the 7400. Every existing placed part with the same identity
    // (part_id + baseRow + footprint) comes out first, through the full
    // removal discipline (bridges, net names, undo guard). The first replaced
    // is kept for resurrection: applyPartPlacement can still refuse (bridge
    // table full), and that failure must not eat the part being updated.
    PartDefinition replacedCopy;
    bool haveReplaced = false;
    {
        for (int i = 0; i < st.parts.numParts;) {
            const PartDefinition& q = st.parts.parts[i];
            if (q.placed && q.baseRow == tmp.baseRow &&
                q.footprint == tmp.footprint &&
                strcmp(q.partId, tmp.partId) == 0 && tmp.partId[0] != '\0') {
                Serial.print("\r\nPARTDB replacing ");
                Serial.println(q.name);
                if (!haveReplaced) {
                    replacedCopy = q;   // copy before jl_remove_part memmoves
                    haveReplaced = true;
                }
                char replaced[16];
                strncpy(replaced, q.name, sizeof(replaced) - 1);
                replaced[sizeof(replaced) - 1] = '\0';
                jl_remove_part(replaced);
                continue;   // same index now holds the next part
            }
            i++;
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
        String refuseWhy = err;   // removePartPlacement reuses err - keep the message
        if (added >= 0) removePartPlacement(st, idx, err);
        st.parts.numParts--;   // roll the append back
        // a replace-on-identity removed the part being updated before this
        // refusal - put it back (its own bridges just came free again, so
        // its re-placement lives in the same conditions it lived in before)
        if (haveReplaced && st.parts.numParts < MAX_PARTS) {
            st.parts.parts[st.parts.numParts] = replacedCopy;
            String rerr;
            if (applyPartPlacement(st, st.parts.numParts, rerr) >= 0 &&
                rerr.length() == 0) {
                st.parts.numParts++;
                Serial.print("\r\nPARTDB replaced part restored: ");
                Serial.println(replacedCopy.name);
            } else {
                Serial.print("\r\nPARTDB could NOT restore ");
                Serial.println(replacedCopy.name);
            }
        }
        Serial.println("\r\nPARTDB place refused reason=\"" + refuseWhy + "\"");
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

// The ONE yes/no gesture set (Kevin, 2026-08-28): probe CONNECT or a short
// encoder click = yes, probe REMOVE or a hold = no. Every confirm responds
// to all four, so no prompt spends OLED lines on a button legend again -
// the paradigm IS the prompt, and `text` is the whole screen (multiline
// ok): the question plus whatever detail earns the lines the legend used
// to burn. Serial twins: y/Y = yes, n/N = no, line endings ignored, ANY
// other byte = -1 (the picker's exit convention - callers treat it as
// "stop prompting me").
int partsConfirmYesNo(const char* text) {
    if (oled.oledConnected) {
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText(text);
    }
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    while (true) {
        jOS.serviceInner();
        rotaryEncoderButtonStuff();
        int bp = partsProbeButton();   // once per pass (eaten-press rule)
        if (bp == 1) return 1;
        if (bp == 2) return 0;
        if (encoderButtonState == HELD) {
            // wait out the hold so the release can't echo into the caller
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
        if (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            return 1;
        }
        if (Serial.available() > 0) {
            char c = (char)Serial.read();
            if (c == 'y' || c == 'Y') return 1;
            if (c == 'n' || c == 'N') return 0;
            if (c == '\r' || c == '\n') continue;   // stray line ending
            return -1;
        }
        delayMicroseconds(1000);
    }
}

// B-M4 slice: clear every part off the table (their bridges come down via
// removePartPlacement - the invariant path), yes/no-confirmed. Returns
// true when parts were cleared.
static bool partsClearAll(void) {
    JumperlessState& st = globalState;
    if (st.parts.numParts <= 0) return false;

    // the legend's old lines now say WHAT is about to go: the part names,
    // as many as fit, "+N" for the rest
    char text[112];
    int len = snprintf(text, sizeof(text), "Clear %d part%s?\n",
                       st.parts.numParts, st.parts.numParts == 1 ? "" : "s");
    int listed = 0;
    for (int i = 0; i < st.parts.numParts && i < MAX_PARTS; i++) {
        const char* nm = st.parts.parts[i].name;
        if (len + (int)strlen(nm) + 7 >= (int)sizeof(text)) break;   // keep room for " +N"
        len += snprintf(text + len, sizeof(text) - len, "%s%s",
                        listed ? " " : "", nm);
        listed++;
    }
    if (listed < st.parts.numParts)
        snprintf(text + len, sizeof(text) - len, " +%d",
                 st.parts.numParts - listed);
    Serial.print("\r\nPARTS clear confirm n=");
    Serial.println(st.parts.numParts);
    Serial.flush();
    if (partsConfirmYesNo(text) != 1) return false;

    partsClearAllRecords();
    if (oled.oledConnected)
        oled.clearPrintShow("Parts\ncleared", 2, true, true, true);
    return true;
}

// The bulk clear itself, no questions asked - shared by Parts > Remove's
// All stop (partsClearAll above, after its confirm) and the `x` command
// (Kevin, 20:53: "x clear all connections should remove all parts too").
// Returns how many went.
int partsClearAllRecords(bool refresh) {
    JumperlessState& st = globalState;
    if (st.parts.numParts <= 0) return 0;
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
    highlightingInvalidatePartFocus();   // every part index just died
    if (refresh) refreshConnections(-1);
    partLabels.requestRun();
    Serial.print("\r\nPARTS cleared n=");
    Serial.println(n);
    Serial.flush();
    return n;
}

// Parts > Remove (Kevin's rulings, 2026-08-27 + 09:12 today): TWO ways in,
// split the way the control surfaces split. The probe does the precise
// work: tap a leg of a placed part to remove THAT LEG - its bridge, its
// net name, its record entry - and removing the last leg removes the part
// itself ("if we remove every node from a part... we should also remove
// the part"). The wheel does the coarse work: scroll through the placed
// parts - each one highlighted on the board in its placement rainbow with
// its card on the panel - and a click removes the WHOLE highlighted part.
// The scroll's last stop is All, which clears every part through the same
// CONNECT-confirmed flow Parts > Clear used to own. Exits: hold, probe
// REMOVE, or a click with nothing selected (the tap-only habit); a serial
// byte exits; a typed row number + enter removes that leg without the
// probe (the placement convention). Returns true when All cleared the
// board - the launcher exits the app, the board is ambient.
int jl_remove_part(const char* name);   // JumperlessMicroPythonAPI.cpp
extern "C" int jl_remove_part_pin(const char* name, const char* pinName,
                                  int rowHint);
static bool partsRemoveByTap(void) {
    JumperlessState& st = globalState;
    bool armed = false;
    bool liftHinted = false;
    unsigned long openMs = millis();
    unsigned long lastEmitMs = 0;
    unsigned long lastShowRequest = 0;
    char digits[4] = "";
    int nDigits = 0;
    // -1 = nothing selected (the tap prompt), 0..numParts-1 = that part,
    // numParts = All. A removal can compact the table under the selection;
    // the loop top clamps before every draw.
    int sel = -1;
    bool needsDraw = true;

    auto paintSel = [&](void) {
        b.clear();
        if (sel >= 0 && sel < st.parts.numParts) {
            const PartDefinition& p = st.parts.parts[sel];
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                int node = partPinNode(p, p.pins[j]);
                if (node >= 1 && node <= 60)
                    b.lightUpNode(node, partsTapHue(j, p.numPins, true));
            }
        } else if (sel >= st.parts.numParts) {
            // All: every part in its own hue, so the sweep reads as "these
            // are the parts", not one anonymous wash
            for (int i = 0; i < st.parts.numParts && i < MAX_PARTS; i++) {
                const PartDefinition& p = st.parts.parts[i];
                if (!p.placed) continue;
                for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                    int node = partPinNode(p, p.pins[j]);
                    if (node >= 1 && node <= 60)
                        b.lightUpNode(node, partsTapHue(i, st.parts.numParts, false));
                }
            }
        }
        requestLedShow(2);
        lastShowRequest = millis();
    };
    auto drawSel = [&](void) {
        if (sel < 0) {
            if (oled.oledConnected) {
                char t[64];
                snprintf(t, sizeof(t), "tap a leg or scroll\n%d placed  (hold = done)",
                         (int)st.parts.numParts);
                oled.resetMultiLineSmallText();
                oled.showMultiLineSmallText(t);
            }
            Serial.print("\r\nPARTS remove prompt n=");
            Serial.println((int)st.parts.numParts);
        } else if (sel < st.parts.numParts) {
            partsShowPartCard(st.parts.parts[sel], -1);
            Serial.print("\r\nPARTS remove sel=");
            Serial.print(st.parts.parts[sel].name);
            Serial.println(" (click = remove)");
        } else {
            if (oled.oledConnected) {
                char t[64];
                snprintf(t, sizeof(t), "All\nclick = clear %d part%s",
                         (int)st.parts.numParts,
                         st.parts.numParts == 1 ? "" : "s");
                oled.resetMultiLineSmallText();
                oled.showMultiLineSmallText(t);
            }
            Serial.print("\r\nPARTS remove sel=ALL n=");
            Serial.println((int)st.parts.numParts);
        }
        Serial.flush();
        paintSel();
    };
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;

    while (st.parts.numParts > 0) {
        if (sel > st.parts.numParts) sel = st.parts.numParts;
        if (needsDraw) {
            drawSel();
            needsDraw = false;
        }
        jOS.serviceInner();
        rotaryEncoderButtonStuff();

        // Core 2's end-of-frame compare-and-swap can swallow a show request
        // issued mid-frame (partsPicker's keepalive) - re-assert so the
        // highlight can't silently go dark mid-scroll.
        if (millis() - lastShowRequest >= 250) {
            requestLedShow(2);
            lastShowRequest = millis();
        }

        if (encoderButtonState == HELD) {
            while (encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
                   encoderButtonState == LONG_HELD) {
                jOS.serviceInner();
                rotaryEncoderButtonStuff();
                delayMicroseconds(1000);
            }
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            break;   // done - the common tail cleans up
        }

        bool clicked =
            (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED);
        if (clicked) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
        }
        int bPress = partsProbeButton();
        if (bPress == 2) break;                       // probe REMOVE = done
        if (bPress == 1 && sel >= 0) clicked = true;  // CONNECT = the click
        if (clicked) {
            if (sel < 0) break;   // nothing selected: the tap-only exit habit
            if (sel >= st.parts.numParts) {
                // All - partsClearAll owns the CONNECT confirm
                if (partsClearAll()) return true;
                needsDraw = true;   // declined: repaint the All stop
                continue;
            }
            // remove the WHOLE highlighted part. Name copied first: the
            // reference dies the moment the table compacts.
            char name[16];
            snprintf(name, sizeof(name), "%s", st.parts.parts[sel].name);
            {
                const PartDefinition& p = st.parts.parts[sel];
                for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                    int node = partPinNode(p, p.pins[j]);
                    if (node >= 1 && node <= 60)
                        b.lightUpNode(node, PARTS_TAP_IGNORED_COLOR);
                }
            }
            requestLedShow(2);
            delay(180);   // the goodbye flash
            if (jl_remove_part(name) == 0) {
                partLabels.requestRun();
                Serial.print("\r\nPARTDB remove ok=");
                Serial.print(name);
                Serial.println(" (part removed)");
                Serial.flush();
                if (oled.oledConnected) {
                    char t[32];
                    snprintf(t, sizeof(t), "removed\n%s", name);
                    oled.clearPrintShow(t, 2, true, true, true);
                    delay(600);
                }
            }
            b.clear();
            requestLedShow(-1);
            // stay in the flow: the next part slides into this slot; past
            // the end, settle on the (new) last real part, not All
            if (sel >= st.parts.numParts) sel = st.parts.numParts - 1;
            needsDraw = true;
            continue;
        }

        if (encoderDirectionState == UP) {
            encoderDirectionState = NONE;
            int stops = st.parts.numParts + 1;   // the parts, then All
            sel = (sel < 0) ? 0 : (sel + 1) % stops;
            needsDraw = true;
        } else if (encoderDirectionState == DOWN) {
            encoderDirectionState = NONE;
            int stops = st.parts.numParts + 1;
            sel = (sel < 0) ? stops - 1 : (sel - 1 + stops) % stops;
            needsDraw = true;
        }

        int row = -1;
        while (Jerial.available() > 0) {
            char c = (char)Jerial.read();
            if (c >= '0' && c <= '9' && nDigits < 2) {
                digits[nDigits++] = c;
                digits[nDigits] = '\0';
            } else if ((c == '\r' || c == '\n') && nDigits > 0) {
                row = atoi(digits);
                nDigits = 0;
                digits[0] = '\0';
                if (row < 1 || row > 60) {
                    Serial.println("  row must be 1-60");
                    row = -1;
                }
            } else if (c != '\r' && c != '\n') {
                // any other serial byte = exit (picker convention). The
                // highlight may be lit - take it down on the way out.
                b.clear();
                requestLedShow(-1);
                return false;
            }
        }

        if (row < 0) {
            // the placement flow's tap discipline: one accepted reading IS
            // the tap; a lift re-arms (partsTapForRow's contract, verbatim)
            int reading = probing.justReadProbe(true);
            if (reading >= 1 && reading <= 60) {
                bool wasArmed = armed;
                lastEmitMs = millis();
                armed = false;
                if (wasArmed) {
                    row = reading;
                } else if (!liftHinted) {
                    liftHinted = true;
                    Serial.println("  (lift the probe, then tap the part)");
                }
            }
            if (!armed) {
                if (lastEmitMs == 0) {
                    if (millis() - openMs >= 80) armed = true;   // opened in the air
                } else if (millis() - lastEmitMs >= 700) {
                    armed = true;                                 // really lifted
                }
            }
        }
        if (row < 0) {
            delayMicroseconds(1000);
            continue;
        }

        int idx = -1, pinIdx = -1;
        for (int i = 0; i < st.parts.numParts && i < MAX_PARTS && idx < 0; i++) {
            const PartDefinition& p = st.parts.parts[i];
            if (!p.placed) continue;
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++)
                if (partPinNode(p, p.pins[j]) == row) { idx = i; pinIdx = j; break; }
        }
        if (idx < 0) {
            b.lightUpNode(row, PARTS_TAP_IGNORED_COLOR);
            requestLedShow(2);
            Serial.print("  no part on row ");
            Serial.println(row);
            continue;
        }

        char name[16], pinName[12];   // PartPin::name is 12 bytes - an
                                      // 8-byte copy truncated "EXTCOMIN"
                                      // and the remove then missed (audit)
        bool lastPin = (st.parts.parts[idx].numPins <= 1);
        // Captured BEFORE the removal compacts the table: a tap that takes
        // a whole part shrinks numParts, and a selection that was parked on
        // a PART must settle back onto a real part afterwards - drifting
        // onto the All stop would arm clear-all without the user ever
        // scrolling there. A selection deliberately ON All stays All.
        bool selWasAll = (sel >= 0 && sel >= st.parts.numParts);
        snprintf(name, sizeof(name), "%s", st.parts.parts[idx].name);
        snprintf(pinName, sizeof(pinName), "%s",
                 st.parts.parts[idx].pins[pinIdx].name);
        if (lastPin) {
            // the whole part flashes goodbye before it goes
            const PartDefinition& p = st.parts.parts[idx];
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                int node = partPinNode(p, p.pins[j]);
                if (node >= 1 && node <= 60)
                    b.lightUpNode(node, PARTS_TAP_IGNORED_COLOR);
            }
            requestLedShow(2);
            delay(180);
        } else {
            // just the tapped leg says goodbye
            b.lightUpNode(row, PARTS_TAP_IGNORED_COLOR);
            requestLedShow(2);
            delay(120);
        }
        if (jl_remove_part_pin(name, pinName, row) == 0) {
            partLabels.requestRun();
            int left = -1;
            {
                int i2 = st.parts.findByName(name);
                if (i2 >= 0) left = st.parts.parts[i2].numPins;
            }
            Serial.print("\r\nPARTDB remove ok=");
            Serial.print(name);
            Serial.print(" pin=");
            Serial.print(pinName);
            Serial.print(" row=");
            Serial.print(row);
            if (left > 0) {
                Serial.print(" left=");
                Serial.println(left);
            } else {
                Serial.println(" (part removed)");
            }
            if (oled.oledConnected) {
                char t[48];
                if (left > 0)
                    snprintf(t, sizeof(t), "removed %s %s\n%d leg%s left", name,
                             pinName, left, left == 1 ? "" : "s");
                else
                    snprintf(t, sizeof(t), "removed\n%s", name);
                oled.clearPrintShow(t, 2, true, true, true);
                delay(600);
            }
        }
        b.clear();
        requestLedShow(-1);
        needsDraw = true;   // the loop top clamps sel and repaints
    }
    b.clear();
    requestLedShow(-1);
    if (oled.oledConnected)
        oled.clearPrintShow(st.parts.numParts > 0 ? "done" : "no parts\nplaced",
                            2, true, true, true);
    delay(600);
    return false;
}

// The "Remove Parts" apps[] row (menu Parts > Remove Parts) lands HERE, not
// in partsAppLauncher - the submenu row used to share the picker launcher
// with "Place Part", so selecting Remove opened the class picker instead of
// the remove flow. Same shape as partsTestLauncher: guard, own the render
// mode, run the flow, common tail.
void partsRemoveLauncher(void) {
    if (globalState.parts.numParts <= 0) {
        Serial.println("\r\nPARTS remove: no parts placed");
        if (oled.oledConnected)
            oled.clearPrintShow("no parts\nplaced", 2, true, true, true);
        delay(600);
        return;
    }
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;
    partsRemoveByTap();
    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    partLabels.clearTransients();   // standing overlays retire on app exit
    requestLedShow(-1);
    Serial.println();
    oled.showJogo32h();
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
        static uint8_t classOf[PARTDB_NUM_CLASSES + 2];
        for (int i = 0; i < kNumPartClasses; i++) {
            if (!partsClassHasPlaceable(kPartClasses[i].cls)) continue;
            s_led[nClasses] = kPartClasses[i].led;
            s_title[nClasses] = kPartClasses[i].title;
            s_desc[nClasses] = kPartClasses[i].desc;
            classOf[nClasses] = kPartClasses[i].cls;
            nClasses++;
        }
        // Remove is ALWAYS in the menu (Kevin, 20:53: a menu whose rows
        // come and go is a menu you can't learn) - with no parts placed it
        // says so and bounces back instead of hiding. Clearing everything
        // lives INSIDE Remove now, as the scroll's trailing All stop
        // (Kevin, 09:12) - the separate Clear row said the same thing twice.
        int removeIdx = nClasses;
        s_led[nClasses] = "Rmv";
        s_title[nClasses] = "Remove parts";
        s_desc[nClasses] = "scroll or tap; All clears";
        classOf[nClasses] = 0xFE;
        nClasses++;
        int pick = partsPicker("class", "Parts", nClasses, classIdx);
        if (pick < 0) break;   // hold or serial byte at the top level = exit
        classIdx = pick;
        if (pick == removeIdx) {
            if (globalState.parts.numParts <= 0) {
                if (oled.oledConnected)
                    oled.clearPrintShow("no parts\nplaced", 2, true, true, true);
                Serial.println("\r\nPARTS remove: no parts placed");
                delay(600);
                continue;
            }
            if (partsRemoveByTap()) break;   // All cleared: exit, board is ambient
            if (globalState.parts.numParts <= 0) classIdx = 0;
            continue;                     // back to the class list
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

            // DIP crosses the center line - TWO taps, pin 1 then the
            // opposite corner, because orientation is never assumed
            // (Kevin's ruling, 2026-08-28: pin 1 can sit bottom-left OR
            // top-right). Transistors and 2-lead discretes take ANY-ORDER
            // taps and the electrical identification sorts out which leg is
            // which (Kevin's ask, 2026-08-26). Everything else (SIP
            // modules, pots) taps every signal by name and the taps are the
            // geometry (Kevin's ruling, 2026-08-25).
            const PartDbPinout& po = *partdbPinoutOf(rec);
            int rows[MAX_PART_PINS];
            int nRows = 0;
            uint8_t unverified = 0;
            float measOhms = 0.0f;
            uint8_t testType = 0;      // identify result, cached onto the
            float testValue = 0.0f;    // placed record after commit (the
            float testValue2 = 0.0f;   // part-highlight card's test data)
            int nSignals = po.numPins;
            if (nSignals > MAX_PART_PINS) nSignals = MAX_PART_PINS;
            bool canIdentify =
                (rec.partClass == PARTDB_CLASS_TRANSISTOR && nSignals == 3) ||
                (rec.partClass == PARTDB_CLASS_DISCRETE && nSignals == 2) ||
                (rec.partClass == PARTDB_CLASS_DISCRETE && nSignals == 3 &&
                 rec.subClass == PARTDB_SUB_DISCRETE_POT);
            if (po.footprint == PARTDB_FOOT_DIP) {
                // Tap pin 1's REAL hole (either half), then the opposite
                // corner (pin W+1, the diagonal). The pair fixes the
                // orientation - bottom-left pin 1 = normal, top-right =
                // rotated 180 - and catches a wrong-footprint tap before
                // any record exists. The corner's row is fully determined
                // by pin 1's row and the width, so a mismatch re-prompts
                // from pin 1 with the expected row named.
                int W = (int)po.pinCount / 2;
                int anchor = -1;
                while (true) {
                    int row = partsTapForRow(rec);
                    if (row == -2) goto done;
                    if (row == -1) break;   // back to the part list
                    bool fits = (row > 30) ? ((row - 30) + (W - 1) <= 30)
                                           : (row - (W - 1) >= 1);
                    if (!fits) {
                        Serial.print("\r\nPARTDB place refused reason=\"dip");
                        Serial.print((int)po.pinCount);
                        Serial.print(" doesn't fit with pin 1 at row ");
                        Serial.print(row);
                        Serial.println("\"");
                        if (oled.oledConnected)
                            oled.clearPrintShow("doesn't fit\nthere", 2,
                                                true, true, true);
                        delay(700);
                        continue;   // re-tap pin 1
                    }
                    int expect = (row > 30) ? (row - 30) + (W - 1)
                                            : (row + 30) - (W - 1);
                    char sig[12];
                    snprintf(sig, sizeof(sig), "pin %d", W + 1);
                    int corner = partsTapForRow(rec, sig, 2, 2, &row, 1);
                    if (corner == -2) goto done;
                    if (corner == -1) continue;   // re-tap pin 1
                    if (corner != expect) {
                        Serial.print("\r\nPARTPICK corner mismatch: pin ");
                        Serial.print(W + 1);
                        Serial.print(" of a dip");
                        Serial.print((int)po.pinCount);
                        Serial.print(" with pin 1 at ");
                        Serial.print(row);
                        Serial.print(" sits at row ");
                        Serial.print(expect);
                        Serial.println(" - re-tap pin 1");
                        if (oled.oledConnected) {
                            char t[64];
                            snprintf(t, sizeof(t),
                                     "pin %d should be\nrow %d - retry",
                                     W + 1, expect);
                            oled.clearPrintShow(t, 2, true, true, true);
                        }
                        delay(900);
                        continue;   // re-tap pin 1
                    }
                    anchor = row;
                    break;
                }
                if (anchor < 0) continue;   // backed out to the part list
                rows[nRows++] = anchor;
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
                if (res.status == 0 && res.type != PartType::EMPTY &&
                    res.type != PartType::UNKNOWN) {
                    testType = (uint8_t)res.type;
                    testValue = res.value;
                    testValue2 = res.value2;
                }
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
                // Cache the identify reading onto the placed record (found
                // by its first tapped row - commit may have replaced an
                // older record) for the part-highlight card's test line.
                if (testType != 0) {
                    for (int i2 = 0; i2 < globalState.parts.numParts && i2 < MAX_PARTS; i2++) {
                        PartDefinition& pp = globalState.parts.parts[i2];
                        if (!pp.placed) continue;
                        bool owns = false;
                        for (int j2 = 0; j2 < pp.numPins && j2 < MAX_PART_PINS; j2++)
                            if (partPinNode(pp, pp.pins[j2]) == rows[0]) { owns = true; break; }
                        if (!owns) continue;
                        pp.lastTestType = testType;
                        pp.lastTestValue = testValue;
                        pp.lastTestValue2 = testValue2;
                        break;
                    }
                }
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
    partLabels.clearTransients();   // standing overlays retire on app exit
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

// The 2-lead junction card (diode / LED / zener), the BJT card's idiom:
// type + name, rows, A/K roles, Vf (plus the color guess or Vz), and the
// board painted in the standing A/K colors.
static void partsShowDiodeResult(const PartDefinition& p, const PartResult& res) {
    int rowsS[2] = { (int)res.rows[0], (int)res.rows[1] };
    PinRole rolesS[2] = { res.roles[0], res.roles[1] };
    if (rowsS[1] < rowsS[0]) {
        int tr = rowsS[0]; rowsS[0] = rowsS[1]; rowsS[1] = tr;
        PinRole tp = rolesS[0]; rolesS[0] = rolesS[1]; rolesS[1] = tp;
    }

    if (oled.oledConnected) {
        char rowLine[24], roleLine[24], vfBuf[16], extraBuf[16] = "";
        snprintf(rowLine, sizeof(rowLine), "%-5d%d", rowsS[0], rowsS[1]);
        snprintf(roleLine, sizeof(roleLine), "%-5s%s",
                 pinRoleName(rolesS[0]), pinRoleName(rolesS[1]));
        snprintf(vfBuf, sizeof(vfBuf), "Vf %.2fV", (double)res.value);
        const char* typeStr = "DIODE";
        if (res.type == PartType::LED) {
            typeStr = "LED";
            snprintf(extraBuf, sizeof(extraBuf), "%.10s", partLedColorGuess(res.value));
        } else if (res.type == PartType::ZENER) {
            typeStr = "ZENER";
            snprintf(extraBuf, sizeof(extraBuf), "Vz %.1fV", (double)res.value2);
        }
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
        rows[3].segs[0] = {vfBuf, f, OLED_ALIGN_INHERIT};
        rows[3].segCount = 1;
        if (extraBuf[0] != '\0') {
            rows[3].segs[1] = {extraBuf, f, OLED_ALIGN_RIGHT};
            rows[3].segCount = 2;
        }
        rows[3].align = OLED_ALIGN_LEFT;
        for (int i = 0; i < 4; i++) rows[i].fixedH = 7;
        oled.clearPrintShowRich(rows, 4, 1, true, true, true);
    }

    b.clear();
    for (int i = 0; i < 2; i++) {
        uint32_t c = (rolesS[i] == PinRole::A) ? PARTS_ROLE_A_COLOR
                                               : PARTS_ROLE_K_COLOR;
        int pr = nodeToPrintRow(rowsS[i]);
        if (pr >= 0) b.printRawRow(0b00011111, pr, c, 0xffffff);
    }
    requestLedShow(2);
}

// The display label for one pin: LEDs wear their polarity (Kevin's ruling,
// 2026-08-27: "use K - and A + as labels").
static void partsCardPinLabel(const PartDefinition& p, const PartPin& pin,
                              char* buf, size_t len) {
    const char* suffix = "";
    if (strcmp(p.typeStr, "led") == 0) {
        if (pin.name[0] == 'A' && pin.name[1] == '\0') suffix = "+";
        else if (pin.name[0] == 'K' && pin.name[1] == '\0') suffix = "-";
    }
    snprintf(buf, len, "%s%s", pin.name, suffix);
}

// The part card (Kevin's spec): the part is the important thing, never the
// nodes. Four 5pt rows on the 128x32 panel, the BJT-card idiom:
//   2N3906
//   PNP
//   hFE 457  0.60V          (cached test data, when there is any)
//   E - 17  B - 18  C - 19  ([brackets] mark a focused pin)
void partsAppendPinLabel(int node, char* buf, size_t cap) {
    if (node < 1 || node > 60 || buf == nullptr || cap == 0) return;
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            if (partPinNode(p, p.pins[j]) != node) continue;
            size_t len = strlen(buf);
            if (len >= cap) return;
            snprintf(buf + len, cap - len, " %s %s", p.name, p.pins[j].name);
            return;
        }
    }
}

void partsShowPartCard(const PartDefinition& p, int focusPin) {
    if (!oled.oledConnected) return;
    highlightingNoteExternalOledPaint();

    // type line: the tested identity first, the authored type as fallback,
    // the authored value riding along when it fits ("LED red")
    char typeLine[26] = "";
    switch ((PartType)p.lastTestType) {
        case PartType::BJT_PNP:   snprintf(typeLine, sizeof(typeLine), "PNP"); break;
        case PartType::BJT_NPN:   snprintf(typeLine, sizeof(typeLine), "NPN"); break;
        case PartType::LED:       snprintf(typeLine, sizeof(typeLine), "LED"); break;
        case PartType::DIODE:     snprintf(typeLine, sizeof(typeLine), "diode"); break;
        case PartType::ZENER:     snprintf(typeLine, sizeof(typeLine), "zener"); break;
        case PartType::RESISTOR:  snprintf(typeLine, sizeof(typeLine), "resistor"); break;
        case PartType::POT:       snprintf(typeLine, sizeof(typeLine), "pot"); break;
        case PartType::NFET:      snprintf(typeLine, sizeof(typeLine), "NFET"); break;
        case PartType::PFET:      snprintf(typeLine, sizeof(typeLine), "PFET"); break;
        case PartType::CAPACITOR: snprintf(typeLine, sizeof(typeLine), "capacitor"); break;
        default:
            snprintf(typeLine, sizeof(typeLine), "%s", p.typeStr);
            break;
    }
    if (p.value[0] != '\0' && strlen(typeLine) + strlen(p.value) + 1 < sizeof(typeLine)) {
        size_t tl = strlen(typeLine);
        snprintf(typeLine + tl, sizeof(typeLine) - tl, " %s", p.value);
    }

    char testLine[26] = "";
    PartLabels::partTestSummary(p, testLine, sizeof(testLine));

    // Pin columns (Kevin's spec, 11:37 + 12:53): labels on one line, rows
    // on the next, the focused pin fenced in |bars| that line up between
    // the two lines. Each column's label/number cell pair is built to the
    // SAME character width (monospace font -> same pixels -> the bars
    // align), an UNFOCUSED cell wears spaces where the bars would be so
    // focus never shifts the layout, and the columns are spread by the
    // panel itself (OLED_ALIGN_JUSTIFY works from measured pixel widths -
    // the fixed 21-char form leaked "C 19" off the right edge).
    const int16_t f = 12;  // Andale Mono 5pt - four rows fit 32px
    char cellL[4][12], cellN[4][12];
    int nCols = 0;
    char l1[24] = "", l2[24] = "";
    bool columnForm = (p.numPins >= 1 && p.numPins <= 4);
    if (columnForm) {
        for (int j = 0; j < p.numPins && j < 4; j++) {
            int node = partPinNode(p, p.pins[j]);
            char label[8], num[8];
            partsCardPinLabel(p, p.pins[j], label, sizeof(label));
            snprintf(num, sizeof(num), "%d", node);
            int w = (int)strlen(label);
            if ((int)strlen(num) > w) w = (int)strlen(num);
            if (j == focusPin) {
                snprintf(cellL[j], sizeof(cellL[j]), "|%-*s|", w, label);
                snprintf(cellN[j], sizeof(cellN[j]), "|%-*s|", w, num);
            } else {
                snprintf(cellL[j], sizeof(cellL[j]), " %-*s ", w, label);
                snprintf(cellN[j], sizeof(cellN[j]), " %-*s ", w, num);
            }
            nCols++;
        }
    } else if (focusPin >= 0 && focusPin < p.numPins) {
        char label[8];
        partsCardPinLabel(p, p.pins[focusPin], label, sizeof(label));
        snprintf(l1, sizeof(l1), "|%s|", label);
        snprintf(l2, sizeof(l2), "|%d|", partPinNode(p, p.pins[focusPin]));
    } else {
        int lo = 61, hi = 0;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int node = partPinNode(p, p.pins[j]);
            if (node < 1 || node > 60) continue;
            if (node < lo) lo = node;
            if (node > hi) hi = node;
        }
        snprintf(l1, sizeof(l1), "%d pins", (int)p.numPins);
        snprintf(l2, sizeof(l2), "rows %d-%d", lo, hi);
    }

    char pinCount[12] = "";
    OledTextRow rows[4] = {};
    rows[0].segs[0] = {p.name, f, OLED_ALIGN_INHERIT};
    rows[0].segs[1] = {typeLine, f, OLED_ALIGN_RIGHT};
    rows[0].segCount = 2;
    rows[0].align = OLED_ALIGN_LEFT;
    if (columnForm) {
        for (int j = 0; j < nCols; j++) {
            rows[1].segs[j] = {cellL[j], f, OLED_ALIGN_INHERIT};
            rows[2].segs[j] = {cellN[j], f, OLED_ALIGN_INHERIT};
        }
        rows[1].segCount = (uint8_t)nCols;
        rows[2].segCount = (uint8_t)nCols;
        rows[1].align = OLED_ALIGN_JUSTIFY;
        rows[2].align = OLED_ALIGN_JUSTIFY;
    } else {
        rows[1].segs[0] = {l1, f, OLED_ALIGN_INHERIT};
        rows[1].segCount = 1;
        rows[1].align = OLED_ALIGN_LEFT;
        if (focusPin >= 0 && focusPin < p.numPins) {
            snprintf(pinCount, sizeof(pinCount), "%d pins", (int)p.numPins);
            rows[1].segs[1] = {pinCount, f, OLED_ALIGN_RIGHT};
            rows[1].segCount = 2;
        }
        rows[2].segs[0] = {l2, f, OLED_ALIGN_INHERIT};
        rows[2].segCount = 1;
        rows[2].align = OLED_ALIGN_LEFT;
    }
    rows[3].segs[0] = {testLine, f, OLED_ALIGN_INHERIT};
    rows[3].segCount = 1;
    rows[3].align = OLED_ALIGN_LEFT;
    for (int i = 0; i < 4; i++) rows[i].fixedH = 7;
    oled.clearPrintShowRich(rows, 4, 1, true, true, true);
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
    partScanActivityHook = partsMeterPulse;   // sessions shimmer their rows

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
                snprintf(l1, sizeof(l1), "no response");
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
            // VCC. Real logic needs the phase-2 vector runner; this reports
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
                        Serial.println("  protection diodes conduct - a chip is seated (logic itself needs the vector runner)");
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
            // A generic IC* record: the re-assignment home (5.2 surface b,
            // "this is actually a ___"). The record knows nothing, so probe
            // the chip: rails from the clamp map, Tier-1 fingerprint,
            // Tier-3 vectors, then the same commit path a scan-time pick
            // takes. The generic record is REMOVED on assignment -
            // replace-on-identity keys on partId and can't see it.
            if (p.footprint == 1 && p.partId[0] == '\0' && p.baseRow >= 31 &&
                p.pinCount >= 4 && (p.pinCount % 2) == 0 &&
                p.baseRow + p.pinCount / 2 - 1 <= 60) {
                int w = p.pinCount / 2;
                int base = p.baseRow;
                int ans = partsConfirmOne("identify", p.name,
                                          "powers it briefly");
                if (ans == -1) break;
                if (ans != 1) continue;
                if (oled.oledConnected)
                    oled.clearPrintShow("reading\nrails...", 2, true, true,
                                        true);
                // rails hint: the corner substrate diode answers directly
                // for corner-power DIPs (partsCornerRails - the clamp vote
                // ties on symmetric CMOS clamp meshes); the vote is the
                // fallback for mid-power/NC-corner layouts. Either way it
                // is only a HINT: partsIdentifyChip re-reads the clamp
                // drops and can name the pair on its own, so an unclear
                // map no longer ends the flow.
                ClusterPower cp;
                if (!partsCornerRails(base, base + w - 1, &cp)) {
                    int conf[6] = {base, base + w - 1, base - 30,
                                   base - 30 + w - 1, base + 1, base - 29};
                    if (!partsFindClusterPower(conf, (w >= 3) ? 6 : 4, &cp)) {
                        cp.gndRow = -1;
                        cp.vddRow = -1;
                    }
                }
                partsAutoAborted = false;   // scan-flow flag, not Test's
                if (oled.oledConnected)
                    oled.clearPrintShow("identifying...", 2, true, true,
                                        true);
                ChipIdentify ident;
                partsIdentifyChip(base, w, &cp.gndRow, &cp.vddRow, &ident);
                partsAutoAborted = false;
                if (ident.nPass == 0) {
                    Serial.println("  no candidate's vectors match - it "
                                   "stays generic");
                    ReadingDisplay::show(p.name, p.baseRow, "no match",
                                         "stays generic");
                    if (partsWaitForPress() == -2) break;
                    continue;
                }
                // survivors: exactly one = offer it by name; several = the
                // picker filtered to them (ties stay ties)
                int chosen = -1;
                if (ident.nPass == 1) {
                    for (int i = 0; i < ident.nTried; i++)
                        if (ident.res[i].verdict == 1) chosen = i;
                    const PartDbRecord& rec =
                        partdb_records[ident.res[chosen].recIdx];
                    int a2 = partsConfirmOne("assign", rec.displayName,
                                             "vectors match");
                    if (a2 == -1) break;
                    if (a2 != 1) continue;
                } else {
                    int nOpt = 0;
                    int survivorAt[8];
                    for (int i = 0; i < ident.nTried &&
                                    nOpt < PARTS_LIST_MAX; i++) {
                        if (ident.res[i].verdict != 1) continue;
                        const PartDbRecord& r =
                            partdb_records[ident.res[i].recIdx];
                        s_led[nOpt] = r.ledName;
                        s_title[nOpt] = r.displayName;
                        s_desc[nOpt] = "vectors match";
                        s_rec[nOpt] = ident.res[i].recIdx;
                        survivorAt[nOpt] = i;
                        nOpt++;
                    }
                    int sel2 = partsPicker("chip", "Which?", nOpt, 0);
                    if (sel2 == -2) break;
                    if (sel2 < 0) continue;
                    chosen = survivorAt[sel2];
                }
                const PartDbRecord& rec =
                    partdb_records[ident.res[chosen].recIdx];
                int p1 = partsChipPinRow(base, 2 * w,
                                         ident.res[chosen].rotated != 0, 1);
                char oldName[16];
                strncpy(oldName, p.name, sizeof(oldName) - 1);
                oldName[sizeof(oldName) - 1] = '\0';
                jl_remove_part(oldName);   // p is dead past this line
                if (partsCommitPlacement(rec, &p1, 1)) {
                    Serial.print("\r\nPARTID reassigned ");
                    Serial.print(oldName);
                    Serial.print(" -> ");
                    Serial.println(rec.id);
                } else {
                    Serial.println("\r\nPARTID reassign commit refused - "
                                   "the generic record was removed");
                }
                pick = 0;   // the list just changed under the picker
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
        if (nRows == 3) partsMeterViz3(rows[0], rows[1], rows[2]);
        else partsMeterViz2(rows[0], rows[1]);
        PartResult res = (nRows == 3)
                             ? identifyThreeLead(rows[0], rows[1], rows[2])
                             : identifyTwoLead(rows[0], rows[1]);
        {   // the verdict replaces the meter - or darkness does, so a
            // failed read never leaves a stuck white pair
            bool named = (res.status == 0 && res.type != PartType::EMPTY &&
                          res.type != PartType::UNKNOWN);
            for (int t = 0; t < res.nRows; t++)
                partsPaintRow((int)res.rows[t],
                              named ? partsResultRowColor(res, t) : 0x000000);
            requestLedShow(2);
        }

        // terminal: the same machine grammar as placement
        Serial.print("\r\nPARTID test part=");
        Serial.print(p.name);
        Serial.print(" type=");
        partsTermRgb(partsTypeColor(res.type, res.value));
        Serial.print(partTypeName(res.type));
        partsTermReset();
        Serial.print(" conf=");
        Serial.print(res.confidence, 2);
        if (res.value != 0.0f) {   // %.4g: a 47nF cap is 4.7e-08, not "0.000"
            char v[20];
            snprintf(v, sizeof(v), " value=%.4g", (double)res.value);
            Serial.print(v);
        }
        if (res.value2 != 0.0f) {
            char v[20];
            snprintf(v, sizeof(v), " value2=%.4g", (double)res.value2);
            Serial.print(v);
        }
        if (res.status != 0) { Serial.print(" status="); Serial.print((int)res.status); }
        Serial.println();

        char line1[24] = "";
        char line2[24] = "";
        bool bjtCard = false;
        bool diodeCard = false;
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
        } else if (res.status == -7) {
            // "busy - try again in a moment" was a lie here: waiting never
            // helps. ADC0-3, the DACs, GND and both rails all hang off one
            // crossbar chip, and every breadboard chip has a single direct
            // lane to it - a neighbour row's wire can hold the only way in
            // (bench, 2026-08-28: C12 on rows 12/42, blocked by the 7447's
            // row-40 rail feed). The session already retries with the whole
            // board briefly unwired, so reaching here means the lane is
            // genuinely gone.
            snprintf(line1, sizeof(line1), "no lane");
            snprintf(line2, sizeof(line2), "to those rows");
            Serial.println("  the fabric has no measurement lane to those rows"
                           " - clear a connection near them and try again");
        } else if (res.status == -4) {
            snprintf(line1, sizeof(line1), "powered");
            snprintf(line2, sizeof(line2), "can't test");
            Serial.println("  those rows read POWERED - a live part can't be"
                           " measured; turn the supply off first");
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
                    if (res.nRows == 2) { diodeCard = true; break; }
                    snprintf(line1, sizeof(line1), "LED %.10s", partLedColorGuess(res.value));
                    snprintf(line2, sizeof(line2), "Vf %.2fV", (double)res.value);
                    break;
                case PartType::DIODE:
                    if (res.nRows == 2) { diodeCard = true; break; }
                    snprintf(line1, sizeof(line1), "diode");
                    snprintf(line2, sizeof(line2), "Vf %.2fV", (double)res.value);
                    break;
                case PartType::ZENER:
                    if (res.nRows == 2) { diodeCard = true; break; }
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
                    if (res.value > 0.0f) {
                        char fds[12];
                        formatFarads(res.value, fds, sizeof(fds));
                        snprintf(line1, sizeof(line1), "%s", fds);
                        snprintf(line2, sizeof(line2), "measured");
                    } else {
                        snprintf(line1, sizeof(line1), "capacitor");
                    }
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
            // cache the reading for the part-highlight card's test line
            // (RAM-only, measuredOhms rules)
            if (res.status == 0 && res.type != PartType::EMPTY &&
                res.type != PartType::UNKNOWN) {
                p.lastTestType = (uint8_t)res.type;
                p.lastTestValue = res.value;
                p.lastTestValue2 = res.value2;
            }
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
        } else if (diodeCard) {
            partsShowDiodeResult(p, res);
        } else {
            ReadingDisplay::show(p.name, p.baseRow, line1[0] ? line1 : nullptr,
                                 line2[0] ? line2 : nullptr);
        }
        // the reading stays up until the user says they've read it
        if (partsWaitForPress() == -2) break;
    }

    partScanActivityHook = nullptr;
    s_meterRows[0] = s_meterRows[1] = s_meterRows[2] = -1;
    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    partLabels.clearTransients();   // standing overlays retire on app exit
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

// (partsAutoAborted itself is defined with the forward declarations up
// top - partsTestLauncher resets it after borrowing scan machinery.)
// WHY it stopped rides along: a stray keystroke into the terminal reads
// as an abort (any serial byte = stop), and without the cause a scan that
// quit mid-board looks like a crash (bench, 14:31: a lone 's' ended the
// run right before the module measurement pass).
static const char* partsAutoAbortCause = nullptr;
static bool partsAutoAbortCheck(void) {
    if (partsAutoAborted) return true;
    jOS.serviceInner();
    rotaryEncoderButtonStuff();
    if (encoderButtonState == HELD ||
        (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED)) {
        partsAutoAborted = true;
        partsAutoAbortCause = "the wheel";
    } else if (partsProbeButton() != 0) {
        partsAutoAborted = true;
        partsAutoAbortCause = "the probe";
    } else if (Serial.available() > 0) {
        (void)Serial.read();
        partsAutoAborted = true;
        partsAutoAbortCause = "serial input";
    }
    return partsAutoAborted;
}
static void partsPrintAborted(void) {
    Serial.print("auto scan stopped (");
    Serial.print(partsAutoAbortCause != nullptr ? partsAutoAbortCause : "?");
    Serial.println(")");
}

// Live scan visualization (Kevin's ask: clear the SCAN text, show what the
// scan is DOING on the breadboard). The census cursor sweeps row by row;
// hits stay lit, empties go dark, the pair sweep walks its pairs. Painted
// straight into the LED buffer - inClickMenu=1 keeps the net render off it.
// s_scanVizFlags points at the launcher's census flags for pair-done paints.
static const uint8_t* s_scanVizFlags = nullptr;
static const uint8_t PARTS_HIT_V = 40;   // held-hit brightness (110 -> 70
                                         // -> 40, "still too bright")

// Cursors AND hits wear the row's hue (Kevin, 2026-08-30: "those rows that
// light up on the initial sweep should have some rainbowiness"): the
// census drags a rainbow down the board and every found row HOLDS its own
// hue, brighter than the moving cursor - found-vs-empty reads as bright
// held color vs dark, and the identify's type colors still overwrite when
// a verdict lands.
static uint32_t partsScanVizHue(int row, uint8_t val) {
    hsvColor h;
    h.h = (uint8_t)(((row - 1) * 255) / 60);
    h.s = 220;
    h.v = val;
    return HsvToRaw(h);
}

static void partsScanViz(int row, int state) {
    int pr = nodeToPrintRow(row);
    if (pr < 0) return;
    switch (state) {
        case 0: b.printRawRow(0b00011111, pr, partsScanVizHue(row, 30), 0xffffff); break;
        case 1: b.printRawRow(0b00011111, pr, partsScanVizHue(row, PARTS_HIT_V), 0xffffff); break;
        case 2: b.printRawRow(0b00011111, pr, 0x000000, 0xffffff); break;
        case 3: {   // pair cursor: this row and the next
            b.printRawRow(0b00011111, pr, partsScanVizHue(row, 22), 0xffffff);
            int pr2 = nodeToPrintRow(row + 1);
            if (pr2 >= 0) b.printRawRow(0b00011111, pr2, partsScanVizHue(row + 1, 22), 0xffffff);
            break;
        }
        case 4: {   // pair done: both rows back to what the flags say
            for (int r = row; r <= row + 1; r++) {
                int prr = nodeToPrintRow(r);
                if (prr < 0) continue;
                bool hit = s_scanVizFlags != nullptr && r >= 1 && r <= 60 &&
                           (s_scanVizFlags[r] == 1 || s_scanVizFlags[r] == 5);
                b.printRawRow(0b00011111, prr,
                              hit ? partsScanVizHue(r, PARTS_HIT_V) : 0x000000,
                              0xffffff);
            }
            break;
        }
        default: break;
    }
    requestLedShow(2);
}

// The meter's exit rule: rows a measurement touched go BACK to what the
// census flags say - a hit holds its hue, an empty goes dark - so a probe
// that found nothing leaves nothing behind (bench, 2026-08-30: the chip
// second look's membership candidates one past the 7447, rows 11/41,
// stayed meter-white forever). Verdict paints land AFTER this, so an
// identified part still wears its colors. Pass -1 for unused slots.
static void partsMeterDone(int a, int b2, int c) {
    s_meterRows[0] = s_meterRows[1] = s_meterRows[2] = -1;
    int rr[3] = {a, b2, c};
    for (int k = 0; k < 3; k++) {
        int r = rr[k];
        if (r < 1 || r > 60) continue;
        bool hit = s_scanVizFlags != nullptr &&
                   (s_scanVizFlags[r] == 1 || s_scanVizFlags[r] == 5);
        partsPaintRow(r, hit ? partsScanVizHue(r, PARTS_HIT_V) : 0x000000);
    }
    requestLedShow(2);
}

// Back to the scan's own stage after a picker or identify owned the
// matrix: wipe the glyphs ("Which? 4051" stood through the SECOND chip's
// whole test - Kevin, 22:01) and every verdict paint, then re-light the
// census hits, so the next finding starts from the ambient scan state
// instead of the last winner's name and colors.
static void partsScanStageRepaint(void) {
    b.clear();
    if (s_scanVizFlags != nullptr)
        for (int r = 1; r <= 60; r++)
            if (s_scanVizFlags[r] == 1 || s_scanVizFlags[r] == 5)
                partsPaintRow(r, partsScanVizHue(r, PARTS_HIT_V));
    requestLedShow(2);
}

// One finding, one detail string, in the summary's own idiom: ohms for
// the resistive family, farads for a capacitor, volts for anything whose
// number is a junction drop. Shared by the cross-gap report, the span
// report, the split report and the add? confirm line.
static void partsResultDetail(const PartResult& res, char* out, size_t n) {
    if (out == nullptr || n == 0) return;
    out[0] = '\0';
    if (res.type == PartType::RESISTOR || res.type == PartType::POT) {
        formatOhms(res.value, out, n);
    } else if (res.type == PartType::CAPACITOR) {
        if (res.value > 0.0f) formatFarads(res.value, out, n);
    } else if (res.value != 0.0f) {
        snprintf(out, n, "%.2fV", (double)res.value);
    }
}

// Turn one scan finding into a PLACED part record (Kevin's ask, 12:15: "make
// it so we can actually act on the scanned parts"). The canonical MicroPython
// place path builds the record - offset pins keep the exact measured rows,
// role names become pin names, footprint infers sipN. Records only: signal
// pins route nothing, so the user's wiring is untouched.
extern "C" int jl_place_part(const char* name, int row, const char* pins_json,
                             const char* footprint, const char* type,
                             const char* value,
                             const char* part_id);   // JumperlessMicroPythonAPI.cpp
static bool partsPlaceScanResult(const PartResult& res) {
    if (res.nRows < 2 || res.nRows > 3) return false;
    int idx[3] = {0, 1, 2};
    for (int a2 = 1; a2 < res.nRows; a2++)
        for (int b2 = a2; b2 > 0 && res.rows[idx[b2]] < res.rows[idx[b2 - 1]]; b2--) {
            int t = idx[b2]; idx[b2] = idx[b2 - 1]; idx[b2 - 1] = t;
        }
    int baseRow = (int)res.rows[idx[0]];

    const char* pfx;
    const char* typeStr;
    char value[12] = "";
    switch (res.type) {
        case PartType::RESISTOR: pfx = "R"; typeStr = "resistor";
            formatOhms(res.value, value, sizeof(value)); break;
        case PartType::DIODE:    pfx = "D";   typeStr = "diode"; break;
        case PartType::ZENER:    pfx = "Z";   typeStr = "zener"; break;
        case PartType::LED:      pfx = "LED"; typeStr = "led";
            snprintf(value, sizeof(value), "%.10s", partLedColorGuess(res.value)); break;
        case PartType::BJT_PNP:
        case PartType::BJT_NPN:  pfx = "Q";   typeStr = "bjt"; break;
        case PartType::POT:      pfx = "POT"; typeStr = "pot";
            formatOhms(res.value, value, sizeof(value)); break;
        case PartType::CAPACITOR: pfx = "C";  typeStr = "capacitor";
            if (res.value > 0.0f) formatFarads(res.value, value, sizeof(value));
            break;
        case PartType::NFET:
        case PartType::PFET:     pfx = "M";   typeStr = "fet"; break;
        default: return false;
    }
    char name[16];
    snprintf(name, sizeof(name), "%s%d", pfx, baseRow);
    // Re-placing over an earlier scan of the same rows must not refuse:
    // place_part's name check runs before its replace-on-identity, so the
    // caller replaces first. Only a record that is OUR OWN artifact goes
    // (partId == the deterministic scan name) - a user's hand-named part
    // never matches (audit, 2026-08-27).
    {
        int prev = globalState.parts.findByName(name);
        if (prev >= 0 &&
            strcmp(globalState.parts.parts[prev].partId, name) == 0)
            jl_remove_part(name);
    }

    // Footprint: same-half 2-leads infer sipN fine, but a CROSS-GAP pair
    // (the LED on 21/51) is the axial2 footprint - legs exactly 30 apart.
    // Inference built a sipN as wide as the whole span and the geometry
    // check rightly refused it ("sip31 does not fit the top half", 14:00).
    const char* footprint = "";
    if (res.nRows == 2 && (int)res.rows[idx[1]] - baseRow == 30)
        footprint = "axial2";

    char pins[160];
    int u = snprintf(pins, sizeof(pins), "{");
    for (int k = 0; k < res.nRows; k++) {
        const char* nm = pinRoleName(res.roles[idx[k]]);
        char fallback[2] = { (char)('1' + k), '\0' };
        if (nm == nullptr || nm[0] == '\0' || strcmp(nm, "LEAD") == 0 ||
            strcmp(nm, "NONE") == 0)
            nm = fallback;
        if (footprint[0] != '\0') {
            // axial2: an `offset:` pin may NEVER cross the ravine (the
            // geometry authority's rule, partPinFootprintNode) - the far
            // leg must ride the footprint's own math as `pin: 2` (bench,
            // 14:43: offset 30 was refused and the LED never placed)
            u += snprintf(pins + u, sizeof(pins) - u, "%s\"%s\": {\"pin\": %d}",
                          k ? ", " : "", nm, k + 1);
        } else {
            u += snprintf(pins + u, sizeof(pins) - u, "%s\"%s\": {\"offset\": %d}",
                          k ? ", " : "", nm, (int)res.rows[idx[k]] - baseRow);
        }
        if (u >= (int)sizeof(pins)) return false;
    }
    snprintf(pins + u, sizeof(pins) - u, "}");

    if (jl_place_part(name, baseRow, pins, footprint, typeStr, value, name) != 0)
        return false;
    int pi = globalState.parts.findByName(name);
    if (pi >= 0) {   // the scan's measurement IS this part's cached test
        globalState.parts.parts[pi].lastTestType = (uint8_t)res.type;
        globalState.parts.parts[pi].lastTestValue = res.value;
        globalState.parts.parts[pi].lastTestValue2 = res.value2;
    }
    Serial.print("\r\nadded ");
    Serial.print(name);
    Serial.print(" rows ");
    Serial.print(baseRow);
    Serial.print("-");
    Serial.println((int)res.rows[idx[res.nRows - 1]]);
    return true;
}

// The hidden-graph star test: one extra junction sharing this diode's anode
// means a chip's clamp network, never a discrete diode (a discrete has
// exactly one isolated edge). Edge PRESENCE is the whole question, so this
// reads junction-map triples (anchor + two candidates per ~2s session)
// instead of a full identify per candidate (~3-4s each - the star tests
// were a big slice of the 170s scan). ANY conduction counts as an edge:
// one-way (a clamp), both ways low (a short, or a pull-up network - both
// prove multi-pin structure). Returns the first edge row, or -1.
// Junction-map reading thresholds: v[i][j] = volts at row i (50k pull-up)
// with row j grounded - a forward junction clamps it low, blocked reads
// the pull-up.
static const float kJmFwd = 2.5f;   // below = clamped through a junction
static const float kJmBlk = 2.9f;   // above = blocked (pull-up wins)
static int partsStarEdgeRow(int anchorRow, const int* cands, int nCand) {
    for (int q = 0; q < nCand && !partsAutoAbortCheck(); q += 2) {
        int c1 = cands[q];
        int c2 = (q + 1 < nCand) ? cands[q + 1] : -1;
        if (c2 < 0) {
            // odd tail: a lone candidate still needs a 3rd session row -
            // reuse the previous candidate (its result is already known,
            // its rows are legal, and the map just measures it again)
            c2 = (q > 0) ? cands[q - 1] : -1;
        }
        if (c2 < 0) {
            // single-candidate call: one typed identify is all we can do
            partsMeterViz2(anchorRow, c1);
            PartResult er = (c1 < anchorRow) ? identifyTwoLead(c1, anchorRow)
                                             : identifyTwoLead(anchorRow, c1);
            partsMeterDone(anchorRow, c1, -1);
            if (er.status == 0 &&
                (er.type == PartType::DIODE || er.type == PartType::ZENER ||
                 er.type == PartType::LED || er.type == PartType::SHORT_CIRCUIT))
                return c1;
            return -1;
        }
        partsMeterViz3(anchorRow, c1, c2);
        int rows3[3] = {anchorRow, c1, c2};
        ScanSession s;
        if (partScanBegin(s, rows3, 3) != 0) continue;
        float v[3][3];
        partScanJunctionMap(s, v);
        partScanEnd(s);
        partsMeterDone(anchorRow, c1, c2);
        // candidate index 1 then 2, vs the anchor at index 0
        if (v[1][0] < kJmFwd || v[0][1] < kJmFwd) return c1;
        if (q + 1 < nCand && (v[2][0] < kJmFwd || v[0][2] < kJmFwd)) return c2;
    }
    return -1;
}
static bool partsDiodeIsChipClamp(int anodeRow, const int* cands, int nCand) {
    return partsStarEdgeRow(anodeRow, cands, nCand) > 0;
}

// When a star test proves a common pin, walk the star to its edge: sweep
// the candidate band around the common - both halves, x-pins included -
// and collect every row that junctions to it. This is what turns "a chip?"
// into a named thing. The census can't see isolated LED cathodes (they
// pre-charge through their own junctions) and the pair sweep skips pairs
// between census hits, so a display's segments are only ever discoverable
// THROUGH their common (bench, 19:18: the 7-seg moved one column off the
// x-pins - common on 58, segments unflagged - and the cluster starved at
// two members). Per-member forward drop and orientation ride along: seven
// LED-drop junctions all pointing one way IS a display, and which way
// they point says common-anode or common-cathode.
struct ClusterFan {
    int rows[16];
    float vf[16];      // forward drop at the map's ~50uA (LEDs read 1.1-2.6)
    int8_t dir[16];    // +1 = the common is the ANODE, -1 = cathode, 0 = both
    int n = 0;
};
static void partsClusterFanOut(int anchorRow, ClusterFan* fan,
                               const uint8_t* flags) {
    fan->n = 0;
    int cands[26];
    int nc = 0;
    auto addC = [&](int r) {
        if (nc >= 26 || r < 1 || r > 60 || r == anchorRow) return;
        if (flags[r] == 2 || flags[r] == 4) return;   // wired / refused
        for (int q = 0; q < nc; q++)
            if (cands[q] == r) return;
        cands[nc++] = r;
    };
    int mirror = (anchorRow <= 30) ? anchorRow + 30 : anchorRow - 30;
    for (int d = 1; d <= 6; d++) { addC(anchorRow - d); addC(anchorRow + d); }
    addC(mirror);
    for (int d = 1; d <= 6; d++) { addC(mirror - d); addC(mirror + d); }
    for (int q = 0; q < nc && fan->n < 16; q += 2) {
        if (partsAutoAbortCheck()) return;
        int c1 = cands[q];
        bool hasC2 = (q + 1 < nc);
        int c2 = hasC2 ? cands[q + 1] : cands[q ? q - 1 : 0];
        if (c2 == c1) break;
        partsMeterViz3(anchorRow, c1, c2);
        int rows3[3] = {anchorRow, c1, c2};
        ScanSession s;
        if (partScanBegin(s, rows3, 3) != 0) continue;
        float v[3][3];
        partScanJunctionMap(s, v);
        partScanEnd(s);
        partsMeterDone(anchorRow, c1, c2);
        for (int t = 1; t <= 2 && fan->n < 16; t++) {
            if (t == 2 && !hasC2) break;   // the doubled filler row
            int cand = (t == 1) ? c1 : c2;
            float fwdA = v[0][t];   // anchor pulled up, cand grounded
            float fwdC = v[t][0];   // cand pulled up, anchor grounded
            bool aLow = fwdA < kJmFwd, cLow = fwdC < kJmFwd;
            if (!aLow && !cLow) continue;       // no edge to the common
            fan->rows[fan->n] = cand;
            fan->vf[fan->n] = aLow ? fwdA : fwdC;
            fan->dir[fan->n] = (aLow && !cLow) ? 1 : (!aLow && cLow) ? -1 : 0;
            fan->n++;
        }
    }
}

// A real silicon BJT's Vbe at identify's test current sits in a narrow
// band; the fakes don't. Bench, 14:00: a 7400's pin trios passed the hFE
// test TWICE - "NPN 0.90V" and "NPN 0.44V", both placed as phantom
// transistors - while the real 2N3906 measured 0.59-0.61V every run.
// (A Darlington's ~1.2V lands outside and reads chip-ish too; that is
// the right side to err on for an auto scan.)
static bool partsBjtVbePlausible(const PartResult& r) {
    if (r.type != PartType::BJT_PNP && r.type != PartType::BJT_NPN) return true;
    return r.value >= 0.50f && r.value <= 0.80f;
}

// ---------------------------------------------------------------------------
// Chip clusters: who is power, and does it talk I2C?
// ---------------------------------------------------------------------------
// The star test proves a cluster is multi-pin, but "a chip?" is a shrug. A
// powered chip's protection diodes point ONE way: every pin clamps to GND
// (GND is the common ANODE) and up to VDD (VDD is the common CATHODE), so
// the two supply rows are the ones whose junction ORIENTATION never varies
// while signal pins read anode one way and cathode the other. Bench truth
// on Kevin's SSD1306 module (GND 1, Vdd 2, SCL 3, SDA 4, 2026-08-27):
//   (2,1) K,A   (3,1) K,A   (4,1) K,A     row 1 anode every time  -> GND
//   (3,2) A,K   (4,2) A,K                 row 2 cathode every time-> Vdd
//   (3,4) EMPTY                           two signal pins: nothing
// Orientation needs junction PRESENCE and DIRECTION, nothing more - so this
// reads partScanJunctionMap triples (one session covers three pairs in
// ~2s) instead of full identifyTwoLead classifications (~3-4s EACH pair;
// six of them were most of the "insane long time" on Kevin's 170s scan).
// In the map, v[i][j] = volts at row i (pulled up) with row j grounded:
// a forward junction i->j clamps it LOW, blocked reads the ~3.2V pull-up.
// (ClusterPower's definition rides with the forward declarations up top -
// partsTestLauncher borrows it for the re-assign flow.)
// Corner-power rails FIRST, vote second (2026-08-30, round 2: against a
// floating GND anchor EVERY clamped pin is a perfect one-way cathode, so
// the vote's VDD pick degenerates to conf iteration order - the 15:05
// scan voted vdd=INH, the corners-first retry voted vdd=CH4). Physics
// that cannot tie: the substrate diode between the two corner-convention
// rows (pin N/2's row and pin N's) conducts exactly one way,
// gnd(anode) -> vdd(cathode), for TTL and CMOS alike - bench 15:30:
// part_identify(38,1) DIODE A,K Vf 0.68 on the 4051, (47,11) A,K 0.57 on
// the 74HC393. The 180-rotation swaps the two rows and the diode
// direction answers that too. A bridged/NC/mid-power corner pair reads
// resistive or open -> no verdict -> the caller falls back to the vote
// (7490, 4049/4050, non-DIP clusters).
// One junction-map read of a candidate rail pair: true only when it
// conducts exactly ONE way at the map's junction thresholds - the anode
// is GND, the cathode VDD. Resistive pairs read low both ways, bulk-cap
// or LED-chain pairs read blocked, and both fall through to the caller's
// next idea. The primitive under partsCornerRails AND the SIP power-end
// hunt.
static bool partsPairOneWay(int ra, int rb, int third, ClusterPower* out) {
    if (third == ra || third == rb) return false;
    ScanSession s;
    int rows3[3] = {ra, rb, third};
    if (partScanBegin(s, rows3, 3) != 0) return false;
    float v[3][3];
    partScanJunctionMap(s, v);
    partScanEnd(s);
    bool fwd = v[0][1] < kJmFwd && v[1][0] > kJmBlk;   // ra anode -> rb
    bool rev = v[1][0] < kJmFwd && v[0][1] > kJmBlk;   // rb anode -> ra
    if (fwd == rev) return false;   // open, resistive, or double-clamped
    out->gndRow = fwd ? ra : rb;
    out->vddRow = fwd ? rb : ra;
    out->nSig = 0;
    return true;
}

static bool partsCornerRails(int lo, int hi, ClusterPower* out) {
    int a = hi;        // pin N/2's row when pin 1 sits at lo
    int b = lo - 30;   // pin N's row
    if (a < 31 || a > 60 || b < 1 || b > 30 || lo > hi) return false;
    int third = (hi - 1 > lo) ? hi - 1 : lo;   // fixture wants 3 rows
    if (!partsPairOneWay(a, b, third, out)) return false;
    Serial.print("  corner diode says gnd ");
    Serial.print(out->gndRow);
    Serial.print(" vdd ");
    Serial.println(out->vddRow);
    return true;
}

static bool partsFindClusterPower(const int* rows, int nRows, ClusterPower* out) {
    if (nRows < 3 || nRows > 6) return false;
    Serial.println("  reading the cluster's clamps...");
    Serial.flush();
    int8_t anodeCount[6] = {0}, cathodeCount[6] = {0}, seen[6] = {0};
    // Triples as a RING - {0,1,2}, {2,3,4}, {4,5,0} - so every row lands in
    // two pair-slots and no row lands in more. They used to all be anchored
    // on rows[0], which put that one row in SIX pair-slots against two or
    // three for everything else; pick() below prefers the row with the most
    // `seen`, so whatever the caller happened to list first won the ground
    // vote on evidence nobody else was allowed to gather. Bench, 2026-08-28:
    // an unpowered TTL chip conducts VCC->pin through its internal
    // resistors, so a conf[] list built downward from row 40 handed the 7447
    // "gnd 40" - its VCC pin - and the identify tried nothing. Same session
    // count, evenly spread. (The uncovered signal<->signal pairs read EMPTY
    // and prove nothing either way.)
    int nTriples = (nRows <= 3) ? 1 : (nRows + 1) / 2;
    for (int q = 0; q < nTriples; q++) {
        if (partsAutoAbortCheck()) return false;
        int a0 = (2 * q) % nRows;
        int a1 = (2 * q + 1) % nRows;
        int a2 = (2 * q + 2) % nRows;
        if (a0 == a1 || a1 == a2 || a0 == a2) continue;
        int idx3[3] = {a0, a1, a2};
        partsMeterViz3(rows[a0], rows[a1], rows[a2]);
        int rows3[3] = {rows[a0], rows[a1], rows[a2]};
        ScanSession s;
        if (partScanBegin(s, rows3, 3) != 0) return false;
        float v[3][3];
        partScanJunctionMap(s, v);
        partScanEnd(s);
        partsMeterDone(rows[a0], rows[a1], rows[a2]);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                if (i == j) continue;
                // clean one-way junction only: both-low is resistive and
                // both-high is empty - neither says who is power
                if (v[i][j] < kJmFwd && v[j][i] > kJmBlk) {
                    seen[idx3[i]]++;
                    seen[idx3[j]]++;
                    anodeCount[idx3[i]]++;
                    cathodeCount[idx3[j]]++;
                }
            }
        }
    }
    // GND: anode in every junction it joined, and it joined the most.
    // VDD: cathode in every junction it joined. A row that is both (a
    // 2-row cluster, one lone diode) proves nothing and is refused below.
    int gi = -1, vi = -1;
    auto pick = [&]() -> bool {
        gi = -1;
        vi = -1;
        for (int i = 0; i < nRows; i++) {
            if (seen[i] < 2) continue;
            if (anodeCount[i] == seen[i] && (gi < 0 || seen[i] > seen[gi])) gi = i;
        }
        for (int i = 0; i < nRows; i++) {
            if (i == gi || seen[i] < 2) continue;
            if (cathodeCount[i] == seen[i] && (vi < 0 || seen[i] > seen[vi])) vi = i;
        }
        return gi >= 0 && vi >= 0;
    };
    if (!pick()) {
        // The quick look has a blind spot: a module's bulk capacitance
        // charges slower than the map's 50k pull can settle, so every
        // pair against VDD reads clamped BOTH ways and drops out (bench,
        // 14:43: the SSD1306's supply hid behind its own decoupling and
        // the 0x3C probe never fired). Measure each pair with full identifies -
        // their hard drives charge the caps and the classifier owns
        // settling. Slower, but only clusters the map couldn't read pay,
        // and pick() exits the moment both rails are known.
        Serial.println("  clamps unclear at a glance - measuring each pair...");
        Serial.flush();
        for (int i = 0; i < 6; i++) {
            anodeCount[i] = 0;
            cathodeCount[i] = 0;
            seen[i] = 0;
        }
        bool done = false;
        for (int i = 0; i < nRows && !done; i++) {
            for (int j = i + 1; j < nRows && !done; j++) {
                if (partsAutoAbortCheck()) return false;
                partsMeterViz2(rows[i], rows[j]);
                PartResult r = identifyTwoLead(rows[i], rows[j]);
                partsMeterDone(rows[i], rows[j], -1);
                if (r.status != 0 || r.nRows != 2) continue;
                if (r.type != PartType::DIODE && r.type != PartType::ZENER &&
                    r.type != PartType::LED)
                    continue;
                for (int t = 0; t < 2; t++) {
                    int which = ((int)r.rows[t] == rows[i]) ? i
                                : ((int)r.rows[t] == rows[j]) ? j : -1;
                    if (which < 0) continue;
                    seen[which]++;
                    if (r.roles[t] == PinRole::A) anodeCount[which]++;
                    else if (r.roles[t] == PinRole::K) cathodeCount[which]++;
                }
                done = pick();
            }
        }
        (void)pick();
    }
    if (gi < 0 || vi < 0) return false;
    out->gndRow = rows[gi];
    out->vddRow = rows[vi];
    out->nSig = 0;
    for (int i = 0; i < nRows && out->nSig < 6; i++)
        if (i != gi && i != vi) out->sig[out->nSig++] = rows[i];
    return true;
}

// The addresses worth naming (DESIGN_PART_ID_FOLLOWUP.md 9 wants a
// /partdb/i2c_addr.txt; this is that table's first pages, in rodata -
// widened from REF_COMPONENT_TESTER_RESEARCH.md 6.3's survey).
static const char* partsI2CAddressName(uint8_t addr) {
    switch (addr) {
        case 0x0D: return "QMC5883L compass";
        case 0x18: case 0x19: return "LIS3DH / MCP9808";
        case 0x1D: return "ADXL345";
        case 0x1E: return "HMC5883L / LIS3MDL";
        case 0x23: return "BH1750 light";
        case 0x28: case 0x29: return "VL53L0X / BNO055";
        case 0x38: return "AHT10/20 / FT6206";
        case 0x39: return "APDS-9960 / AS7341";
        case 0x3C: case 0x3D: return "SSD1306/SH1106 display";
        case 0x40: case 0x41: return "INA219 / PCA9685";
        case 0x44: case 0x45: return "SHT3x/SHT4x";
        case 0x48: case 0x49: case 0x4A: case 0x4B:
            return "ADS1115 / LM75 / TMP102";
        case 0x50: case 0x51: case 0x52: case 0x54:
        case 0x55: case 0x56: return "24Cxx EEPROM";
        case 0x53: return "ADXL345 / 24Cxx";
        case 0x57: return "24Cxx / MAX3010x";
        case 0x5A: case 0x5B: return "CCS811 / MLX90614";
        case 0x5C: return "LPS2x / AM2320 / BH1750";
        case 0x5D: return "GT911 touch / MPR121";
        case 0x60: return "MCP4725 / Si5351";
        case 0x61: return "SCD30 / Si5351";
        case 0x68: return "MPU6050 / DS3231";
        case 0x69: return "MPU (AD0 high) / ICM20948";
        case 0x6A: case 0x6B: return "LSM6DSx IMU";
        case 0x70: return "TCA9548A mux / HT16K33 / SHTC3";
        case 0x76: case 0x77: return "BMP/BME280 / BMP3xx";
        default: return nullptr;
    }
}

// A confirmed I2C module, held for the CONNECT prompt: the measured rows
// plus the address are everything a partdb placement needs (Kevin, 15:19:
// "We need to add the parts we find, including the display").
struct I2cModuleFinding {
    bool valid = false;
    uint8_t addr = 0;
    int gnd = -1, vdd = -1, scl = -1, sda = -1;
};

// Power a suspected chip cluster and probe its signal pins whether they speak
// I2C (Kevin, 12:53: "Can we sense I2C data lines"). Bench-proven on the
// SSD1306 module over the crossbar before this landed. Discipline, straight
// from the doc: power pins connected LAST, current-limited first power-up,
// INA watchdog, everything torn back down on every exit.
static bool partsProbeClusterI2C(const ClusterPower& cp, char* out, size_t outLen,
                                 I2cModuleFinding* foundOut) {
    if (cp.nSig < 2) return false;
    // Wire1 is the panel's bus on connection types 0/1/3 - re-pointing it
    // at breadboard rows would take the display down mid-scan. Kevin's r7
    // is type 2 (the panel is on I2C0), which is why this can run at all.
    if (jumperlessConfig.top_oled.enabled &&
        jumperlessConfig.top_oled.connection_type != 2) {
        Serial.println("  (I2C probe skipped - the display owns that bus)");
        return false;
    }
    Serial.print("  powering rows ");
    Serial.print(cp.vddRow);
    Serial.print("/+ ");
    Serial.print(cp.gndRow);
    Serial.println("/- to check if it speaks I2C...");
    Serial.flush();

    bool ppRestore = infraProbePowerWanted();
    infraSetProbePowerEnabled(false);
    float dac0Restore = globalState.power.dac0;
    bool found = false;
    // GND first, then the two bus legs, and the supply LAST (doc 9).
    bool bGnd = addBridgeToState(GND, cp.gndRow);
    refreshConnections(-1, 0, 0);
    setDac0voltage(0.0f, 0, 0, false);
    bool bVdd = addBridgeToState(DAC0, cp.vddRow);
    refreshConnections(-1, 0, 0);
    setDac0voltage(3.3f, 0, 0, false);
    delay(30);
    float draw = INA1.getCurrent_mA() - currentReadingOffset1_mA;
    if (draw > 20.0f) {
        // the supply row was a bad guess (or the part is shorted): a
        // 3.3V hard drive into a clamp is ~65mA through the fabric
        Serial.print("  it draws ");
        Serial.print(draw, 1);
        Serial.println(" mA - backing off, that isn't a supply pin");
    } else {
        // The GPIO service manages the routable pads: without the same
        // gpioState/gpio_function_map bookkeeping initI2C does, it
        // reasserts SIO on pins 22/23 mid-transaction and wedges the bus.
        uint8_t gsSda = gpioState[2], gsScl = gpioState[3];
        gpio_function_t gfSda = gpio_function_map[2], gfScl = gpio_function_map[3];
        gpioState[2] = gpioState[3] = 6;
        gpio_function_map[2] = gpio_function_map[3] = GPIO_FUNC_I2C;
        for (int order = 0; order < 2 && !found; order++) {
            int sdaRow = order ? cp.sig[1] : cp.sig[0];
            int sclRow = order ? cp.sig[0] : cp.sig[1];
            bool bSda = addBridgeToState(RP_GPIO_3, sdaRow);   // pin 22, I2C1 SDA
            bool bScl = addBridgeToState(RP_GPIO_4, sclRow);   // pin 23, I2C1 SCL
            refreshConnections(-1, 0, 0);
            Wire1.end();
            Wire1.setSDA(22);
            Wire1.setSCL(23);
            Wire1.begin();
            Wire1.setClock(100000);
            // Bound every transaction like initI2C does (15ms + bus
            // recovery). The Earle core's default is ~1 SECOND per try -
            // a wedged bus made the 126-address sweep read as a 4-minute
            // freeze on the bench (Kevin, 13:52).
            Wire1.setTimeout(15, true);
            gpio_set_pulls(22, true, false);
            gpio_set_pulls(23, true, false);
            delay(2);
            // A dead line never ACKs: an unpowered or misrouted part's
            // clamps drag SDA/SCL low. Say so and skip the whole sweep.
            if (!gpio_get(22) || !gpio_get(23)) {
                Serial.println("  bus line held low - not I2C (or unpowered)");
            } else {
                // A healthy bus NACKs an empty address in ~0.2ms; only a
                // wedged one runs into the 15ms timeout. Six timeouts in a
                // row means this ordering's bus is not responding sanely -
                // bail instead of grinding all 112 addresses through the
                // timeout (Kevin, 15:19: "the I2C scan still takes a
                // super long time").
                int sour = 0;
                for (int addr = 0x08; addr <= 0x77 && !partsAutoAbortCheck();
                     addr++) {
                    Wire1.beginTransmission((uint8_t)addr);
                    uint8_t rc = Wire1.endTransmission();
                    if (rc == 2 || rc == 3) {
                        sour = 0;   // clean NACK: the bus is alive
                        continue;
                    }
                    if (rc != 0) {
                        if (++sour >= 6) {
                            Serial.println("  bus not responding sanely -"
                                           " skipping this ordering");
                            break;
                        }
                        continue;
                    }
                    const char* known = partsI2CAddressName((uint8_t)addr);
                    snprintf(out, outLen, "I2C 0x%02X on %d/%d%s%s", addr,
                             sdaRow, sclRow, known ? " - " : "",
                             known ? known : "");
                    Serial.print("  it reports: ");
                    Serial.println(out);
                    if (foundOut != nullptr) {
                        foundOut->valid = true;
                        foundOut->addr = (uint8_t)addr;
                        foundOut->gnd = cp.gndRow;
                        foundOut->vdd = cp.vddRow;
                        foundOut->scl = sclRow;
                        foundOut->sda = sdaRow;
                    }
                    found = true;
                    break;
                }
            }
            Wire1.end();
            // Put the bus back the way i2cScan does - but ONLY when the
            // panel actually lives on Wire1. Connection type 2 is I2C0,
            // and its config still carries the default sda/scl 26/27: a
            // bare begin() on those would claim pin 27 for I2C, and pin 27
            // is the probe-power pad (BUF_IN <- GP_8). That is the exact
            // hazard this probe borrows 22/23 to avoid - don't hand it
            // back on the teardown path.
            if (jumperlessConfig.top_oled.enabled &&
                jumperlessConfig.top_oled.connection_type != 2) {
                Wire1.setSDA(jumperlessConfig.top_oled.sda_pin);
                Wire1.setSCL(jumperlessConfig.top_oled.scl_pin);
                Wire1.begin();
            }
            if (bSda) removeBridgeFromState(RP_GPIO_3, sdaRow, false);
            if (bScl) removeBridgeFromState(RP_GPIO_4, sclRow, false);
            if (!found && order == 0)
                Serial.println("  no response that way round - swapping SDA/SCL...");
        }
        // hand the pads back to the GPIO service exactly as they were
        gpioState[2] = gsSda;
        gpioState[3] = gsScl;
        gpio_function_map[2] = gfSda;
        gpio_function_map[3] = gfScl;
    }
    setDac0voltage(0.0f, 0, 0, false);
    if (bVdd) removeBridgeFromState(DAC0, cp.vddRow, false);
    if (bGnd) removeBridgeFromState(GND, cp.gndRow, false);
    setDac0voltage(dac0Restore, 0, 0, false);
    infraSetProbePowerEnabled(ppRestore);
    refreshConnections(-1, 0, 0);
    if (!found)
        Serial.println("  no I2C response - it isn't an I2C part (or needs 5V)");
    return found;
}

// A confirmed I2C module becomes a real partdb placement: the address picks
// the record from the same candidates table the doc's i2c_addr design
// names, the measurement pass's measured rows become the pins, and
// DisplayService takes it from there - the panel comes alive the moment
// power reaches it, exactly as a hand placement does (Kevin, 15:19: "We
// need to add the parts we find, including the display").
static bool partsPlaceI2cModule(const I2cModuleFinding& f) {
    if (!f.valid) return false;
    const PartDbRecord* cands[4] = {nullptr};
    int n = partdbCandidatesForI2cAddr(f.addr, cands, 4);
    if (n < 1 || cands[0] == nullptr) {
        Serial.println("found module has no partdb record - not placed");
        return false;
    }
    const PartDbRecord* rec = cands[0];
    // only the 4-pin GND/VCC/SCL/SDA module shape places automatically -
    // anything richer needs the picker's per-signal taps
    if (partdb_pinouts[rec->pinoutIdx].numPins != 4) return false;
    int base = f.gnd;
    if (f.vdd < base) base = f.vdd;
    if (f.scl < base) base = f.scl;
    if (f.sda < base) base = f.sda;
    char pins[200];
    snprintf(pins, sizeof(pins),
             "{\"GND\": {\"offset\": %d}, \"VCC\": {\"offset\": %d}, "
             "\"SCL\": {\"offset\": %d}, \"SDA\": {\"offset\": %d}}",
             f.gnd - base, f.vdd - base, f.scl - base, f.sda - base);
    // record names allow [A-Za-z0-9_-] only - displayName can carry spaces
    char name[16];
    int u = 0;
    for (const char* c = rec->displayName; *c && u < 15; c++) {
        char ch = *c;
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        name[u++] = ok ? ch : '_';
    }
    name[u] = '\0';
    if (globalState.parts.findByName(name) >= 0) {
        char uniq[16];
        snprintf(uniq, sizeof(uniq), "%.11s_%d", name, base);
        strncpy(name, uniq, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    if (jl_place_part(name, base, pins, "", rec->id, "", rec->id) != 0)
        return false;
    Serial.print("\r\nadded ");
    Serial.print(name);
    Serial.print(" (");
    Serial.print(rec->desc);
    Serial.println(")");
    return true;
}

// Chip membership, probed on the chip itself (Kevin, 2026-08-28: "do more
// passes in different configurations to get more data before identification"
// - the census poke reads TTL inputs and open-collector outputs as EMPTY,
// so his 7447's bottom side censused 4 of 8 pins). An unpowered chip's ESD
// network conducts supply-pin <-> any real pin: drive the GND row above the
// candidate (substrate diode forward) or the candidate above the VDD row
// (high-side clamp forward). partScanServo's reached-0.5mA IS the verdict -
// no new fixture, and signal<->signal pairs (which read EMPTY and prove
// nothing) are never probed.
static bool partsChipMemberProbe(int candRow, int gndRow, int vddRow) {
    for (int pass = 0; pass < 2; pass++) {
        int drv = (pass == 0) ? gndRow : candRow;
        int shn = (pass == 0) ? candRow : vddRow;
        if (drv < 1 || shn < 1 || drv == shn) continue;
        partsMeterViz2(drv, shn);
        int rows2[2] = {drv, shn};
        ScanSession s;
        if (partScanBegin(s, rows2, 2) != 0) continue;
        bool reached = partScanServo(s, 0, 1, 0.5f, 3.3f, nullptr, nullptr);
        partScanEnd(s);
        if (reached) return true;   // a pin: the chip-span paint covers it
    }
    partsMeterDone(candRow, gndRow, vddRow);
    return false;
}

// ---------------------------------------------------------------------------
// Tier-3 vector runner (DESIGN_IC_IDENTIFICATION.md 5.2, bench-decided
// 2026-08-28). Drives a candidate record's truth-table vectors into a found
// chip and reads the outputs: inputs on routable GPIOs (3.3V - TTL Vih is
// 2.0V, 74HC@3V3 is native), outputs one at a time through a scan-ADC leg
// with a GPIO pull-up attached (an open-collector "off" reads H, exactly
// what the datasheet truth tables mean). Power discipline is
// partsProbeClusterI2C's - GND first, current-limited first touch, shunt in
// the feed, teardown on every exit - plus the copper lesson: physically
// wired boards power the chip AROUND anything we build (the bench 7447 ran
// off jumper wires while INA0 read zero), so the VCC pin is read FIRST and
// "already high" means board-powered mode: drive and read against the
// user's own supply, never fight it.
// ---------------------------------------------------------------------------

static bool partsNodeWired(int node) {
    for (int i = 0; i < globalState.connections.numBridges; i++)
        if (globalState.connections.bridges[i][0] == node ||
            globalState.connections.bridges[i][1] == node)
            return true;
    return false;
}

static bool partsBridgeExists(int a, int b) {
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        int n1 = globalState.connections.bridges[i][0];
        int n2 = globalState.connections.bridges[i][1];
        if ((n1 == a && n2 == b) || (n1 == b && n2 == a)) return true;
    }
    return false;
}

// Physical pin -> breadboard row for a bottom-anchored dipN. rotated =
// pin 1 top-right: the U-ordering shifted by HALF the pin count (a cyclic
// shift, not a reversal - bench-verified on the pin-1-on-row-10 7447).
static int partsChipPinRow(int baseRow, int nPins, bool rotated, int pin) {
    int pos = rotated ? ((pin - 1 + nPins / 2) % nPins) + 1 : pin;
    int half = nPins / 2;
    if (pos <= half) return baseRow + (pos - 1);
    return (baseRow - 30) + (nPins - pos);
}

// The record's own supply pins, or false when it doesn't declare both.
static bool partsRecordPowerPins(const PartDbRecord& rec, int* gndPin,
                                 int* vccPin) {
    const PartDbPinout& po = partdb_pinouts[rec.pinoutIdx];
    *gndPin = -1;
    *vccPin = -1;
    for (int i = 0; i < po.numPins; i++) {
        if (po.pins[i].role == PARTDB_ROLE_GND && *gndPin < 0)
            *gndPin = po.pins[i].pinNumber;
        if (po.pins[i].role == PARTDB_ROLE_VCC && *vccPin < 0)
            *vccPin = po.pins[i].pinNumber;
    }
    return *gndPin > 0 && *vccPin > 0;
}

// Which way does this record sit in these rows? The measured rails are the
// truth: the record's GND/VCC pins must land exactly on them, and one of
// the two DIP orientations does - or the candidate is impossible here.
static bool partsChipOrientFromRails(const PartDbRecord& rec, int baseRow,
                                     int nPins, int gndRow, int vddRow,
                                     bool* rotatedOut) {
    const PartDbPinout& po = partdb_pinouts[rec.pinoutIdx];
    if ((int)po.pinCount != nPins) return false;
    int gndPin = -1, vccPin = -1;
    if (!partsRecordPowerPins(rec, &gndPin, &vccPin)) return false;
    for (int rot = 0; rot < 2; rot++) {
        if (partsChipPinRow(baseRow, nPins, rot != 0, gndPin) == gndRow &&
            partsChipPinRow(baseRow, nPins, rot != 0, vccPin) == vddRow) {
            *rotatedOut = (rot != 0);
            return true;
        }
    }
    return false;
}

// Free routable GPIOs, claimRovingGpio's filter (PartMeasure.cpp) - except
// the panel pins are only off-limits when the panel actually rides them:
// connection type 2 puts the OLED on I2C0, so its configured gpio_sda/scl
// are stale numbers, not live wires (the exact gate partsProbeClusterI2C
// runs on; a blanket exclusion cost the 7-input 7447 set its 7th GPIO on
// the bench).
static int partsFreeGpios(int* out, int maxOut) {
    int n = 0;
    for (int gi = 0; gi < 8 && n < maxOut; gi++) {
        int node = gpioDef[gi][1];
        if (globalState.config.gpioPythonOwned[gi]) continue;
        if (globalState.config.gpioPwmEnabled[gi]) continue;
        if (jumperlessConfig.top_oled.enabled &&
            jumperlessConfig.top_oled.connection_type != 2 &&
            (node == jumperlessConfig.top_oled.gpio_sda ||
             node == jumperlessConfig.top_oled.gpio_scl)) continue;
        if (infraOwnsNode(node)) continue;
        if (partsNodeWired(node)) continue;
        out[n++] = gi;
    }
    return n;
}

// Run ONE candidate's vector set at ONE supply choice.
// Returns 1 = every checked step agreed, 0 = a step disagreed (failStepOut
// says which), -1 = refused (resources / wiring / overcurrent) - a refusal
// says nothing about the part.
static int partsRunVectorSet(const PartDbVectorSet& vs, int baseRow,
                             int nPins, bool rotated, int gndRow, int vddRow,
                             bool use5V, int* failStepOut, float* iccOut) {
    if (failStepOut) *failStepOut = -1;
    if (iccOut) *iccOut = -1.0f;
    if (vs.numIn > 9 || vs.numOut > 16) return -1;

    // Resources first - refuse before touching the board. numIn drivers
    // plus one pull-up reader.
    int gpios[10];
    int nFree = partsFreeGpios(gpios, 10);
    if (nFree < vs.numIn + 1) {
        Serial.println("  vectors: not enough free GPIOs");
        return -1;
    }
    for (int i = 0; i < vs.numIn; i++) {
        int row = partsChipPinRow(baseRow, nPins, rotated, vs.inPins[i]);
        if (row < 1 || row > 60 || partsNodeWired(row)) {
            Serial.print("  vectors: input row ");
            Serial.print(row);
            Serial.println(" is wired - not driving into it");
            return -1;
        }
    }
    uint8_t adcMask = 0x0F;
    for (int c = 0; c < 4; c++)
        if (partsNodeWired(ADC0 + c)) adcMask &= (uint8_t)~(1u << c);
    int adcCh = infraAcquireAdc(INFRA_ADC_SCAN, adcMask, false);
    if (adcCh < 0) {
        Serial.println("  vectors: no free measurement lane");
        return -1;
    }

    bool ppRestore = infraProbePowerWanted();
    infraSetProbePowerEnabled(false);
    float dac0Restore = globalState.power.dac0;
    float railRestore = getDacHardwareVoltage(2);
    bool railTouched = false;

    // The copper lesson, BOTH directions: read both rails before touching
    // anything. A hot "GND" row means the hypothesis is wrong (rails
    // swapped is geometrically identical to the flipped orientation, and
    // the bench proved a wrong guess grounds a live copper-fed node) -
    // refuse, never bridge. A hot VDD row means the board already powers
    // the chip: use that supply, don't fight it.
    bool boardPowered = false;
    {
        bool bG = addBridgeToState(ADC0 + adcCh, gndRow);
        refreshConnections(-1, 0, 0);
        delay(3);
        float vg = readAdcVoltage(adcCh, 8);
        if (bG) removeBridgeFromState(ADC0 + adcCh, gndRow, false);
        if (vg > 2.5f) {
            Serial.print("  vectors: the GND row reads ");
            Serial.print(vg, 2);
            Serial.println("V live - wrong rails, not grounding that");
            infraSetProbePowerEnabled(ppRestore);
            refreshConnections(-1, 0, 0);
            infraReleaseAdc(INFRA_ADC_SCAN);
            return -1;
        }
        bool bV = addBridgeToState(ADC0 + adcCh, vddRow);
        refreshConnections(-1, 0, 0);
        delay(3);
        float v = readAdcVoltage(adcCh, 8);
        if (bV) removeBridgeFromState(ADC0 + adcCh, vddRow, false);
        boardPowered = (v > 2.5f);
        if (boardPowered) {
            Serial.print("  vectors: the board already powers it (");
            Serial.print(v, 2);
            Serial.println("V) - using that supply");
        }
    }

    // GND first (doc 9 discipline) - but only if the board doesn't already
    // provide it: a blind add-then-remove STOLE the placed record's own
    // GND bridge on the bench (the 595's teardown unfloated the 7447's
    // ground mid-pass). Only remove what this run created.
    bool bGnd = false;
    if (!partsBridgeExists(GND, gndRow)) {
        bGnd = addBridgeToState(GND, gndRow);
        refreshConnections(-1, 0, 0);
    }

    int verdict = 1;
    bool bFeedP = false, bFeedM = false;
    if (!boardPowered) {
        if (use5V) {
            // TOP_RAIL -> shunt -> VCC pin: the only source stiff enough
            // for bipolar TTL (bench: a 7447 held 4.0-4.4V at the pin).
            bFeedP = addBridgeToState(TOP_RAIL, ISENSE_PLUS);
            bFeedM = addBridgeToState(ISENSE_MINUS, vddRow);
            refreshConnections(-1, 0, 0);
            if (railRestore < 4.4f) {
                setDacByNumber(2, 5.0f, 0, 0, true);
                railTouched = true;
            }
        } else {
            setDac0voltage(0.0f, 0, 0, false);
            bFeedP = addBridgeToState(DAC0, ISENSE_PLUS);
            bFeedM = addBridgeToState(ISENSE_MINUS, vddRow);
            refreshConnections(-1, 0, 0);
            setDac0voltage(3.3f, 0, 0, false);
        }
        delay(25);
        float icc = INA0.getCurrent_mA();
        if (iccOut) *iccOut = icc;
        if (icc > 150.0f) {
            Serial.print("  vectors: it draws ");
            Serial.print(icc, 0);
            Serial.println(" mA - backing off");
            verdict = -1;
        } else {
            partsTermRgb(0xE0A000);
            Serial.print("  vectors: powered at ");
            Serial.print(use5V ? "5V (rail)" : "3.3V");
            Serial.print(", icc ");
            Serial.print(icc, 1);
            Serial.println(" mA");
            partsTermReset();
            // The Tier-2 quiescent signature: a record with an authored
            // icc band that the measured feed falls outside is not this
            // part - the only separator for vec-identical candidates (the
            // LM358/TL072/NE5532 trio). failStep -2 marks the icc refusal.
            // Skipped when the board powers the chip (nothing measured).
            if (vs.iccMin10 != 0 || vs.iccMax10 != 0) {
                float lo = vs.iccMin10 * 0.1f, hi = vs.iccMax10 * 0.1f;
                if (icc < lo || icc > hi) {
                    Serial.print("  vectors: icc outside the ");
                    Serial.print(lo, 1);
                    Serial.print("-");
                    Serial.print(hi, 1);
                    Serial.println(" mA band for this record");
                    verdict = 0;
                    if (failStepOut) *failStepOut = -2;
                }
            }
        }
    }

    // Input drivers + the pull-up reader. The pins are CLAIMED the way the
    // MicroPython wrappers claim them (owned flag + GPIO_FUNC_SIO + memory
    // barrier) - core 2's readGPIO() twiddles unowned pads' pulls and
    // input buffers between scans, and on the bench that unmade every
    // driven level mid-vector (the chip read floating inputs = 1111 =
    // blank). Configs saved for the restoreRovingGpio-idiom teardown;
    // input buffers OFF - the E9 rule.
    int savedDir[10], savedPull[10];
    uint8_t savedFloat[10], savedState[10], savedOwned[10];
    gpio_function_t savedFunc[10];
    int nCfg = 0;
    int pullGi = gpios[vs.numIn];
    int pullNode = gpioDef[pullGi][1];
    if (verdict == 1) {
        for (int i = 0; i <= vs.numIn; i++) {
            int gi = gpios[i];
            savedDir[nCfg] = globalState.config.gpioDirection[gi];
            savedPull[nCfg] = globalState.config.gpioPulls[gi];
            savedFloat[nCfg] = globalState.config.gpioReadFloating[gi];
            savedState[nCfg] = gpioState[gi];
            savedOwned[nCfg] = globalState.config.gpioPythonOwned[gi] ? 1 : 0;
            savedFunc[nCfg] = gpio_function_map[gi];
            nCfg++;
            int pin = gpioDef[gi][0];
            globalState.config.gpioPythonOwned[gi] = true;
            gpio_function_map[gi] = GPIO_FUNC_SIO;
            __dmb();
            globalState.config.gpioReadFloating[gi] = 0;
            gpioReadFloating[gi] = 0;
            if (i < vs.numIn) {
                globalState.config.gpioDirection[gi] = 0;
                gpioState[gi] = 0;
                pinMode(pin, OUTPUT);
                digitalWrite(pin, LOW);
            } else {
                globalState.config.gpioDirection[gi] = 1;
                globalState.config.gpioPulls[gi] = 1;
                gpioState[gi] = 3;
                pinMode(pin, INPUT_PULLUP);
            }
            gpio_set_input_enabled(pin, false);
        }
        bool inputsOk = true;
        for (int i = 0; i < vs.numIn; i++) {
            int row = partsChipPinRow(baseRow, nPins, rotated, vs.inPins[i]);
            if (!addBridgeToState(gpioDef[gpios[i]][1], row)) inputsOk = false;
        }
        refreshConnections(-1, 0, 0);
        if (!inputsOk) {
            Serial.println("  vectors: an input leg was refused");
            verdict = -1;
        }
    }

    // The steps. One moving read fixture: ADC leg + pull-up leg follow the
    // output being read (one refresh per move, not two per read).
    int readRow = -1;
    auto moveRead = [&](int row) {
        if (row == readRow) return;
        if (readRow > 0) {
            removeBridgeFromState(ADC0 + adcCh, readRow, false);
            removeBridgeFromState(pullNode, readRow, false);
        }
        if (row > 0) {
            addBridgeToState(ADC0 + adcCh, row);
            addBridgeToState(pullNode, row);
        }
        refreshConnections(-1, 0, 0);
        readRow = row;
    };
    for (int s = 0; s < vs.numSteps && verdict == 1; s++) {
        if (partsAutoAbortCheck()) { verdict = -1; break; }
        for (int i = 0; i < vs.numIn; i++) {
            int gi = gpios[i];
            int lvl = (vs.inBits[s] >> i) & 1;
            gpioState[gi] = (uint8_t)lvl;   // keep the service's book true
            digitalWrite(gpioDef[gi][0], lvl);
        }
        // the vector, on the chip: warm = driven high, deep blue = low
        for (int i = 0; i < vs.numIn; i++)
            partsPaintRow(partsChipPinRow(baseRow, nPins, rotated, vs.inPins[i]),
                          ((vs.inBits[s] >> i) & 1) ? 0x1E1C00 : 0x000418);
        requestLedShow(2);
        delay(3);
        if (!boardPowered && INA0.getCurrent_mA() > 150.0f) {
            Serial.println("  vectors: overcurrent mid-step - backing off");
            verdict = -1;
            break;
        }
        for (int o = 0; o < vs.numOut && verdict == 1; o++) {
            if (!((vs.outCare[s] >> o) & 1)) continue;
            int row = partsChipPinRow(baseRow, nPins, rotated, vs.outPins[o]);
            moveRead(row);
            partsPaintRow(row, 0x101014);   // the output under the meter
            requestLedShow(2);
            delay(2);
            float v = readAdcVoltage(adcCh, 8);
            int want = (vs.outBits[s] >> o) & 1;
            // L up to 1.4V: an output sinking a physically-wired LED sat
            // at ~1.0V on the bench; the 1.4-2.2 gap still refuses garbage
            int got = (v > 2.2f) ? 1 : (v < 1.4f) ? 0 : -1;
            partsPaintRow(row, (got == want) ? 0x002006 : 0x2A0004);
            requestLedShow(2);
            if (got != want) {
                verdict = 0;
                if (failStepOut) *failStepOut = s;
                Serial.print("  vectors: step ");
                Serial.print(s);
                Serial.print(" pin ");
                Serial.print(vs.outPins[o]);
                Serial.print(" read ");
                Serial.print(v, 2);
                Serial.print("V, expected ");
                Serial.println(want ? "H" : "L");
            }
        }
    }
    moveRead(-1);

    // Teardown, every exit funnels through here.
    for (int i = 0; i < nCfg; i++) {
        int gi = gpios[i];
        int pin = gpioDef[gi][0];
        if (i < vs.numIn) digitalWrite(pin, LOW);
        pinMode(pin, INPUT);
        gpio_set_input_enabled(pin, true);
        globalState.config.gpioDirection[gi] = savedDir[i];
        globalState.config.gpioPulls[gi] = savedPull[i];
        globalState.config.gpioReadFloating[gi] = savedFloat[i];
        gpioReadFloating[gi] = savedFloat[i];
        gpioState[gi] = savedState[i];
        gpio_function_map[gi] = savedFunc[i];
        globalState.config.gpioPythonOwned[gi] = (savedOwned[i] != 0);
        __dmb();
    }
    for (int i = 0; i < vs.numIn && nCfg > 0; i++) {
        int row = partsChipPinRow(baseRow, nPins, rotated, vs.inPins[i]);
        removeBridgeFromState(gpioDef[gpios[i]][1], row, false);
    }
    if (!boardPowered) {
        if (use5V) {
            if (railTouched) setDacByNumber(2, railRestore, 0, 0, true);
        } else {
            setDac0voltage(0.0f, 0, 0, false);
        }
        if (bFeedP) removeBridgeFromState(use5V ? TOP_RAIL : DAC0,
                                          ISENSE_PLUS, false);
        if (bFeedM) removeBridgeFromState(ISENSE_MINUS, vddRow, false);
        if (!use5V) setDac0voltage(dac0Restore, 0, 0, false);
    }
    if (bGnd) removeBridgeFromState(GND, gndRow, false);
    infraSetProbePowerEnabled(ppRestore);
    refreshConnections(-1, 0, 0);
    infraReleaseAdc(INFRA_ADC_SCAN);
    return verdict;
}

int partsVectorIdentify(int baseRow, int width, int gndRow, int vddRow,
                        VectorIdentifyResult* out, int maxOut,
                        const char* fpMeasured, int* triedOut) {
    if (triedOut) *triedOut = 0;
    if (out == nullptr || maxOut < 1) return -1;
    int nPins = 2 * width;
    if (baseRow < 31 || baseRow > 60 || width < 2 || nPins > MAX_PART_PINS ||
        baseRow + width - 1 > 60)
        return -1;
    if (gndRow < 1 || gndRow > 60 || vddRow < 1 || vddRow > 60 ||
        gndRow == vddRow)
        return -1;
    // The measured clamp map picks the supply family ONCE for every
    // either-supply record, halving the power cycles of the two-pass
    // dance (Kevin, 22:01: "speed it up a little"): top clamps on the
    // pins ('B'/'V') = CMOS, 3.3V pass alone; an all-G map = bipolar
    // TTL, the rail pass alone. Mixed or missing map = both passes,
    // exactly as before.
    int fpFamily = 0;   // 0 = unknown, 1 = TTL-ish, 2 = CMOS-ish
    if (fpMeasured != nullptr && fpMeasured[0] != '\0') {
        int nB = 0, nG = 0;
        for (const char* c = fpMeasured; *c != '\0'; c++) {
            if (*c == 'B' || *c == 'V') nB++;
            else if (*c == 'G') nG++;
        }
        if (nB >= 2 && nG <= 1) fpFamily = 2;
        else if (nG >= 2 && nB <= 1) fpFamily = 1;
    }
    // Try EVERY candidate - the result array caps what gets reported, not
    // what gets run. With the 2026-08-30 database (60 vector sets, ~25 per
    // DIP width) the old n<maxOut loop gate meant a part authored past the
    // first 8 records (the 74393, the whole 4000 family) could never be
    // named. Passes are never dropped: when the array is full of failures
    // a pass evicts one.
    int n = 0;
    for (uint16_t i = 0; i < partdb_numRecords; i++) {
        const PartDbRecord& rec = partdb_records[i];
        const PartDbPinout& po = partdb_pinouts[rec.pinoutIdx];
        if (po.footprint != PARTDB_FOOT_DIP) continue;
        if ((int)po.pinCount != nPins) continue;
        const PartDbVectorSet* vs = partdbVectorSetOf(rec);
        if (vs == nullptr) continue;
        bool rotated = false;
        if (!partsChipOrientFromRails(rec, baseRow, nPins, gndRow, vddRow,
                                      &rotated))
            continue;   // its rails don't land on the measured ones
        if (fpMeasured != nullptr && fpMeasured[0] != '\0' &&
            partdbFingerprintOf(rec) != nullptr &&
            partdbFingerprintMismatchOriented(rec, fpMeasured, nullptr) > 3) {
            // Tier-1 gate: an all-G TTL chip never powers up as a CMOS
            // candidate (and vice versa) - don't even feed it
            partsTermDim();
            Serial.print("  vectors: ");
            Serial.print(rec.id);
            Serial.println(" ruled out by the clamp fingerprint");
            partsTermReset();
            continue;
        }
        Serial.print("  vectors: trying ");
        partsTermRgb(0x00C0E0);
        Serial.print(rec.id);
        Serial.println(rotated ? " (rotated 180)" : "");
        partsTermReset();
        int failStep = -1;
        float iccMa = -1.0f;
        // supply passes per the 5.2 decision: TTL-only = the rail; CMOS =
        // 3.3V; family-wide = rail first, 3.3V retry (74HC at 5V can't
        // trust 3.3V GPIO drive - Vih 3.5V)
        int verdict;
        if (vs->supply == PARTDB_VEC_SUPPLY_3V3 ||
            (vs->supply == PARTDB_VEC_SUPPLY_EITHER && fpFamily == 2)) {
            // CMOS-only record, or the measured clamp map already says
            // CMOS: the 3.3V pass alone suffices (74HC is native there;
            // even HCT's TTL thresholds pass a slow static test)
            verdict = partsRunVectorSet(*vs, baseRow, nPins, rotated, gndRow,
                                        vddRow, false, &failStep, &iccMa);
        } else {
            verdict = partsRunVectorSet(*vs, baseRow, nPins, rotated, gndRow,
                                        vddRow, true, &failStep, &iccMa);
            if (verdict == 0 && vs->supply == PARTDB_VEC_SUPPLY_EITHER &&
                fpFamily != 1)
                verdict = partsRunVectorSet(*vs, baseRow, nPins, rotated,
                                            gndRow, vddRow, false, &failStep,
                                            &iccMa);
        }
        if (triedOut) (*triedOut)++;
        int slot = n;
        if (n < maxOut) {
            n++;
        } else if (verdict == 1) {
            slot = -1;  // full: a pass evicts the latest non-pass
            for (int j = maxOut - 1; j >= 0; j--)
                if (out[j].verdict != 1) { slot = j; break; }
        } else {
            slot = -1;  // full of results and this one failed - drop it
        }
        if (slot >= 0) {
            out[slot].recIdx = i;
            out[slot].rotated = rotated ? 1 : 0;
            out[slot].verdict = (int8_t)verdict;
            out[slot].failStep = (int8_t)failStep;
            out[slot].icc10 = (iccMa < 0.0f) ? -1
                                             : (int16_t)(iccMa * 10.0f + 0.5f);
        }
        if (partsAutoAbortCheck()) break;
    }
    return n;
}

// The measured Tier-1 fingerprint as the PartDb.h alphabet string the
// matcher takes (same encoding jl_part_clamp_fingerprint emits). fpOut
// must hold nPins+1. Returns pins probed, <0 = refused outright.
static int partsCollectFingerprint(int baseRow, int width, int gndRow,
                                   int vddRow, char* fpOut) {
    int nPins = 2 * width;
    fpOut[0] = '\0';
    if (nPins > MAX_PART_PINS) return -1;
    int rows[MAX_PART_PINS];
    for (int k = 1; k <= nPins; k++)
        rows[k - 1] = partsChipPinRow(baseRow, nPins, false, k);
    static ClampPin pins[MAX_PART_PINS];
    int probed = partScanClampFingerprint(rows, nPins, gndRow, vddRow, pins,
                                          partsAutoAbortCheck, partsFpViz);
    if (probed < 0) return probed;
    for (int i = 0; i < nPins; i++) {
        const ClampPin& p = pins[i];
        char c;
        if (p.row == gndRow || p.row == vddRow) c = '-';
        else if (!p.probed) c = 'x';
        else if (p.toGnd == PART_CLAMP_RESISTIVE ||
                 p.toVdd == PART_CLAMP_RESISTIVE) c = 'T';
        else if (p.toGnd == PART_CLAMP_JUNCTION &&
                 p.toVdd == PART_CLAMP_JUNCTION) c = 'B';
        else if (p.toGnd == PART_CLAMP_JUNCTION) c = 'G';
        else if (p.toVdd == PART_CLAMP_JUNCTION) c = 'V';
        else c = 'N';
        fpOut[i] = c;
    }
    fpOut[nPins] = '\0';
    return probed;
}

// WHICH row is ground? The junction map's result is a guess, and a wrong
// guess is worse than none - the vector runner would drive GND onto a
// supply pin. Bench, 2026-08-28, the placed 7447 (rows 33-40 / 3-10, VCC on
// 40, GND on 3): the clamp map named "gnd 40 vdd 39", every record was
// ruled impossible, and the identify tried nothing. The fingerprint STRING
// cannot referee it either - gnd=3/vdd=40 and gnd=40/vdd=3 both read
// GGGGGGG-GGGBGGG- on that chip. The DROP can: one substrate diode reads
// 0.67-0.69V at 1mA, while driving the real VCC row above a pin walks a
// junction CHAIN and reads 1.54-1.74V (measured, all 14 signal pins, both
// swapped orderings).
//
// So: collect the rail pairs the partdb's same-footprint records imply -
// each record, each orientation - plus the scan's own guess, and keep the
// pair whose GND side reads like ONE junction on two signal pins. Nothing
// in band means no identify: a generic dipN is honest, a reverse-fed chip
// is not.
static const float kRailVfLo = 0.40f;   // clampProbeDir's junction floor
static const float kRailVfHi = 1.10f;   // above this it is a chain, not a diode

static bool partsResolveChipRails(int baseRow, int width, int* gndIo,
                                  int* vddIo) {
    int nPins = 2 * width;
    if (nPins < 4 || nPins > MAX_PART_PINS) return false;
    int cand[6][2];
    int nCand = 0;
    auto addCand = [&](int g, int v) {
        if (g < 1 || g > 60 || v < 1 || v > 60 || g == v) return;
        for (int i = 0; i < nCand; i++)
            if (cand[i][0] == g && cand[i][1] == v) return;
        if (nCand < 6) { cand[nCand][0] = g; cand[nCand][1] = v; nCand++; }
    };
    addCand(*gndIo, *vddIo);   // the scan's guess goes first, so it wins ties
    for (uint16_t i = 0; i < partdb_numRecords && nCand < 6; i++) {
        const PartDbRecord& rec = partdb_records[i];
        const PartDbPinout& po = partdb_pinouts[rec.pinoutIdx];
        if (po.footprint != PARTDB_FOOT_DIP) continue;
        if ((int)po.pinCount != nPins) continue;
        int gndPin = -1, vccPin = -1;
        if (!partsRecordPowerPins(rec, &gndPin, &vccPin)) continue;
        for (int rot = 0; rot < 2; rot++)
            addCand(partsChipPinRow(baseRow, nPins, rot != 0, gndPin),
                    partsChipPinRow(baseRow, nPins, rot != 0, vccPin));
    }
    if (nCand == 0) return false;

    // two signal pins no candidate claims as a rail - a rail row reads
    // nothing against itself, and one quirky pin should not get a vote
    int probeRows[2];
    int nProbe = 0;
    for (int k = 1; k <= nPins && nProbe < 2; k++) {
        int r = partsChipPinRow(baseRow, nPins, false, k);
        bool isRail = false;
        for (int i = 0; i < nCand && !isRail; i++)
            if (cand[i][0] == r || cand[i][1] == r) isRail = true;
        if (!isRail) probeRows[nProbe++] = r;
    }
    if (nProbe == 0) return false;

    int best = -1, bestScore = 0;
    float bestVf = 99.0f;
    for (int i = 0; i < nCand; i++) {
        if (partsAutoAbortCheck()) break;
        // the candidate, on the board: green = tried as ground, warm = as
        // VDD, white = the two witness pins under the meter
        s_meterRows[0] = (int16_t)cand[i][0];
        s_meterRows[1] = (int16_t)cand[i][1];
        s_meterRows[2] = (int16_t)probeRows[0];
        partsPaintRow(cand[i][0], 0x003008);
        partsPaintRow(cand[i][1], 0x2A0800);
        for (int p = 0; p < nProbe; p++) partsPaintRow(probeRows[p], 0x101014);
        requestLedShow(2);
        ClampPin pins[2];
        if (partScanClampFingerprint(probeRows, nProbe, cand[i][0], cand[i][1],
                                     pins, partsAutoAbortCheck) < 0)
            continue;
        int score = 0;
        float sum = 0.0f;
        for (int p = 0; p < nProbe; p++) {
            if (pins[p].toGnd != PART_CLAMP_JUNCTION) continue;
            if (pins[p].vfGnd < kRailVfLo || pins[p].vfGnd > kRailVfHi) continue;
            score++;
            sum += pins[p].vfGnd;
        }
        Serial.print("  rails? gnd ");
        partsTermRgb(0x00A030);
        Serial.print(cand[i][0]);
        partsTermReset();
        Serial.print(" vdd ");
        partsTermRgb(0xE05000);
        Serial.print(cand[i][1]);
        partsTermReset();
        Serial.print(": ");
        for (int p = 0; p < nProbe; p++) {
            if (p) Serial.print("/");
            Serial.print(pins[p].vfGnd, 2);
        }
        if (score > 0) partsTermGood(); else partsTermBad();
        Serial.println(score > 0 ? " V - one junction" : " V - not one junction");
        partsTermReset();
        // the verdict, painted where it was measured
        partsPaintRow(cand[i][0], (score > 0) ? 0x003008 : 0x200004);
        partsPaintRow(cand[i][1], (score > 0) ? 0x003008 : 0x200004);
        requestLedShow(2);
        if (score == 0) continue;
        float mean = sum / (float)score;
        if (score > bestScore || (score == bestScore && mean < bestVf - 0.05f)) {
            best = i;
            bestScore = score;
            bestVf = mean;
        }
    }
    if (best < 0) return false;
    *gndIo = cand[best][0];
    *vddIo = cand[best][1];
    // the resolved rails stand while the fingerprint and vectors run:
    // ground green, supply warm
    partsPaintRow(*gndIo, 0x00300A);
    partsPaintRow(*vddIo, 0x2A0800);
    requestLedShow(2);
    return true;
}

// The whole identification of one dipN chip: resolve which rows are really
// the supply pins, then the Tier-1 clamp fingerprint (unpowered, ~0.7s/pin)
// and Tier-3 vectors on the candidates that survive it. One serial line
// carries the evidence. gndRow/vddRow are in-out - a scan guess of -1 means
// "no idea", and the resolved pair goes back to the caller either way.
// (ChipIdentify's definition rides with the forward declarations up top.)
static void partsIdentifyChip(int baseRow, int width, int* gndRow, int* vddRow,
                              ChipIdentify* out) {
    out->fp[0] = '\0';
    out->nTried = 0;
    out->nPass = 0;
    if (!partsResolveChipRails(baseRow, width, gndRow, vddRow)) {
        partsTermBad();
        Serial.println("  no row reads like this chip's ground - not"
                       " identifying (it stays a generic IC)");
        partsTermReset();
        return;
    }
    (void)partsCollectFingerprint(baseRow, width, *gndRow, *vddRow, out->fp);
    if (partsAutoAbortCheck()) return;
    int tried = 0;
    int n = partsVectorIdentify(baseRow, width, *gndRow, *vddRow, out->res, 8,
                                out->fp[0] ? out->fp : nullptr, &tried);
    out->nTried = (n > 0) ? n : 0;
    for (int i = 0; i < out->nTried; i++)
        if (out->res[i].verdict == 1) out->nPass++;
    Serial.print("identify gnd=");
    Serial.print(*gndRow);
    Serial.print(" vdd=");
    Serial.print(*vddRow);
    Serial.print(" fp=");
    if (out->fp[0]) {
        // each letter in its clamp color - the same key the pins wore
        // while partsFpViz painted them
        for (const char* c = out->fp; *c; c++) {
            partsTermRgb(partsBrighten(partsBrighten(partsFpColor(*c))));
            Serial.print(*c);
        }
        partsTermReset();
    } else {
        Serial.print("(none)");
    }
    Serial.print(" tried=");    // candidates RUN; the res[] cap only
    Serial.print(tried);        // limits what is reported (passes kept)
    Serial.print(" pass=");
    if (out->nPass > 0) partsTermGood();
    for (int i = 0, k = 0; i < out->nTried; i++) {
        if (out->res[i].verdict != 1) continue;
        if (k++) Serial.print(",");
        Serial.print(partdb_records[out->res[i].recIdx].id);
        if (out->res[i].rotated) Serial.print("(r)");
    }
    if (out->nPass == 0) {
        partsTermDim();
        Serial.print("none");
    }
    partsTermReset();
    Serial.println();
    if (out->nPass > 0) {
        // a named chip takes a bow: two green sweeps over its own pins
        for (int f = 0; f < 2; f++) {
            for (int k = 1; k <= 2 * width; k++)
                partsPaintRow(partsChipPinRow(baseRow, 2 * width, false, k),
                              (f == 0) ? 0x003008 : 0x000000);
            requestLedShow(2);
            delay(120);
        }
        for (int k = 1; k <= 2 * width; k++)
            partsPaintRow(partsChipPinRow(baseRow, 2 * width, false, k),
                          0x003008);
        requestLedShow(2);
    }
}

// An unidentified chip the scan paired across the ravine becomes a generic
// dipN record (Kevin, 2026-08-28: his 7447 censused as two half-spans and
// was offered nothing). Every leg is listed - so labels, the part card and
// Remove-by-tap see the whole package - but nothing connects: we don't know
// what the chip IS, so the wiring stays the user's. Name IC<row>; the
// record is a real part and persists like any hand placement.
static bool partsPlaceFoundChip(int baseRow, int width) {
    int nPins = 2 * width;
    if (nPins < 4 || nPins > MAX_PART_PINS) return false;
    char name[16];
    snprintf(name, sizeof(name), "IC%d", baseRow);
    if (globalState.parts.findByName(name) >= 0)
        snprintf(name, sizeof(name), "IC%d_2", baseRow);
    char fp[8];
    snprintf(fp, sizeof(fp), "dip%d", nPins);
    char pins[512];
    size_t len = 0;
    pins[len++] = '{';
    for (int k = 1; k <= nPins; k++) {
        len += (size_t)snprintf(pins + len, sizeof(pins) - len,
                                "%s\"P%d\": {\"pin\": %d}",
                                k > 1 ? ", " : "", k, k);
        if (len >= sizeof(pins) - 2) return false;
    }
    pins[len++] = '}';
    pins[len] = '\0';
    if (jl_place_part(name, baseRow, pins, fp, "ic", "", "") != 0)
        return false;
    partLabels.requestRun();
    Serial.print("\r\nadded ");
    Serial.print(name);
    Serial.print(" - dip");
    Serial.print(nPins);
    Serial.print(" at rows ");
    Serial.print(baseRow);
    Serial.print("-");
    Serial.print(baseRow + width - 1);
    Serial.print(" / ");
    Serial.print(baseRow - 30);
    Serial.print("-");
    Serial.println(baseRow - 30 + width - 1);
    return true;
}

// ---------------------------------------------------------------------------
// SIP modules - the SPI display path (Kevin, 2026-08-30: "let's allow us to
// add spi displays"). An SPI module never ACKs a bus probe, and its census
// face is tiny: an unpowered ST7789 flags only its power END (bench: rows
// 22/23/24 of a module on 22-28 - SDA/RES/DC/BL read EMPTY to the poke).
// But the rails-and-member-probe physics the DIP second look uses works
// here too: the clamps name the power pair, the member probe walks the
// quiet rows, and the partdb SIP records' own GND/VCC pin positions anchor
// which records CAN sit in those rows - direction falls out of the anchor,
// so no pin-1 tap. Identity is footprint + the user's confirm (SPI has no
// ACK to prove more), exactly the honesty level of the generic-DIP offer.
// ---------------------------------------------------------------------------
struct SipModuleFinding {
    bool valid = false;            // at least one record fits - placeable
    int lo = 0, hi = 0;            // the probed cluster extent
    int gndRow = -1, vddRow = -1;  // the clamp map's power pair
    int nCand = 0;                 // fitting records, closest pin count first
    uint16_t candRec[8];
    int16_t candPin1[8];           // pin 1's ROW under the anchor
    int8_t candDir[8];             // +1 = pins ascend from pin 1, -1 descend
};

static const char* partsPlacedPartOnRow(int row);   // defined further down

// A listed pin's same-side offset from pin 1 (the PartPin law: an explicit
// offset wins, else pinNumber - 1 - SIP legs march in pin order).
static int partsSipPinOffset(const PartDbPin& p) {
    return (p.offset >= 0) ? p.offset : p.pinNumber - 1;
}

// PURE anchor math (mirrored by test/test_sip_anchor/sip_anchor_check.c -
// if you change one, change both). Like partsChipOrientFromRails, the
// measured rails are the truth: the record's own GND/VCC pin offsets
// (og/ov, 0-based) must land EXACTLY on the measured power rows, and at
// most one direction can (both solve only when og == ov, and no record
// has one pin as both rails). The record's whole run must stay on the
// half - halfLo/halfHi INCLUDE the x-pin columns (a SIP leg can sit on
// 29/30, only sessions refuse them) - and cover every probed row [lo,hi].
// A quiet END pin (a backlight behind its own driver never conducts to a
// rail) is covered the same way: the anchored extent reaches past the
// last probed row, and the confirm prompt shows the full extent.
static bool partsSipAnchor(int og, int ov, int gndRow, int vddRow,
                           int pinCount, int lo, int hi, int halfLo,
                           int halfHi, int* pin1Out, int* dirOut) {
    for (int dir = 1; dir >= -1; dir -= 2) {
        int p1 = gndRow - dir * og;
        if (p1 + dir * ov != vddRow) continue;
        int eLo = (dir > 0) ? p1 : p1 - (pinCount - 1);
        int eHi = eLo + pinCount - 1;
        if (eLo < halfLo || eHi > halfHi) continue;
        if (eLo > lo || eHi < hi) continue;
        *pin1Out = p1;
        *dirOut = dir;
        return true;
    }
    return false;
}

// Rails from a 3-row seed - the power END is all the census sees of an
// unpowered SPI display. partsFindClusterPower's seen>=2 gate rightly
// refuses 3-row clusters (each rail joins exactly ONE junction there), so
// this reads the three pairs with full identifies (their hard drives
// charge a module's bulk caps, the map's 50k pull can't) and asks for the
// module signature instead: exactly one row anode-everywhere (GND),
// exactly one cathode-everywhere (VDD), and one clamping BOTH ways (a
// signal pin). A lone diode has no both-row and a transistor doubles a
// rail role - both refuse.
static bool partsModuleRails3(const int* seed, ClusterPower* out) {
    int8_t anodeCount[3] = {0}, cathodeCount[3] = {0}, seen[3] = {0};
    Serial.println("  reading the power end's junctions...");
    Serial.flush();
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (partsAutoAbortCheck()) return false;
            partsMeterViz2(seed[i], seed[j]);
            PartResult r = identifyTwoLead(seed[i], seed[j]);
            partsMeterDone(seed[i], seed[j], -1);
            if (r.status != 0 || r.nRows != 2) continue;
            if (r.type != PartType::DIODE && r.type != PartType::ZENER &&
                r.type != PartType::LED)
                continue;
            for (int t = 0; t < 2; t++) {
                int which = ((int)r.rows[t] == seed[i]) ? i
                            : ((int)r.rows[t] == seed[j]) ? j : -1;
                if (which < 0) continue;
                seen[which]++;
                if (r.roles[t] == PinRole::A) anodeCount[which]++;
                else if (r.roles[t] == PinRole::K) cathodeCount[which]++;
            }
        }
    }
    int gi = -1, vi = -1, bi = -1;
    for (int i = 0; i < 3; i++) {
        if (seen[i] < 1) continue;
        if (anodeCount[i] == seen[i]) gi = (gi < 0) ? i : -2;
        else if (cathodeCount[i] == seen[i]) vi = (vi < 0) ? i : -2;
        else if (anodeCount[i] > 0 && cathodeCount[i] > 0)
            bi = (bi < 0) ? i : -2;
    }
    if (gi < 0 || vi < 0 || bi < 0) return false;
    out->gndRow = seed[gi];
    out->vddRow = seed[vi];
    out->nSig = 1;
    out->sig[0] = seed[bi];
    return true;
}

// Grow a suspected module cluster and name the records that fit. seed =
// the span's flagged rows (>= 3 - the rails need triangulation); cpKnown
// skips the clamp read when the caller already paid for it. Writes the
// findings line and returns true when there was a cluster to report at
// all; out->valid says whether anything is PLACEABLE. ponytail: the
// member-probe walk treats an already-flagged neighbor as a pin without
// re-proving it belongs to THIS part - a module butted hard against
// another cluster can annex a row, and the anchor containment plus the
// confirm prompt (it shows the rows) are the referees.
static bool partsFindSipModule(uint8_t* flags, const int* seed, int nSeed,
                               const ClusterPower* cpKnown,
                               SipModuleFinding* out, char* line,
                               size_t lineLen) {
    out->valid = false;
    out->nCand = 0;
    if (nSeed < 3) return false;
    int lo = seed[0], hi = seed[0];
    for (int i = 1; i < nSeed; i++) {
        if (seed[i] < lo) lo = seed[i];
        if (seed[i] > hi) hi = seed[i];
    }
    ClusterPower cp;
    bool got = (cpKnown != nullptr);
    if (got) cp = *cpKnown;
    if (!got) {
        // The power pair sits at one END of a module and its two pins are
        // adjacent on every SIP record in the DB - and it hides from both
        // voters: a decoupled supply pair reads CAPACITOR to the
        // classifier, and the census often never flags GND at all (bench
        // GMT177 at 21-28: the seed was {22,23,24} - VCC/SCL/SDA - and
        // partsModuleRails3 rightly refused it). So step outward from the
        // seed's edges and put the corner-diode question to each adjacent
        // pair: one-way single junction = gnd(anode) -> vdd(cathode).
        // Bench 22:05: part_identify(21,22) -> DIODE A,K 0.80V named the
        // GMT177's rails on the first pair tried.
        int bandLo = (lo <= 30) ? 1 : 31;
        int bandHi = (lo <= 30) ? 28 : 58;
        int pairA[6] = {lo - 1, lo,     lo - 2, hi,     hi - 1, hi + 1};
        int pairB[6] = {lo,     lo + 1, lo - 1, hi + 1, hi,     hi + 2};
        for (int k = 0; k < 6 && !got; k++) {
            int ra = pairA[k], rb = pairB[k];
            if (ra < bandLo || rb > bandHi) continue;
            if (partsPlacedPartOnRow(ra) != nullptr ||
                partsPlacedPartOnRow(rb) != nullptr)
                continue;
            if (partsAutoAbortCheck()) return false;
            partsMeterViz2(ra, rb);
            got = partsPairOneWay(ra, rb, seed[nSeed / 2], &cp);
            partsMeterDone(ra, rb, -1);
        }
        if (got) {
            Serial.print("  power-end diode says gnd ");
            Serial.print(cp.gndRow);
            Serial.print(" vdd ");
            Serial.println(cp.vddRow);
            if (cp.gndRow < lo || cp.vddRow < lo) lo = (cp.gndRow < cp.vddRow)
                                                           ? cp.gndRow
                                                           : cp.vddRow;
            if (cp.gndRow > hi || cp.vddRow > hi) hi = (cp.gndRow > cp.vddRow)
                                                           ? cp.gndRow
                                                           : cp.vddRow;
        }
    }
    if (!got) {
        got = (nSeed == 3)
                  ? partsModuleRails3(seed, &cp)
                  : partsFindClusterPower(seed, (nSeed > 6) ? 6 : nSeed, &cp);
        if (!got) return false;
        Serial.print("  its clamps say row ");
        Serial.print(cp.gndRow);
        Serial.print(" is ground and row ");
        Serial.print(cp.vddRow);
        Serial.println(" is the supply");
    }
    // the pokeable band; the anchor may still reach onto the x-pin
    // columns just past it (they hold legs, they just refuse sessions)
    int probeLo = (lo <= 30) ? 1 : 31;
    int probeHi = (lo <= 30) ? 28 : 58;
    Serial.println("  a module? probing the quiet rows against its rails...");
    Serial.flush();
    auto probe = [&](int r) {
        if (r < probeLo || r > probeHi) return false;
        if (flags[r] == 1 || flags[r] == 5) return true;   // already a pin
        if (flags[r] != 0) return false;                   // wired / refused
        if (partsPlacedPartOnRow(r) != nullptr) return false;
        if (partsAutoAbortCheck()) return false;
        if (!partsChipMemberProbe(r, cp.gndRow, cp.vddRow)) return false;
        flags[r] = 5;
        partsScanViz(r, 1);
        Serial.print("  row ");
        Serial.print(r);
        Serial.println(" conducts - a pin");
        return true;
    };
    for (int r = lo + 1; r < hi; r++) probe(r);   // interior gaps
    // sip16 is the widest record in the DB - nothing longer can match
    while (hi - lo + 1 < 16 && probe(hi + 1)) hi++;
    while (hi - lo + 1 < 16 && probe(lo - 1)) lo--;
    if (partsAutoAborted) return false;
    out->lo = lo;
    out->hi = hi;
    out->gndRow = cp.gndRow;
    out->vddRow = cp.vddRow;
    int halfLo = (lo <= 30) ? 1 : 31;
    int halfHi = (lo <= 30) ? 30 : 60;
    for (uint16_t i = 0; i < partdb_numRecords && out->nCand < 8; i++) {
        const PartDbRecord& r = partdb_records[i];
        const PartDbPinout& po = partdb_pinouts[r.pinoutIdx];
        if (po.footprint != PARTDB_FOOT_SIP) continue;
        if ((int)po.pinCount < hi - lo + 1 || po.pinCount < 3) continue;
        if (!partdbPlaceableHere(r)) continue;
        int gndPin = -1, vccPin = -1;
        if (!partsRecordPowerPins(r, &gndPin, &vccPin)) continue;
        int og = -1, ov = -1;
        for (int j = 0; j < po.numPins; j++) {
            if (po.pins[j].pinNumber == gndPin)
                og = partsSipPinOffset(po.pins[j]);
            if (po.pins[j].pinNumber == vccPin)
                ov = partsSipPinOffset(po.pins[j]);
        }
        if (og < 0 || ov < 0) continue;
        int p1 = 0, dir = 0;
        if (!partsSipAnchor(og, ov, cp.gndRow, cp.vddRow, po.pinCount, lo,
                            hi, halfLo, halfHi, &p1, &dir))
            continue;
        int eLo = (dir > 0) ? p1 : p1 - (po.pinCount - 1);
        bool clash = false;
        for (int rr = eLo; rr < eLo + po.pinCount && !clash; rr++)
            if (partsPlacedPartOnRow(rr) != nullptr) clash = true;
        if (clash) continue;
        // closest pin count first - insertion keeps the list sorted, so
        // the picker preselects the tightest fit
        int at = out->nCand;
        while (at > 0 &&
               partdb_pinouts[partdb_records[out->candRec[at - 1]].pinoutIdx]
                       .pinCount > po.pinCount) {
            out->candRec[at] = out->candRec[at - 1];
            out->candPin1[at] = out->candPin1[at - 1];
            out->candDir[at] = out->candDir[at - 1];
            at--;
        }
        out->candRec[at] = i;
        out->candPin1[at] = (int16_t)p1;
        out->candDir[at] = (int8_t)dir;
        out->nCand++;
    }
    if (out->nCand > 0) {
        out->valid = true;
        const PartDbRecord& best = partdb_records[out->candRec[0]];
        if (out->nCand == 1)
            snprintf(line, lineLen, "rows %d-%d: %s? (sip module)", lo, hi,
                     best.displayName);
        else
            snprintf(line, lineLen, "rows %d-%d: %s? (+%d more fit)", lo, hi,
                     best.displayName, out->nCand - 1);
    } else {
        snprintf(line, lineLen, "rows %d-%d: a module? (%d pins, no record fits)",
                 lo, hi, hi - lo + 1);
    }
    return true;
}

// The picked record, committed through the per-signal flow: rows[j] = the
// row of LISTED pin j under the anchor. partsCommitPlacement does the rest
// (per-pin offsets, power auto-route, geometry, replace-on-identity), and
// DisplayService picks the driver up from the record - the panel comes
// alive the moment power reaches it, same as a hand placement.
static bool partsPlaceSipModule(const SipModuleFinding& f, int pick) {
    if (pick < 0 || pick >= f.nCand) return false;
    const PartDbRecord& rec = partdb_records[f.candRec[pick]];
    const PartDbPinout& po = partdb_pinouts[rec.pinoutIdx];
    if (po.numPins > MAX_PART_PINS) return false;
    int rows[MAX_PART_PINS];
    for (int j = 0; j < po.numPins; j++)
        rows[j] = (int)f.candPin1[pick] +
                  (int)f.candDir[pick] * partsSipPinOffset(po.pins[j]);
    return partsCommitPlacement(rec, rows, po.numPins);
}

// A display the fan-out named, held for the wire-up prompt (Kevin, 20:17:
// "give the option to wire up the parts when they're detected").
struct DisplayFinding {
    bool valid = false;
    bool commonAnode = false;
    int common = -1;
    int rows[16];
    int n = 0;
};

// Place the found display as a record whose pins CONNECT to free routable
// GPIOs (segments) and the rail or GND (common) - the placement machinery
// builds the bridges, so persistence, teardown and Remove-by-tap all ride
// the parts layer, same as every display. Then a segment chase, one pass
// around the digit: the user SEES it wired, and which GPIO lights which
// segment is no longer a mystery. Common-anode: rail feeds the common and
// a LOW segment GPIO lights it (fabric resistance is the series resistor,
// ~8mA); HIGH parks it 0.5V under the rail - dark. Common-cathode mirrors.
static bool partsWireFoundDisplay(const DisplayFinding& f) {
    if (!f.valid || f.n < 1) return false;
    // free routable GPIOs: nothing in the bridge table touches the node
    // (this also skips the probe-power feed on GP_8 - BUF_IN owns it)
    int freeG[8];
    int nFree = 0;
    for (int g = 0; g < 8 && nFree < 8; g++) {
        int node = RP_GPIO_1 + g;
        bool used = false;
        for (int i = 0; i < globalState.connections.numBridges && !used; i++)
            if (globalState.connections.bridges[i][0] == node ||
                globalState.connections.bridges[i][1] == node)
                used = true;
        if (!used) freeG[nFree++] = g + 1;   // 1-based GPIO index
    }
    if (nFree < 1) {
        Serial.println("no free GPIOs - display not wired");
        return false;
    }
    int nWire = (f.n < nFree) ? f.n : nFree;
    if (nWire < f.n) {
        Serial.print("only ");
        Serial.print(nFree);
        Serial.print(" GPIOs free - wiring ");
        Serial.print(nWire);
        Serial.print(" of ");
        Serial.print(f.n);
        Serial.println(" segments");
    }
    // dip geometry from the measured columns: bottom row r = column r-30,
    // top row t = column t; base anchors the bottom half (the dip rule)
    int minCol = 61, maxCol = 0;
    auto colOf = [](int r) { return (r <= 30) ? r : r - 30; };
    for (int q = 0; q < f.n; q++) {
        int c = colOf(f.rows[q]);
        if (c < minCol) minCol = c;
        if (c > maxCol) maxCol = c;
    }
    int cc = colOf(f.common);
    if (cc < minCol) minCol = cc;
    if (cc > maxCol) maxCol = cc;
    int W = maxCol - minCol + 1;
    if (W < 1 || W > 15) return false;
    int base = minCol + 30;
    auto pinOf = [&](int r) {
        return (r > 30) ? (colOf(r) - minCol + 1)          // bottom: 1..W
                        : (2 * W - (colOf(r) - minCol));   // top: 2W..W+1
    };
    char pins[640];
    int u = snprintf(pins, sizeof(pins), "{\"COM\": {\"pin\": %d, \"connect\": \"%s\"}",
                     pinOf(f.common), f.commonAnode ? "TOP_RAIL" : "GND");
    for (int q = 0; q < f.n; q++) {
        if (q < nWire)
            u += snprintf(pins + u, sizeof(pins) - u,
                          ", \"S%d\": {\"pin\": %d, \"connect\": \"RP_GPIO_%d\"}",
                          q + 1, pinOf(f.rows[q]), freeG[q]);
        else
            u += snprintf(pins + u, sizeof(pins) - u,
                          ", \"S%d\": {\"pin\": %d}", q + 1, pinOf(f.rows[q]));
        if (u >= (int)sizeof(pins)) return false;
    }
    snprintf(pins + u, sizeof(pins) - u, "}");
    // segments OFF before the bridges land: CA parks HIGH, CC parks LOW
    for (int q = 0; q < nWire; q++) {
        jl_gpio_set_dir(freeG[q], 0);
        jl_gpio_set(freeG[q], f.commonAnode ? 1 : 0);
    }
    char fp[8], name[16];
    snprintf(fp, sizeof(fp), "dip%d", 2 * W);
    snprintf(name, sizeof(name), "7SEG%d", base);
    {
        // re-scan of the same display: replace our own artifact (see
        // partsPlaceScanResult's identical guard)
        int prev = globalState.parts.findByName(name);
        if (prev >= 0 &&
            strcmp(globalState.parts.parts[prev].partId, name) == 0)
            jl_remove_part(name);
    }
    const char* typeStr = f.commonAnode ? "led_7seg_ca" : "led_7seg_cc";
    if (jl_place_part(name, base, pins, fp, typeStr, "", name) != 0)
        return false;
    if (f.commonAnode && getDacHardwareVoltage(2) < 2.0f)
        Serial.println("note: the top rail is under 2V - raise it"
                       " to light the segments");
    Serial.print("\r\nwired ");
    Serial.print(name);
    Serial.print(" - ");
    Serial.print(nWire);
    Serial.print(" segments on GPIO, common ");
    Serial.print(f.commonAnode ? "anode to TOP_RAIL" : "cathode to GND");
    Serial.println();
    // the chase: each segment alone briefly, twice around, then dark -
    // proof of wiring, and a live map of which GPIO owns which segment
    for (int pass = 0; pass < 2; pass++) {
        for (int q = 0; q < nWire; q++) {
            jl_gpio_set(freeG[q], f.commonAnode ? 0 : 1);
            uint32_t until = millis() + 120;
            while (millis() < until) jOS.serviceInner();
            jl_gpio_set(freeG[q], f.commonAnode ? 1 : 0);
        }
    }
    return true;
}

// One yes/no per finding (Kevin, 20:17: "ask per part, not all or
// nothing"), shown through partsConfirmYesNo - the shared gesture set, no
// button legend. `detail` (may be "") takes the OLED line the legend used
// to burn: the measured value, the bus pins, the polarity. A non-y/n
// serial byte (-1) ends the whole confirm pass.
static int partsConfirmOne(const char* verb, const char* what,
                           const char* detail, uint32_t rgb) {
    char t[112];
    snprintf(t, sizeof(t), "%s %s?%s%s", verb, what,
             (detail && detail[0]) ? "\n" : "", detail ? detail : "");
    Serial.print("\r\n");
    Serial.print(verb);
    Serial.print("? ");
    if (rgb) partsTermRgb(rgb);   // the finding's own hue
    Serial.print(what);
    if (detail && detail[0]) {
        Serial.print("  ");
        Serial.print(detail);
    }
    if (rgb) partsTermReset();
    Serial.println("  (y/n)");
    Serial.flush();
    return partsConfirmYesNo(t);
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

// Widen [lo,hi] to the rows this placed part claims in the same breadboard
// half as `row`. The census only sees a part's FREE legs (wired legs are
// flag 2 and never hit - bench: the 2N3906 with E and C wired in shows as a
// lone base-row hit), but the placed record knows where the rest of it sits.
static void partsPlacedRowsInHalf(const char* name, int row, int* lo, int* hi) {
    int hLo = (row <= 30) ? 1 : 31, hHi = (row <= 30) ? 30 : 60;
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed || strcmp(p.name, name) != 0) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int r = partPinNode(p, p.pins[j]);
            if (r < hLo || r > hHi) continue;
            if (r < *lo) *lo = r;
            if (r > *hi) *hi = r;
        }
        return;
    }
}

void partsAutoLauncher(void) {
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;
    partsAutoAborted = false;
    partsAutoAbortCause = nullptr;
    partScanActivityHook = partsMeterPulse;   // sessions shimmer their rows

    // No SCAN banner - the board itself shows what the scan is doing
    // (Kevin's ask): the cursor row sweeps, hits stay lit, empties go dark.
    b.clear();
    requestLedShow(2);
    if (oled.oledConnected) {
        oled.resetMultiLineSmallText();
        oled.showMultiLineSmallText("scanning the board...\n(any press stops)");
    }
    Serial.println("\r\nauto scan begin");
    unsigned long scanT0 = millis();

    static uint8_t flags[61];
    static float v0[61], v1[61];
    s_scanVizFlags = flags;

    // The scan owns a CLEAN board (Kevin's ruling, 12:15): every user bridge
    // comes off before the census - no router contention (a full board
    // sprayed "couldn't find a path" through every identify), every lane
    // free, every row scannable - and goes back at adone. Safe by
    // construction: SlotManager is not in the inner set, so no auto-save
    // can run while this app's modal loop holds the board cleared - slot0
    // keeps the pre-scan state throughout, and a crash mid-scan reboots
    // into the full board.
    static int16_t scanLiftA[MAX_BRIDGES], scanLiftB[MAX_BRIDGES];
    static int16_t scanLiftDup[MAX_BRIDGES];
    int scanLiftN = 0;
    // findings the user can ACT on (Kevin's ask): identified discretes
    // collect here and a CONNECT press at the end places them as records
    static PartResult addable[8];
    int nAddable = 0;
    // a confirmed I2C module (the SSD1306 responding at 0x3C) is placeable too
    static I2cModuleFinding i2cModule;
    i2cModule.valid = false;
    // and a fan-out-named display is WIREABLE (segments to GPIOs)
    static DisplayFinding dispFound;
    dispFound.valid = false;
    // an anchored SIP module (the SPI display path) is placeable too.
    // ponytail: ONE per scan - the first cluster that anchors wins, like
    // the two-DIP cap on chipBase[]
    static SipModuleFinding sipFound;
    sipFound.valid = false;
    sipFound.nCand = 0;
    // an unidentified chip is placeable as a generic dipN (Kevin,
    // 2026-08-28: "There's a 7447 IC on the board it's missing" - both of
    // its half-spans were reported and NEITHER was offered): a bottom-half
    // "a chip?" span whose mirror window also holds hits is one DIP
    static int chipBase[2];
    static int chipWidth[2];
    // the cluster rails the second look found, carried to the confirm pass:
    // Tier-1 fingerprinting and the vector runner both anchor on them, and
    // recomputing clamps after the confirm would pay the junction maps twice
    // (-1 = the clamps stayed unclear and the chip has no named rails)
    static int chipGnd[2];
    static int chipVdd[2];
    int nChipF = 0;
    // pairs that conducted ACROSS the center channel (n <-> n+30): their
    // rows live in different halves and can never form one span, so they
    // arrive from the sweep as explicit pairs and get identified as such
    static int16_t gapPairs[2 * 12];
    int nGapPairs = 0;
    // rows a cross-gap finding claimed - the span former skips them so
    // they can't decay into two width-1 "noise" spans, one per half
    static bool crossUsed[61];
    memset(crossUsed, 0, sizeof(crossUsed));
    {
        for (int i = 0; i < globalState.connections.numBridges && i < MAX_BRIDGES; i++) {
            int n1 = globalState.connections.bridges[i][0];
            int n2 = globalState.connections.bridges[i][1];
            if (globalState.isEphemeralConnection(n1, n2)) continue;
            if (infraIsBridge(n1, n2)) continue;
            scanLiftA[scanLiftN] = (int16_t)n1;
            scanLiftB[scanLiftN] = (int16_t)n2;
            scanLiftDup[scanLiftN] = globalState.connections.bridges[i][2];
            scanLiftN++;
        }
        for (int i = 0; i < scanLiftN; i++)
            removeBridgeFromState(scanLiftA[i], scanLiftB[i], false);
        if (scanLiftN > 0) {
            Serial.print("board cleared for the scan (");
            Serial.print(scanLiftN);
            Serial.println(" wires lifted - they go back when it's done)");
            refreshConnections(-1, 0, 0);
        }
    }
    // Park the probe power feed for the WHOLE scan: it is an infra bridge,
    // so the wire lift above deliberately skips it - and the census has no
    // session to park it (the sweep and every identify session do). Bench,
    // 2026-08-28: a scan launched from the menu right after a probe tap
    // (feed = DAC0 at 3.37V, tip resting on the board) censused ALL 56
    // rows charged and read 3.13V on a "discharged" ADC lane; the same
    // scan launched with the probe quiet ran clean. The per-session parks
    // inside become harmless no-ops; adone restores it in the same refresh
    // that puts the wires back.
    bool scanPpRestore = infraProbePowerWanted();
    infraSetProbePowerEnabled(false);
    refreshConnections(-1, 0, 0);

    int found = partScanCensus(flags, v0, v1, partsAutoAbortCheck, partsScanViz);
    if (found == -6) {
        Serial.println("no clean measurement lane - every free ADC"
                       " reads driven (unwire the ADCs, or unpower whatever"
                       " feeds them) - scan aborted");
        if (oled.oledConnected)
            oled.clearPrintShow("no clean ADC lane\nfor scanning", 2, true, true, true);
        delay(1200);
        goto adone;
    }
    if (found < 0) {
        Serial.println("auto scan busy - try again in a moment");
        if (oled.oledConnected)
            oled.clearPrintShow("scan busy\ntry again", 2, true, true, true);
        delay(900);
        goto adone;
    }
    if (partsAutoAborted) {
        partsPrintAborted();
        goto adone;
    }
    {
        // Plausibility: most of the free board "holding charge" is not
        // parts, it is POWER (45 legs unwired at once would be 15 loose
        // transistors). Say so instead of clustering phantoms into chips
        // (bench, 2026-08-27: a fed lane did exactly that).
        int pokeable = 0;
        for (int r = 1; r <= 60; r++)
            if (flags[r] == 0 || flags[r] == 1) pokeable++;
        if (found > 8 && pokeable > 0 && found * 10 >= pokeable * 7) {
            Serial.print("\r\nimplausible: ");
            Serial.print(found);
            Serial.print(" of ");
            Serial.print(pokeable);
            Serial.println(" free rows read charged - that is power, not"
                           " parts (something is feeding the matrix) - fix"
                           " it and rescan");
            if (oled.oledConnected)
                oled.clearPrintShow("board reads powered\nscan can't see parts",
                                    2, true, true, true);
            partsWaitForPress();
            goto adone;
        }
    }
    {
        // second pass: isolated junction parts are invisible to the poke
        // (they pre-charge through their own junctions) - sweep free pairs
        // in the common layouts (adjacent, across the middle) and see
        // what conducts
        if (oled.oledConnected) {
            oled.resetMultiLineSmallText();
            oled.showMultiLineSmallText("scanning pairs...\n(any press stops)");
        }
        int pairHits = partScanPairSweep(flags, partsAutoAbortCheck,
                                         partsScanViz, gapPairs, &nGapPairs, 12);
        if (pairHits > 0) found += pairHits;
        else if (pairHits < 0)
            // no silent caps: without the sweep, isolated junction parts
            // (a lone transistor, a diode) are invisible to this scan
            Serial.println("pair sweep skipped (measure path busy"
                           " or too wired) - lone junction parts won't be seen");
    }
    if (partsAutoAborted) {
        partsPrintAborted();
        goto adone;
    }

    {
        // the census, row by row, for the curious (and for tuning)
        Serial.print("census hits=");
        Serial.println(found);
        for (int r = 1; r <= 60; r++) {
            if (flags[r] != 1 && flags[r] != 5) continue;
            partsTermRgb(partsScanVizHue(r, 160));   // the cursor's own hue
            Serial.print(flags[r] == 1 ? "  row " : "  row+ ");
            Serial.print(r);
            partsTermDim();
            Serial.print(" v0=");
            Serial.print(v0[r], 2);
            Serial.print(" v1=");
            Serial.print(v1[r], 2);
            partsTermReset();
            Serial.println();
        }

        // edge-row findings first: pairs whose far row is an x-pin column
        // (29/30/59/60) came from the sweep's edge stage. Several pairs
        // sharing one edge row are ONE part - a 7-seg display is many LEDs
        // into a common pin (bench, 14:00: Kevin's display's common anode
        // on row 59 fans out to segments in BOTH halves, and its rows read
        // EMPTY pairwise among themselves - only the common tells the story).
        {
            static const int kEdgeRows[4] = {29, 30, 59, 60};
            for (int e = 0; e < 4 && !partsAutoAborted; e++) {
                int E = kEdgeRows[e];
                int members[12];
                int nMembers = 0;
                for (int gp = 0; gp < nGapPairs && nMembers < 12; gp++)
                    if (gapPairs[2 * gp + 1] == E &&
                        gapPairs[2 * gp] >= 1 && gapPairs[2 * gp] <= 60 &&
                        flags[gapPairs[2 * gp]] == 5)
                        members[nMembers++] = gapPairs[2 * gp];
                if (nMembers == 0) continue;
                Serial.print("  checking row ");
                Serial.print(E);
                Serial.print(" (an x-pin column) - ");
                Serial.print(nMembers);
                Serial.println(" rows conduct to it...");
                Serial.flush();
                if (partsAutoAbortCheck()) break;
                // one typed identify names the family; the sweep already
                // proved every member conducts the same way
                partsMeterViz2(members[0], E);
                PartResult er = identifyTwoLead(members[0], E);
                bool ledLike = (er.status == 0 && (er.type == PartType::LED ||
                                                   er.type == PartType::DIODE));
                if (nMembers >= 3) {
                    Serial.print("  rows ");
                    for (int m = 0; m < nMembers; m++) {
                        if (m) Serial.print(",");
                        Serial.print(members[m]);
                    }
                    Serial.print(" all ");
                    Serial.print(ledLike ? "light from" : "conduct to");
                    Serial.print(" row ");
                    Serial.print(E);
                    Serial.println(ledLike ? " - a 7-seg display?" : " - a chip?");
                    for (int m = 0; m < nMembers; m++) {
                        crossUsed[members[m]] = true;
                        int pr = nodeToPrintRow(members[m]);
                        if (pr >= 0)
                            b.printRawRow(0b00011111, pr, PARTS_ROLE_K_COLOR,
                                          0xffffff);
                    }
                    int prE = nodeToPrintRow(E);
                    if (prE >= 0)
                        b.printRawRow(0b00011111, prE, PARTS_ROLE_A_COLOR,
                                      0xffffff);
                    requestLedShow(2);
                    // placement of a multi-pin display is the Parts menu's
                    // job (it knows the footprints); the scan names it only
                } else {
                    // one or two lone pairs: report like a cross-gap find;
                    // same-half pairs are placeable (a lone LED to row 29),
                    // mirror-half spans have no footprint yet - name only
                    for (int m = 0; m < nMembers; m++) {
                        if (m != 0) partsMeterViz2(members[m], E);
                        PartResult mr = (m == 0) ? er
                                                 : identifyTwoLead(members[m], E);
                        partsMeterDone(members[m], E, -1);
                        if (mr.status != 0 || mr.type == PartType::EMPTY ||
                            mr.type == PartType::UNKNOWN)
                            continue;
                        char detail[24] = "";
                        if (mr.value != 0.0f)
                            snprintf(detail, sizeof(detail), "%.2fV",
                                     (double)mr.value);
                        Serial.print("  rows ");
                        Serial.print(members[m]);
                        Serial.print("-");
                        Serial.print(E);
                        Serial.print(": ");
                        Serial.print(partTypeName(mr.type));
                        Serial.print(" ");
                        Serial.println(detail);
                        crossUsed[members[m]] = true;
                        bool sameHalf = (members[m] <= 30) == (E <= 30);
                        if (sameHalf && nAddable < 8 &&
                            mr.type != PartType::SHORT_CIRCUIT)
                            addable[nAddable++] = mr;
                    }
                }
            }
        }

        // cross-gap findings FIRST: a pair conducting across the center
        // channel (the LED plugged 21 -> 51) is one part in two halves.
        // Identified here and claimed via crossUsed, so the span former
        // below can't decay it into two width-1 "noise" spans. A pair
        // whose ends sit against other horizontal evidence stays with the
        // span logic - that shape is a chip's, not a discrete's.
        for (int gp = 0; gp < nGapPairs && !partsAutoAborted; gp++) {
            int ga = gapPairs[2 * gp], gb = gapPairs[2 * gp + 1];
            if (ga < 1 || gb > 60) continue;
            if (flags[ga] != 5 || flags[gb] != 5) continue;
            bool lonely = true;
            for (int d = -1; d <= 1 && lonely; d += 2) {
                int n1 = ga + d, n2 = gb + d;
                if (n1 >= 1 && n1 <= 28 && (flags[n1] == 1 || flags[n1] == 5))
                    lonely = false;
                if (n2 >= 31 && n2 <= 58 && (flags[n2] == 1 || flags[n2] == 5))
                    lonely = false;
            }
            if (!lonely) continue;
            Serial.print("  checking rows ");
            Serial.print(ga);
            Serial.print("-");
            Serial.print(gb);
            Serial.println(" (across the middle)...");
            Serial.flush();
            if (partsAutoAbortCheck()) break;
            partsMeterViz2(ga, gb);
            PartResult gres = identifyTwoLead(ga, gb);
            if (gres.status == 0 && gres.type != PartType::EMPTY &&
                gres.type != PartType::UNKNOWN) {
                char detail[24] = "";
                partsResultDetail(gres, detail, sizeof(detail));
                Serial.print("  rows ");
                Serial.print(ga);
                Serial.print("-");
                Serial.print(gb);
                Serial.print(": ");
                partsTermRgb(partsTypeColor(gres.type, gres.value));
                Serial.print(partTypeName(gres.type));
                Serial.print(" ");
                Serial.print(detail);
                partsTermReset();
                Serial.println();
                crossUsed[ga] = crossUsed[gb] = true;
                if (nAddable < 8 && gres.type != PartType::SHORT_CIRCUIT)
                    addable[nAddable++] = gres;
                for (int t = 0; t < gres.nRows; t++)
                    partsPaintRow((int)gres.rows[t],
                                  partsResultRowColor(gres, t));
                requestLedShow(2);
            } else {
                partsMeterDone(ga, gb, -1);
            }
        }

        // cluster hits into spans per half; a 1-row gap stays inside the
        // span (a PNP base row can hold its charge while E and C slump)
        int spanStart[12], spanEnd[12];
        int nSpans = 0;
        for (int half = 0; half < 2; half++) {
            int lo = half ? 31 : 1, hi = half ? 58 : 28;
            int cur = -1, last = -1;
            const char* curOwner = nullptr;
            for (int r = lo; r <= hi + 1; r++) {
                bool hit = (r <= hi) && (flags[r] == 1 || flags[r] == 5) &&
                           !crossUsed[r];
                const char* owner = hit ? partsPlacedPartOnRow(r) : nullptr;
                // two parts can abut on neighboring rows (bench: the 7400 at
                // 11-16, the 2N3906 at 17-19) - a placed-ownership change is
                // the split (signature kinds mix WITHIN one part: a BJT's
                // base holds while its E and C only pair-conduct)
                bool ownerChanged =
                    hit && cur >= 0 &&
                    (owner != curOwner &&
                     (owner == nullptr || curOwner == nullptr ||
                      strcmp(owner, curOwner) != 0));
                if (ownerChanged) {
                    if (nSpans < 12) {
                        spanStart[nSpans] = cur;
                        spanEnd[nSpans] = last;
                        nSpans++;
                    }
                    cur = -1;
                }
                if (hit) {
                    if (cur < 0) { cur = r; curOwner = owner; }
                    last = r;
                } else if (cur >= 0 && (r > last + 1 || r > hi)) {
                    // gap of 2+ or the end of the half: close the span.
                    // (r > hi is the end - a span whose last hit IS row
                    // hi has r == last + 1 here and the gap test alone
                    // silently dropped it: a part on 26/27/28 scanned
                    // clean and was never reported)
                    if (nSpans < 12) {
                        spanStart[nSpans] = cur;
                        spanEnd[nSpans] = last;
                        nSpans++;
                    }
                    cur = -1;
                }
            }
        }

        Serial.print("spans=");
        Serial.println(nSpans);
        int shown = 0;
        char summary[96] = "";
        size_t sumLen = 0;
        const char* lastOwner = nullptr;   // the dedupe for fragmented
        int lastOwnerLo = -1, lastOwnerHi = -1;  // placed-part spans
        int annexedRow = -1;   // row absorbed by a DIODE->BJT upgrade
        for (int sp = 0; sp < nSpans && !partsAutoAborted; sp++) {
            int a = spanStart[sp], z = spanEnd[sp];
            if (a == annexedRow) a++;   // already reported as the BJT's pin
            if (a > z) continue;
            int width = z - a + 1;
            const char* owner = nullptr;
            for (int r = a; r <= z && owner == nullptr; r++)
                owner = partsPlacedPartOnRow(r);

            char line[64] = "";
            uint32_t lineRgb = 0;     // 0 = a dim line (placed/noise/unclear)
            bool noiseLine = false;   // terminal-only; never eats an OLED slot
            if (owner != nullptr) {
                // already the user's - name the record, over its claimed rows
                // (the census span can be narrower: wired legs never hit)
                int plo = a, phi = z;
                partsPlacedRowsInHalf(owner, a, &plo, &phi);
                // census gaps can fragment one placed part into several
                // spans; the record names them all identically - once is
                // enough (repeats were eating both OLED summary lines)
                if (lastOwner != nullptr && strcmp(owner, lastOwner) == 0 &&
                    plo == lastOwnerLo && phi == lastOwnerHi)
                    continue;
                lastOwner = owner;
                lastOwnerLo = plo;
                lastOwnerHi = phi;
                snprintf(line, sizeof(line), "rows %d-%d: %s (placed)", plo,
                         phi, owner);
            } else if (width == 1) {
                // a lone hit: measure before crying part. A marginal
                // census row, or a chip pin conducting through the sweep's
                // flagged-neighbor allowance, reads EMPTY on a real 2-lead
                // identify (bench: rows 1 and 39, 3/3 scans each, both
                // artifacts) - those say noise, and only on the terminal.
                int loH = (a <= 28) ? 1 : 31, hiH = (a <= 28) ? 28 : 58;
                int nb = (a + 1 <= hiH && flags[a + 1] == 0) ? a + 1
                         : (a - 1 >= loH && flags[a - 1] == 0) ? a - 1 : -1;
                if (partsAutoAbortCheck()) break;
                PartResult r1;
                r1.status = -1;
                if (nb > 0) {
                    Serial.print("  checking row ");
                    Serial.print(a);
                    Serial.println("...");
                    Serial.flush();
                    partsMeterViz2(a, nb);
                    r1 = (nb > a) ? identifyTwoLead(a, nb)
                                  : identifyTwoLead(nb, a);
                    partsMeterDone(a, nb, -1);
                }
                if (r1.status == 0 && r1.type == PartType::EMPTY) {
                    snprintf(line, sizeof(line),
                             "row %d: noise (nothing conducts)", a);
                    noiseLine = true;
                } else {
                    snprintf(line, sizeof(line),
                             "row %d: one leg of something?", a);
                }
            } else if (width <= 3) {
                Serial.print("  checking rows ");
                Serial.print(a);
                Serial.print("-");
                Serial.print(z);
                Serial.println("...");
                Serial.flush();
                for (int r = a; r <= z; r++) partsPaintRow(r, 0x0A0A0C);
                requestLedShow(2);   // the span under investigation
                if (oled.oledConnected) {
                    char t[48];
                    snprintf(t, sizeof(t), "rows %d-%d:\nchecking...", a, z);
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText(t);
                }
                if (partsAutoAbortCheck()) break;
                PartResult res;
                if (width == 3 && flags[a + 1] != 1 && flags[a + 1] != 5) {
                    // the middle row never hit: this span exists because the
                    // sweep's one-apart arrangement flagged (a, a+2) - a
                    // skip-one two-lead layout. Identify the two real legs;
                    // only if that comes up empty run the three-lead
                    // identify (a BJT whose base row held nothing).
                    partsMeterViz2(a, z);
                    res = identifyTwoLead(a, z);
                    if (res.status != 0 || res.type == PartType::EMPTY ||
                        res.type == PartType::UNKNOWN) {
                        partsMeterViz3(a, a + 1, z);
                        res = identifyThreeLead(a, a + 1, z);
                    }
                } else {
                    if (width == 3) partsMeterViz3(a, a + 1, z);
                    else partsMeterViz2(a, z);
                    res = (width == 3) ? identifyThreeLead(a, a + 1, z)
                                       : identifyTwoLead(a, z);
                }
                if (width == 2 && (res.type == PartType::DIODE ||
                                   res.type == PartType::ZENER)) {
                    // two junction-legs might be a transistor missing its
                    // quiet third pin - try a neighbor on either side. A
                    // pair-conducting neighbor (flag 5) comes first even when
                    // a placed record claims it: conduction INTO this span is
                    // electrical evidence, a record is just data (bench: a
                    // stale 74153 claiming row 17 split the 2N3906's emitter
                    // off into the chip's span, and the empty-only fallback
                    // went looking on the wrong side). The hFE test referees
                    // - a chip pin that happens to conduct won't pass it.
                    int half2Lo = (a <= 28) ? 1 : 31;
                    int half2Hi = (a <= 28) ? 28 : 58;
                    int extra = (z + 1 <= half2Hi && flags[z + 1] == 5) ? z + 1
                                : (a - 1 >= half2Lo && flags[a - 1] == 5) ? a - 1
                                : (z + 1 <= half2Hi && flags[z + 1] == 0) ? z + 1
                                : (a - 1 >= half2Lo && flags[a - 1] == 0) ? a - 1
                                                                          : -1;
                    if (extra > 0 && !partsAutoAbortCheck()) {
                        partsMeterViz3(a, z, extra);
                        PartResult res3 = (extra > z) ? identifyThreeLead(a, z, extra)
                                                      : identifyThreeLead(extra, a, z);
                        if (res3.status == 0 &&
                            (res3.type == PartType::BJT_PNP ||
                             res3.type == PartType::BJT_NPN ||
                             res3.type == PartType::NFET ||
                             res3.type == PartType::PFET) &&
                            partsBjtVbePlausible(res3)) {
                            res = res3;
                            if (extra > z) annexedRow = extra;  // the next
                            // span must not report this row a second time
                            z = (extra > z) ? extra : z;
                            a = (extra < a) ? extra : a;
                        }
                    }
                }
                // A 2-row DIODE verdict is only PROVISIONAL: a chip's clamp
                // diode between a pin and its GND pin reads exactly like one
                // (bench, 2026-08-27: a loose 7400's substrate diode scanned
                // as "DIODE 0.65V"). Hidden-graph learning by edge-detecting
                // queries: a discrete diode is one isolated edge; a chip is
                // a STAR - many pins clamp to one common pin. So query
                // neighbor rows (same half +-3, and the DIP counterparts in
                // the other half) against the diode's ANODE; the FIRST extra
                // junction sharing that terminal proves multi-pin structure
                // and the verdict becomes "a chip". (The group-testing
                // refinement - gang candidates into one INA measurement -
                // waits until scans need to be faster.)
                int chipStarRow = -1;
                if (res.status == 0 && res.nRows == 2 &&
                    (res.type == PartType::DIODE || res.type == PartType::ZENER ||
                     res.type == PartType::LED)) {
                    int anodeRow = (res.roles[0] == PinRole::A) ? (int)res.rows[0]
                                                                : (int)res.rows[1];
                    int candList[6];
                    int nCand = 0;
                    auto addCand = [&](int c) {
                        if (nCand >= 6 || c < 1 || c > 60) return;
                        if (c == 29 || c == 30 || c == 59 || c == 60) return;
                        if (c >= a && c <= z) return;
                        if (flags[c] == 2 || flags[c] == 3 || flags[c] == 4) return;
                        for (int q = 0; q < nCand; q++)
                            if (candList[q] == c) return;
                        candList[nCand++] = c;
                    };
                    for (int d = 1; d <= 3; d++) {
                        addCand(a - d);
                        addCand(z + d);
                    }
                    addCand((a <= 28) ? a + 30 : a - 30);   // the DIP's other side
                    addCand((z <= 28) ? z + 30 : z - 30);
                    if (nCand > 0) {
                        Serial.print("  a diode, or a chip's clamp? testing ");
                        Serial.print(nCand);
                        Serial.println(" neighbor rows against its anode...");
                        Serial.flush();
                    }
                    // a second edge on the anode: no discrete diode has one
                    chipStarRow = partsStarEdgeRow(anodeRow, candList, nCand);
                    if (chipStarRow > 0) {
                        int lo = (a < chipStarRow) ? a : chipStarRow;
                        int hi = (z > chipStarRow) ? z : chipStarRow;
                        snprintf(line, sizeof(line),
                                 "rows %d-%d: a chip? (several pins clamp to row %d)",
                                 lo, hi, anodeRow);
                        // Walk the star to its full extent: the flags can't
                        // feed this (a display's segments never flag), so
                        // the common itself is the only reliable map.
                        Serial.print("  mapping everything that shares row ");
                        Serial.print(anodeRow);
                        Serial.println("...");
                        Serial.flush();
                        static ClusterFan fan;
                        partsClusterFanOut(anodeRow, &fan, flags);
                        int ledish = 0, up = 0, down = 0;
                        for (int q = 0; q < fan.n; q++) {
                            if (fan.vf[q] >= 1.1f) ledish++;
                            if (fan.dir[q] > 0) up++;
                            else if (fan.dir[q] < 0) down++;
                        }
                        if (fan.n >= 4 && ledish >= fan.n - 1 &&
                            (up == 0 || down == 0)) {
                            // many LED-drop junctions, one common terminal,
                            // every one pointing the same way: a display
                            Serial.print("  rows ");
                            for (int q = 0; q < fan.n; q++) {
                                if (q) Serial.print(",");
                                Serial.print(fan.rows[q]);
                            }
                            Serial.print(" all light from row ");
                            Serial.print(anodeRow);
                            Serial.print(" - a 7-seg display? (common ");
                            Serial.print(up > 0 ? "anode" : "cathode");
                            Serial.println(")");
                            snprintf(line, sizeof(line),
                                     "LED display? (%d segments, common %s %d)",
                                     fan.n, up > 0 ? "anode" : "cathode",
                                     anodeRow);
                            dispFound.valid = true;
                            dispFound.commonAnode = (up > 0);
                            dispFound.common = anodeRow;
                            dispFound.n = (fan.n < 16) ? fan.n : 16;
                            for (int q = 0; q < dispFound.n; q++)
                                dispFound.rows[q] = fan.rows[q];
                            for (int q = 0; q < fan.n; q++) {
                                int pr = nodeToPrintRow(fan.rows[q]);
                                if (pr >= 0)
                                    b.printRawRow(0b00011111, pr,
                                                  PARTS_ROLE_K_COLOR, 0xffffff);
                            }
                            int prA = nodeToPrintRow(anodeRow);
                            if (prA >= 0)
                                b.printRawRow(0b00011111, prA,
                                              PARTS_ROLE_A_COLOR, 0xffffff);
                            requestLedShow(2);
                        } else if (fan.n >= 2 && !partsAutoAbortCheck()) {
                            // a real chip cluster: the fan's members feed
                            // the supply-pin reader and the I2C question
                            // (bench: Kevin's SSD1306 on rows 1-4 lands
                            // here - this path no longer needs the flags)
                            int cl[6];
                            int nCl = 0;
                            cl[nCl++] = anodeRow;
                            for (int q = 0; q < fan.n && nCl < 6; q++)
                                cl[nCl++] = fan.rows[q];
                            ClusterPower cp;
                            char i2cLine[64] = "";
                            if (nCl >= 3 &&
                                partsFindClusterPower(cl, nCl, &cp)) {
                                Serial.print("  its clamps say row ");
                                Serial.print(cp.gndRow);
                                Serial.print(" is ground and row ");
                                Serial.print(cp.vddRow);
                                Serial.println(" is the supply");
                                if (partsProbeClusterI2C(cp, i2cLine,
                                                         sizeof(i2cLine),
                                                         &i2cModule))
                                    snprintf(line, sizeof(line), "rows %d-%d: %s",
                                             lo, hi, i2cLine);
                            }
                        }
                        for (int r = a; r <= z; r++) {
                            int pr = nodeToPrintRow(r);
                            if (pr >= 0)
                                b.printRawRow(0b00011111, pr,
                                              partsTapHue(sp, nSpans, false), 0xffffff);
                        }
                        requestLedShow(2);
                    }
                }

                if (chipStarRow > 0) {
                    // reported above - never as a discrete diode
                } else if (res.status == 0 && !partsBjtVbePlausible(res)) {
                    // it PASSED the hFE test with an impossible Vbe: chip
                    // pins can fake transistor action (a TTL input IS a
                    // multi-emitter transistor), but they can't fake the
                    // junction physics. Say what it probably is, offer
                    // nothing for placement.
                    snprintf(line, sizeof(line),
                             "rows %d-%d: transistor-ish, but Vbe %.2fV is"
                             " wrong - a chip's pins?",
                             a, z, (double)res.value);
                    for (int r = a; r <= z; r++) {
                        int pr = nodeToPrintRow(r);
                        if (pr >= 0)
                            b.printRawRow(0b00011111, pr,
                                          partsTapHue(sp, nSpans, false), 0xffffff);
                    }
                    requestLedShow(2);
                } else if (res.status == 0 && res.type != PartType::EMPTY &&
                    res.type != PartType::UNKNOWN) {
                    char detail[24] = "";
                    partsResultDetail(res, detail, sizeof(detail));
                    snprintf(line, sizeof(line), "rows %d-%d: %s %s", a, z,
                             partTypeName(res.type), detail);
                    if (nAddable < 8 && res.type != PartType::SHORT_CIRCUIT)
                        addable[nAddable++] = res;   // actable finding
                    lineRgb = partsTypeColor(res.type, res.value);
                    // one color language: role colors for junction legs,
                    // the type's hue for the rest, an LED in the color it
                    // would glow
                    for (int t = 0; t < res.nRows; t++)
                        partsPaintRow((int)res.rows[t],
                                      partsResultRowColor(res, t));
                    requestLedShow(2);
                } else {
                    partsMeterDone(a, (width == 3) ? a + 1 : -1, z);
                    // an unclear small span can be a module's whole census
                    // FACE: an unpowered SPI display flags only its power
                    // end (bench, 2026-08-30: the ST7789 on 22-28 censused
                    // as exactly 22/23/24 and read "something (unclear)").
                    // Rails + the member probe see the rest of it.
                    int seed3[3];
                    int nSeed3 = 0;
                    for (int r = a; r <= z && nSeed3 < 3; r++)
                        if (flags[r] == 1 || flags[r] == 5) seed3[nSeed3++] = r;
                    if (!sipFound.valid && nSeed3 >= 3 &&
                        !partsAutoAbortCheck() &&
                        partsFindSipModule(flags, seed3, nSeed3, nullptr,
                                           &sipFound, line, sizeof(line))) {
                        for (int r = sipFound.lo; r <= sipFound.hi; r++) {
                            int pr = nodeToPrintRow(r);
                            if (pr >= 0)
                                b.printRawRow(0b00011111, pr,
                                              partsTapHue(sp, nSpans, false),
                                              0xffffff);
                        }
                        requestLedShow(2);
                    } else {
                        snprintf(line, sizeof(line),
                                 "rows %d-%d: something (unclear)", a, z);
                    }
                }
            } else {
                // NEXT-LAYER CHECKS (Kevin's ask: a wide span must EARN "one
                // chip"). First, is it even a part? One 2-row identify says:
                // its powered guard refuses (-4) when the rows are FED - and
                // fed rows are power, not a component. Then, for spans small
                // enough to walk, adjacent hit pairs get real identifies -
                // two 2-leg parts side by side are NOT a 4-leg chip, and
                // each discrete found splits out of the span. Chip pins
                // read EMPTY between themselves and stay pooled.
                Serial.print("  measuring rows ");
                Serial.print(a);
                Serial.print("-");
                Serial.print(z);
                Serial.println("...");
                Serial.flush();
                for (int r = a; r <= z; r++) partsPaintRow(r, 0x0A0A0C);
                requestLedShow(2);   // the span under investigation
                if (oled.oledConnected) {
                    char t[48];
                    snprintf(t, sizeof(t), "rows %d-%d:\nchecking...", a, z);
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText(t);
                }
                bool poweredSpan = false;
                int nSplit = 0, legsLeft = width;
                bool consumed[61] = {false};
                if (!partsAutoAbortCheck()) {
                    int r = a;
                    while (r < z && !partsAutoAborted) {
                        bool rHit = (flags[r] == 1 || flags[r] == 5);
                        bool nHit = (flags[r + 1] == 1 || flags[r + 1] == 5);
                        if (!rHit || consumed[r] || !nHit || consumed[r + 1]) {
                            r++;
                            continue;
                        }
                        partsMeterViz2(r, r + 1);
                        PartResult pres = identifyTwoLead(r, r + 1);
                        if (pres.status == -4) {
                            poweredSpan = true;
                            break;
                        }
                        bool discrete =
                            pres.status == 0 &&
                            (pres.type == PartType::RESISTOR ||
                             pres.type == PartType::DIODE ||
                             pres.type == PartType::ZENER ||
                             pres.type == PartType::LED ||
                             pres.type == PartType::CAPACITOR ||
                             pres.type == PartType::SHORT_CIRCUIT);
                        if (discrete && (pres.type == PartType::DIODE ||
                                         pres.type == PartType::ZENER ||
                                         pres.type == PartType::LED ||
                                         pres.type == PartType::RESISTOR ||
                                         pres.type == PartType::SHORT_CIRCUIT)) {
                            // the same clamp trap as the 2-row span: a
                            // "diode" inside a chip-wide span is probably
                            // the chip's clamp - the star test referees
                            // against the span's other hit rows. LED counts:
                            // a clamp's forward drop is a MEASUREMENT, and a
                            // high one reads as an LED (bench: Kevin's
                            // SSD1306 module scans Vdd->SCL as "LED 2.23V").
                            // Letting LED skip this split his whole module
                            // into a phantom discrete and starved the chip
                            // check of the legs it needs. Resistive verdicts
                            // sit in the same trap and now share the
                            // referee: the bench 7447 split "RESISTOR 2" out
                            // of its own pins 3/4 (2026-08-28) and lost two
                            // legs to it. The star test only needs an
                            // anchor, and it reads both directions, so a
                            // roleless pair anchors on either row.
                            int anode2 = (pres.roles[0] == PinRole::A)
                                             ? (int)pres.rows[0]
                                             : (int)pres.rows[1];
                            int cands[4];
                            int nc = 0;
                            for (int rr = a; rr <= z && nc < 4; rr++) {
                                if (rr == r || rr == r + 1 || consumed[rr]) continue;
                                if (flags[rr] == 1 || flags[rr] == 5) cands[nc++] = rr;
                            }
                            if (nc > 0 && partsDiodeIsChipClamp(anode2, cands, nc))
                                discrete = false;   // stays pooled with the chip
                        }
                        if (discrete && (pres.type == PartType::DIODE ||
                                         pres.type == PartType::ZENER)) {
                            // a junction pair that survived the clamp star
                            // may still be two legs of a TRANSISTOR (bench,
                            // 14:00: a PNP's E-B split off as "D17" when
                            // its record was missing) - the ≤3-row branch
                            // runs the three-lead identify here, so the
                            // walk must too. The third leg is the pair's
                            // NEIGHBOR (r+2 first, then r-1, the ≤3
                            // branch's shape) - the span's first hit can
                            // be a different part entirely (bench: span
                            // 16-19 put the 7400's row 16 ahead of the
                            // PNP's own collector on 19). Vbe referees.
                            int tryRows[2] = {(r + 2 <= z) ? r + 2 : -1,
                                              (r - 1 >= a) ? r - 1 : -1};
                            for (int ti = 0; ti < 2; ti++) {
                                int extra = tryRows[ti];
                                if (extra < 0 || consumed[extra]) continue;
                                if (flags[extra] != 1 && flags[extra] != 5)
                                    continue;
                                if (partsAutoAbortCheck()) break;
                                partsMeterViz3(r, r + 1, extra);
                                PartResult r3 =
                                    (extra > r + 1) ? identifyThreeLead(r, r + 1, extra)
                                    : identifyThreeLead(extra, r, r + 1);
                                if (r3.status == 0 &&
                                    (r3.type == PartType::BJT_PNP ||
                                     r3.type == PartType::BJT_NPN ||
                                     r3.type == PartType::NFET ||
                                     r3.type == PartType::PFET) &&
                                    partsBjtVbePlausible(r3)) {
                                    pres = r3;
                                    break;
                                }
                            }
                        }
                        if (discrete) {
                            int splitLo = r, splitHi = r + 1;
                            consumed[r] = consumed[r + 1] = true;
                            legsLeft -= 2;
                            for (int t = 0; t < pres.nRows; t++) {
                                int pr2 = (int)pres.rows[t];
                                if (pr2 >= 1 && pr2 <= 60 && !consumed[pr2]) {
                                    consumed[pr2] = true;
                                    legsLeft--;
                                }
                                if (pr2 < splitLo) splitLo = pr2;
                                if (pr2 > splitHi) splitHi = pr2;
                            }
                            nSplit++;
                            if (nAddable < 8 && pres.type != PartType::SHORT_CIRCUIT)
                                addable[nAddable++] = pres;   // actable finding
                            char detail[24] = "";
                            partsResultDetail(pres, detail, sizeof(detail));
                            Serial.print("\r\n  rows ");
                            Serial.print(splitLo);
                            Serial.print("-");
                            Serial.print(splitHi);
                            Serial.print(": ");
                            partsTermRgb(partsTypeColor(pres.type, pres.value));
                            Serial.print(partTypeName(pres.type));
                            Serial.print(" ");
                            Serial.print(detail);
                            partsTermReset();
                            Serial.println(" (split from the span)");
                            for (int t = 0; t < pres.nRows; t++)
                                partsPaintRow((int)pres.rows[t],
                                              partsResultRowColor(pres, t));
                            requestLedShow(2);
                            r += 2;
                        } else {
                            r++;
                        }
                        // wide spans get the powered probe and at most a few
                        // splits - a 24-leg walk would take half a minute
                        if (width > 8 && nSplit == 0) break;
                    }
                }
                // Still a chip after the walk? Then identify what it IS.
                // Its clamp diodes name the supply pins, and a powered
                // module responds on the bus - a real identity is better
                // than "a chip?" (Kevin, 12:53: "Can we sense I2C data
                // lines").
                char i2cLine[64] = "";
                ClusterPower cp;   // shared with the chip second look below
                bool cpValid = false;
                if (!poweredSpan && legsLeft >= 3 && width <= 6 &&
                    !partsAutoAbortCheck()) {
                    int cl[6];
                    int nCl = 0;
                    for (int r = a; r <= z && nCl < 6; r++)
                        if ((flags[r] == 1 || flags[r] == 5) && !consumed[r])
                            cl[nCl++] = r;
                    if (nCl >= 3 && partsFindClusterPower(cl, nCl, &cp)) {
                        cpValid = true;
                        Serial.print("  its clamps say row ");
                        Serial.print(cp.gndRow);
                        Serial.print(" is ground and row ");
                        Serial.print(cp.vddRow);
                        Serial.println(" is the supply");
                        partsProbeClusterI2C(cp, i2cLine, sizeof(i2cLine),
                                             &i2cModule);
                    }
                }
                bool chipVerdict = false;
                if (poweredSpan) {
                    snprintf(line, sizeof(line),
                             "rows %d-%d read POWERED - not a part", a, z);
                } else if (i2cLine[0] != '\0') {
                    snprintf(line, sizeof(line), "rows %d-%d: %s", a, z, i2cLine);
                } else if (nSplit > 0 && legsLeft <= 0) {
                    snprintf(line, sizeof(line), "rows %d-%d: %d separate parts",
                             a, z, nSplit);
                } else if (nSplit > 0) {
                    snprintf(line, sizeof(line),
                             "rows %d-%d: %d parts + %d legs (a chip?)", a, z,
                             nSplit, legsLeft);
                    chipVerdict = true;
                } else {
                    snprintf(line, sizeof(line), "rows %d-%d: %d legs (a chip?)",
                             a, z, width);
                    chipVerdict = true;
                }
                // A chip verdict on a BOTTOM-half span pairs with hits in
                // its mirror window: the far side of the same U (the dip
                // rule - pin 1 anchors the bottom, so the bottom span is
                // the near side). Kevin's 7447 censused as span 32-39 plus
                // top hits 2,3,5,9 and was offered NOTHING; the second
                // bench round censused the bottom raggeder still (33 +
                // 37-40 against top span 3-10) and a window-only union
                // sized it dip8. So: the seed hits adopt their WHOLE
                // far-side span (gap-1 tolerant, the span former's own
                // rule), and then the chip itself gets probed on every
                // unlit row (the second look below).
                int nChipFBefore = nChipF;
                if (chipVerdict && a >= 31 && nChipF < 2) {
                    auto topHit = [&](int r) {
                        return r >= 1 && r <= 30 && !crossUsed[r] &&
                               (flags[r] == 1 || flags[r] == 5);
                    };
                    int mLo = 0, mHi = 0;
                    for (int r = a - 30; r <= z - 30; r++) {
                        if (!topHit(r)) continue;
                        if (mLo == 0) mLo = r;
                        mHi = r;
                    }
                    if (mLo > 0) {
                        for (;;) {
                            if (topHit(mLo - 1)) { mLo -= 1; continue; }
                            if (topHit(mLo - 2)) { mLo -= 2; continue; }
                            break;
                        }
                        for (;;) {
                            if (topHit(mHi + 1)) { mHi += 1; continue; }
                            if (topHit(mHi + 2)) { mHi += 2; continue; }
                            break;
                        }
                        int lo = a, hi = z;
                        if (mLo + 30 < lo) lo = mLo + 30;
                        if (mHi + 30 > hi) hi = mHi + 30;

                        // The SECOND LOOK (Kevin, 2026-08-28: "more passes
                        // in different configurations"): with the union
                        // sized, probe every unlit row inside it - plus one
                        // column past each edge - against the cluster's
                        // rails (partsChipMemberProbe; a different physics
                        // than the poke, so it sees the pins the poke
                        // can't). Needs the rails: when the clamps can't
                        // name them, the union stands unprobed.
                        // ponytail: interior confirms only flag/paint (the
                        // union already spans them); a phantom pairing (a
                        // top SIP module over an unrelated bottom cluster)
                        // survives until the confirm prompt names rows the
                        // user knows are two parts.
                        // rails: reuse the i2c path's clamp reading when it
                        // ran (width <= 6 spans - the exact 37-40 case paid
                        // twice before this), search for them over the UNION's
                        // confirmed rows otherwise
                        // The corner substrate diode first - one junction
                        // read, cannot tie (see partsCornerRails)
                        if (!cpValid)
                            cpValid = partsCornerRails(lo, hi, &cp);
                        if (!cpValid) {
                            // CORNERS FIRST (2026-08-30: the 4051 scan
                            // voted vdd=INH). The old hi-downward fill
                            // spent all 6 slots on one half, so a
                            // right-side-up chip's VDD - a TOP corner on
                            // the standard layout - was never even a
                            // candidate and a signal pin won the vote.
                            // (The 08-28 7447 dodged this by sitting
                            // rotated.) The vote still ties on symmetric
                            // CMOS clamp meshes - it is the last resort.
                            int conf[6];
                            int nConf = 0;
                            int want[6] = {hi, lo, hi - 30, lo - 30,
                                           hi - 1, lo + 1};
                            auto confAdd = [&](int r) {
                                if (nConf >= 6) return;
                                for (int k = 0; k < nConf; k++)
                                    if (conf[k] == r) return;
                                if (r >= 31 && r <= 60) {
                                    if ((flags[r] == 1 || flags[r] == 5) &&
                                        !consumed[r])   // split-out parts
                                        conf[nConf++] = r;
                                } else if (r >= 1 && r <= 30 && topHit(r)) {
                                    conf[nConf++] = r;
                                }
                            };
                            for (int k = 0; k < 6; k++) confAdd(want[k]);
                            for (int r = hi; r >= lo && nConf < 6; r--)
                                confAdd(r);
                            for (int r = lo - 30; r <= hi - 30 && nConf < 6;
                                 r++)
                                confAdd(r);
                            if (nConf >= 3 && !partsAutoAbortCheck())
                                cpValid = partsFindClusterPower(conf, nConf, &cp);
                        }
                        if (cpValid) {
                            // "rails LOOK like", not "rails are": on an
                            // unpowered bipolar chip VCC conducts to every
                            // pin through its internal resistors, so the
                            // junction map sees TWO universal anodes and can
                            // only tie between them (bench 7447, both at
                            // seen=3 unanimous). Good enough to find the
                            // remaining pins, which is all it is used for
                            // here; partsResolveChipRails re-reads the drops
                            // and is the authority for the identify.
                            Serial.print("  second look: rails look like gnd ");
                            Serial.print(cp.gndRow);
                            Serial.print(" vdd ");
                            Serial.print(cp.vddRow);
                            Serial.println(" - checking the unlit rows...");
                            Serial.flush();
                            auto probe = [&](int r) {
                                if (r < 1 || r > 60) return false;
                                if (r <= 30 && crossUsed[r]) return false;
                                if (flags[r] != 0) return false;
                                if (partsAutoAbortCheck()) return false;
                                if (!partsChipMemberProbe(r, cp.gndRow,
                                                          cp.vddRow))
                                    return false;
                                flags[r] = 5;
                                partsScanViz(r, 1);
                                Serial.print("  row ");
                                Serial.print(r);
                                Serial.println(" conducts - a pin");
                                return true;
                            };
                            for (int r = lo; r <= hi; r++) probe(r);
                            for (int r = lo - 30; r <= hi - 30; r++) probe(r);
                            // edges: a column joins if EITHER of its rows
                            // conducts; stop at the first silent column
                            while (lo - 1 >= 31 &&
                                   hi - lo + 1 < MAX_PART_PINS / 2 &&
                                   (probe(lo - 1) || probe(lo - 31)))
                                lo--;
                            while (hi + 1 <= 60 &&
                                   hi - lo + 1 < MAX_PART_PINS / 2 &&
                                   (probe(hi + 1) || probe(hi - 29)))
                                hi++;
                        }
                        int w = hi - lo + 1;
                        if (w >= 2 && w <= MAX_PART_PINS / 2) {
                            chipBase[nChipF] = lo;
                            chipWidth[nChipF] = w;
                            chipGnd[nChipF] = cpValid ? cp.gndRow : -1;
                            chipVdd[nChipF] = cpValid ? cp.vddRow : -1;
                            nChipF++;
                        }
                    }
                }
                // A chip-ish span that did NOT pair into a DIP can be a
                // SIP module: the top-half shape the union pairing punts
                // on (see the phantom-pairing note above), and a bottom-
                // half module with an empty mirror window. The rails and
                // member probe are the second look's own physics; the
                // record anchor does the naming. A TOP-half span whose
                // MIRROR window holds census hits is almost certainly a
                // DIP's far side - the pairing pass (which owns the
                // corner-diode ask) will adopt it when its bottom span
                // comes around, and the module attempt here only burned
                // 20s of pair-measuring on the 4051/393 tops (Kevin,
                // 22:01: "speed it up a little"). A genuine top-half
                // module over a coincidentally busy bottom loses its
                // module offer - the confirm prompts referee that, same
                // as the phantom-pairing note above.
                bool mirrorBusy = false;
                if (a <= 30) {
                    int mh = 0;
                    for (int r = a + 30; r <= z + 30 && r <= 60; r++)
                        if (flags[r] == 1 || flags[r] == 5) mh++;
                    mirrorBusy = (mh >= 2);
                }
                if (chipVerdict && nChipF == nChipFBefore && !sipFound.valid &&
                    !mirrorBusy && !partsAutoAbortCheck()) {
                    int seedW[6];
                    int nSeedW = 0;
                    for (int r = a; r <= z && nSeedW < 6; r++)
                        if ((flags[r] == 1 || flags[r] == 5) && !consumed[r])
                            seedW[nSeedW++] = r;
                    if (nSeedW >= 3)
                        partsFindSipModule(flags, seedW, nSeedW,
                                           cpValid ? &cp : nullptr, &sipFound,
                                           line, sizeof(line));
                }
                for (int r = a; r <= z && !poweredSpan; r++) {
                    if (consumed[r]) continue;
                    int pr = nodeToPrintRow(r);
                    if (pr >= 0) b.printRawRow(0b00011111, pr, partsTapHue(sp, nSpans, false), 0xffffff);
                }
                requestLedShow(2);
            }
            Serial.print("\r\n  ");
            if (lineRgb) partsTermRgb(lineRgb);
            else if (strstr(line, "chip") != nullptr ||
                     strstr(line, "display") != nullptr ||
                     strstr(line, "module") != nullptr)
                partsTermRgb(0x18203A);   // chip-ish findings: steel blue
            else partsTermDim();          // placed / noise / unclear
            Serial.print(line);
            partsTermReset();
            Serial.println();
            if (!noiseLine && shown < 2 &&
                sumLen + strlen(line) + 2 < sizeof(summary)) {
                sumLen += (size_t)snprintf(summary + sumLen, sizeof(summary) - sumLen,
                                           "%s%s", shown ? "\n" : "", line);
                shown++;
            }
        }

        if (partsAutoAborted) {
            partsPrintAborted();
        } else {
            // the confirmed I2C module, the paired chips, the wireable
            // display and the anchored SIP module count too
            int nPlaceable = nAddable + nChipF + (i2cModule.valid ? 1 : 0) +
                             (dispFound.valid ? 1 : 0) +
                             (sipFound.valid ? 1 : 0);
            Serial.print("auto scan done in ");
            Serial.print((millis() - scanT0) / 1000);
            if (nPlaceable > 0) {
                Serial.print("s - ");
                Serial.print(nPlaceable);
                Serial.println(" placeable (CONNECT adds them to the board)");
            } else {
                Serial.println("s (add parts via Parts > Place)");
            }
            if (oled.oledConnected) {
                if (nSpans == 0) {
                    oled.clearPrintShow("board looks\nempty", 2, true, true, true);
                } else {
                    oled.resetMultiLineSmallText();
                    oled.showMultiLineSmallText(summary);
                }
            }
            // With findings, the confirm below IS the wait - the extra
            // press between "done" and the add prompt was pure friction
            // (Kevin, 14:00: it took long "even to give me the add to the
            // board prompt"). With nothing to add, hold the summary.
            if (nPlaceable == 0 || partsAutoAborted) partsWaitForPress();

            // Act on the findings, ONE AT A TIME (Kevin, 20:17: "ask per
            // part, not all or nothing"): every placeable finding gets its
            // own yes/no (the shared gesture set), the display's question
            // is "wire it", and a stray serial byte ends the pass early.
            if (nPlaceable > 0 && !partsAutoAborted) {
                Serial.print("\r\nadd confirm n=");
                Serial.println(nPlaceable);
                Serial.flush();
                int placed = 0;
                bool bail = false;
                for (int q = 0; q < nAddable && !bail; q++) {
                    int rlo = 61, rhi = 0;
                    for (int t = 0; t < addable[q].nRows; t++) {
                        int rr = (int)addable[q].rows[t];
                        if (rr < rlo) rlo = rr;
                        if (rr > rhi) rhi = rr;
                    }
                    char what[48];
                    snprintf(what, sizeof(what), "%s rows %d-%d",
                             partTypeName(addable[q].type), rlo, rhi);
                    // the freed line says WHICH one: the measured value,
                    // in the summary's own idiom (ohms / volts)
                    char detail[24] = "";
                    partsResultDetail(addable[q], detail, sizeof(detail));
                    int ans = partsConfirmOne(
                        "add", what, detail,
                        partsTypeColor(addable[q].type, addable[q].value));
                    if (ans < 0) { bail = true; break; }
                    if (ans == 1 && partsPlaceScanResult(addable[q])) placed++;
                }
                for (int q = 0; q < nChipF && !bail; q++) {
                    char what[48];
                    snprintf(what, sizeof(what), "dip%d chip rows %d-%d",
                             2 * chipWidth[q], chipBase[q],
                             chipBase[q] + chipWidth[q] - 1);
                    char detail[24];   // the freed line: the far side
                    snprintf(detail, sizeof(detail), "far side %d-%d",
                             chipBase[q] - 30,
                             chipBase[q] - 30 + chipWidth[q] - 1);
                    int ans = partsConfirmOne("add", what, detail);
                    if (ans < 0) { bail = true; break; }
                    if (ans != 1) continue;
                    // The "identify?" escalation (5.2 surface a): the chip
                    // itself can be identified - Tier-1 clamp
                    // fingerprint, then partdb truth-table vectors. Opt-in,
                    // because it powers the part. Offered even when the
                    // span's clamp map named no rails: partsIdentifyChip
                    // resolves them from the drops (and re-checks the ones
                    // it WAS handed - the bench 7447's guess was its VCC).
                    ChipIdentify ident;
                    ident.nPass = 0;
                    {
                        int ans2 = partsConfirmOne("identify", what,
                                                   "powers it briefly");
                        if (ans2 < 0) { bail = true; break; }
                        if (ans2 == 1) {
                            if (oled.oledConnected)
                                oled.clearPrintShow("identifying...", 2,
                                                    true, true, true);
                            partsIdentifyChip(chipBase[q], chipWidth[q],
                                              &chipGnd[q], &chipVdd[q],
                                              &ident);
                            if (partsAutoAborted) { bail = true; break; }
                        }
                    }
                    // WHICH chip? (Kevin, 2026-08-28: "allow users to
                    // assign a chip when we find a generic IC"). Every
                    // partdb record with this exact DIP footprint, behind
                    // a leading Generic stop - click-click keeps the fast
                    // path, a scroll names the real part and places ITS
                    // record instead: pin names, roles, power auto-route
                    // and drivers included. Hold = changed my mind, skip
                    // this finding; a serial byte ends the pass. When the
                    // vectors NAMED survivors, the list is exactly them
                    // (5.2: "offer the picker filtered to survivors") with
                    // the best one preselected - and their orientation is
                    // proven, so no pin-1 tap.
                    int nOpt = 1;
                    s_led[0] = "IC?";
                    s_title[0] = "Generic IC";
                    s_desc[0] = "unnamed pins";
                    s_rec[0] = 0xFFFF;
                    uint8_t rotFor[PARTS_LIST_MAX] = {0};
                    bool taplessFor[PARTS_LIST_MAX] = {false};
                    if (ident.nPass >= 1) {
                        for (int i = 0; i < ident.nTried &&
                                        nOpt < PARTS_LIST_MAX; i++) {
                            if (ident.res[i].verdict != 1) continue;
                            const PartDbRecord& r =
                                partdb_records[ident.res[i].recIdx];
                            s_led[nOpt] = r.ledName;
                            s_title[nOpt] = r.displayName;
                            s_desc[nOpt] = "vectors match";
                            s_rec[nOpt] = ident.res[i].recIdx;
                            rotFor[nOpt] = ident.res[i].rotated;
                            taplessFor[nOpt] = true;
                            nOpt++;
                        }
                    } else {
                        for (uint16_t i = 0; i < partdb_numRecords &&
                                             nOpt < PARTS_LIST_MAX; i++) {
                            const PartDbRecord& r = partdb_records[i];
                            const PartDbPinout& p2 =
                                partdb_pinouts[r.pinoutIdx];
                            if (p2.footprint != PARTDB_FOOT_DIP) continue;
                            if ((int)p2.pinCount != 2 * chipWidth[q]) continue;
                            if (!partdbPlaceableHere(r)) continue;
                            s_led[nOpt] = r.ledName;
                            s_title[nOpt] = r.displayName;
                            s_desc[nOpt] = r.desc;
                            s_rec[nOpt] = i;
                            nOpt++;
                        }
                    }
                    bool placedOne = false;
                    if (nOpt > 1) {
                        int pick = partsPicker("chip", "Which?", nOpt,
                                               ident.nPass >= 1 ? 1 : 0);
                        if (pick == -2) { bail = true; break; }
                        if (pick >= 1) {
                            const PartDbRecord& rec2 =
                                partdb_records[s_rec[pick]];
                            if (taplessFor[pick]) {
                                // the vectors proved the orientation - the
                                // rails only fit one way, so pin 1's row is
                                // knowledge, not a question
                                int p1 = partsChipPinRow(
                                    chipBase[q], 2 * chipWidth[q],
                                    rotFor[pick] != 0, 1);
                                placedOne =
                                    partsCommitPlacement(rec2, &p1, 1);
                            } else {
                                // A REAL record has a REAL pin 1 - never
                                // assume which corner it sits in (Kevin,
                                // 2026-08-28: "we need to ask for pin 1, it
                                // placed the 7447 upside down"). The tap is
                                // the truth, exactly like the place flow.
                                int p1 = partsTapForRow(rec2);
                                if (p1 == -2) { bail = true; break; }
                                if (p1 >= 1)
                                    placedOne =
                                        partsCommitPlacement(rec2, &p1, 1);
                                // p1 == -1 = changed their mind: skip
                            }
                        } else if (pick == 0) {
                            placedOne = partsPlaceFoundChip(chipBase[q],
                                                            chipWidth[q]);
                        }
                    } else {
                        // nothing in the DB wears this footprint - the
                        // generic record is the only honest offer
                        placedOne = partsPlaceFoundChip(chipBase[q],
                                                        chipWidth[q]);
                    }
                    if (placedOne) placed++;
                    // the picker's glyphs and this chip's identify paints
                    // must not bleed into the NEXT finding's test
                    partsScanStageRepaint();
                }
                if (!bail && i2cModule.valid) {
                    int rlo = i2cModule.gnd, rhi = i2cModule.gnd;
                    int r4[3] = {i2cModule.vdd, i2cModule.scl, i2cModule.sda};
                    for (int t = 0; t < 3; t++) {
                        if (r4[t] < rlo) rlo = r4[t];
                        if (r4[t] > rhi) rhi = r4[t];
                    }
                    char what[48];
                    snprintf(what, sizeof(what), "I2C 0x%02X module rows %d-%d",
                             i2cModule.addr, rlo, rhi);
                    char detail[24];   // the freed line: the bus pins
                    snprintf(detail, sizeof(detail), "SDA %d SCL %d",
                             i2cModule.sda, i2cModule.scl);
                    int ans = partsConfirmOne("add", what, detail);
                    if (ans < 0) bail = true;
                    else if (ans == 1 && partsPlaceI2cModule(i2cModule)) placed++;
                }
                if (!bail && dispFound.valid) {
                    char what[56];
                    snprintf(what, sizeof(what),
                             "%d-seg display (common %d) to GPIOs",
                             dispFound.n, dispFound.common);
                    int ans = partsConfirmOne("wire", what,   // freed line: polarity
                                              dispFound.commonAnode
                                                  ? "common anode"
                                                  : "common cathode");
                    if (ans == 1 && partsWireFoundDisplay(dispFound)) placed++;
                }
                if (!bail && sipFound.valid) {
                    // one candidate confirms; several go through the picker
                    // (closest pin count preselected). The anchor already
                    // proved orientation, so no pin-1 tap - the rails only
                    // fit the record one way.
                    int pick = -1;
                    if (sipFound.nCand > 1) {
                        for (int q = 0; q < sipFound.nCand; q++) {
                            const PartDbRecord& r =
                                partdb_records[sipFound.candRec[q]];
                            s_led[q] = r.ledName;
                            s_title[q] = r.displayName;
                            s_desc[q] = r.desc;
                            s_rec[q] = sipFound.candRec[q];
                        }
                        pick = partsPicker("module", "Which?",
                                           sipFound.nCand, 0);
                        if (pick == -2) { bail = true; pick = -1; }
                    } else {
                        const PartDbRecord& rec3 =
                            partdb_records[sipFound.candRec[0]];
                        const PartDbPinout& po3 =
                            partdb_pinouts[rec3.pinoutIdx];
                        int eLo = (sipFound.candDir[0] > 0)
                                      ? sipFound.candPin1[0]
                                      : sipFound.candPin1[0] -
                                            ((int)po3.pinCount - 1);
                        char what2[48];
                        snprintf(what2, sizeof(what2), "%s rows %d-%d",
                                 rec3.displayName, eLo,
                                 eLo + po3.pinCount - 1);
                        int ans = partsConfirmOne("add", what2, rec3.desc);
                        if (ans < 0) bail = true;
                        else if (ans == 1) pick = 0;
                    }
                    if (!bail && pick >= 0 &&
                        partsPlaceSipModule(sipFound, pick)) placed++;
                    partsScanStageRepaint();   // no stale picker glyphs
                }
                if (placed > 0) {
                    partLabels.requestRun();
                    if (oled.oledConnected) {
                        char t[32];
                        snprintf(t, sizeof(t), "added\n%d part%s", placed,
                                 placed == 1 ? "" : "s");
                        oled.clearPrintShow(t, 2, true, true, true);
                        delay(800);
                    }
                }
            }
        }
    }

adone:
    // the user's wiring AND the parked probe feed go back, duplicate
    // stacking and all, in ONE refresh - every exit path funnels through
    // here (infraEvaluate at the refresh head reads the restored flag)
    infraSetProbePowerEnabled(scanPpRestore);
    if (scanLiftN > 0) {
        for (int i = 0; i < scanLiftN; i++)
            addBridgeToState(scanLiftA[i], scanLiftB[i], scanLiftDup[i], false);
        Serial.print("board restored (");
        Serial.print(scanLiftN);
        Serial.println(" wires back)");
        scanLiftN = 0;
    }
    refreshConnections(-1, 0, 0);
    partsAutoAborted = false;
    partsAutoAbortCause = nullptr;
    s_scanVizFlags = nullptr;   // it points at this frame's flags[] - never
                                // let partsMeterDone read it after we return
    partScanActivityHook = nullptr;
    s_meterRows[0] = s_meterRows[1] = s_meterRows[2] = -1;
    inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear();
    partLabels.clearTransients();   // standing overlays retire on app exit
    requestLedShow(-1);
    Serial.println();
    oled.showJogo32h();
}
