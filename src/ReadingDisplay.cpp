#include "ReadingDisplay.h"

#include <Arduino.h>
#include <string.h>

#include "NetManager.h"  // definesToChar
#include "config.h"      // jumperlessConfig
#include "oled.h"        // oled, OledTextRow, FontManager, mapConfigValueToFontFamily

namespace ReadingDisplay {

namespace {

// Widest line we compose. The serial clear below is sized to match.
constexpr size_t LINE_CAP = 96;

// What the panel is showing right now. Identical content is dropped rather
// than repainted: several callers fire on every loop pass, and re-pushing the
// same frame flickers the OLED and floods the serial line.
char lastLine[LINE_CAP] = "";

void appendField(char* buf, size_t& len, const char* sep, const char* text) {
    if (text == nullptr || text[0] == '\0' || len >= LINE_CAP - 1) {
        return;
    }
    int written = snprintf(buf + len, LINE_CAP - len, "%s%s", sep, text);
    if (written < 0) {
        return;
    }
    len += (size_t)written;
    if (len >= LINE_CAP) {
        len = LINE_CAP - 1;  // snprintf truncated
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The pinned live line
// ---------------------------------------------------------------------------
//
// A reading is a LIVE value that rewrites itself, so it gets its own two rows
// ABOVE the line the user types on, and the cursor always goes home:
//
//     3.29 V  row 25          <- pinned reading, rewritten in place
//                             <- blank spacer row
//     > user input + cursor   <- cursor restored here, mid-word
//
// It used to print at the cursor behind a run of spaces, which ate whatever
// the user was typing every time a reading changed (and the spaces only
// overwrote rightward - they never actually cleared the row, so a shorter
// reading left the tail of a longer one behind).
//
// Sequences per xterm ctlseqs:
//   ESC 7 / ESC 8   DECSC / DECRC, save + restore cursor. Preferred over the
//                   ANSI.SYS CSI s / CSI u forms, which are constrained when
//                   left/right margins are enabled.
//   CSI Ps A        CUU, cursor up Ps rows.
//   CSI Ps K        EL; Ps=2 is "erase all" (Ps=0, the default, only erases
//                   rightward - that is the tail bug above).
// NOTE the literal split in "\x1b" "7": written "\x1b7" the compiler parses
// the whole thing as ONE overlong hex escape, not ESC followed by '7'.
//
// The anchor is cursor-relative, so anything that scrolls the terminal moves
// it. Blocking contexts that take over the terminal (probeMode, menus, apps)
// run their loops on jOS.serviceCritical(), which dispatches only CRITICAL
// services - no reading can repaint underneath them - and they announce
// themselves through resetLastShown(), which drops the anchor so the next
// reading pins a fresh pair of rows below their output.

namespace {
bool pinReserved = false;

void pinCursorToLiveRow(void) {
    Serial.print("\x1b" "7");   // DECSC - save the user's input-line cursor
    Serial.print("\x1b[2A");    // CUU 2 - climb to the reading row
    Serial.print("\r\x1b[2K");  // column 0, then EL 2 (erase the whole row)
}
}  // namespace

void emitLiveSerialLine(const char* line) {
    if (!pinReserved) {
        // Scroll two fresh rows into place so the reading has somewhere to
        // live that isn't the user's input line. "\n\r", not bare "\n": over
        // a raw serial link LF moves down but holds the column, which would
        // leave the input line - and every later DECRC - at a stale column.
        Serial.print("\n\r\n\r");
        pinReserved = true;
    }
    pinCursorToLiveRow();
    Serial.print(line);
    Serial.print("\x1b" "8");   // DECRC - back to the input line
    Serial.flush();
}

void clearLiveSerialLine(void) {
    if (!pinReserved) {
        return;
    }
    // Wipe on the way out: the value is live, and one left frozen on screen
    // reads as current when it no longer is.
    pinCursorToLiveRow();
    Serial.print("\x1b" "8");
    Serial.flush();
    pinReserved = false;
}

void resetLastShown(void) {
    lastLine[0] = '\0';
    // Something else has painted over the display - and, on the serial side,
    // has almost certainly scrolled our pinned rows somewhere unknown. Drop
    // the anchor WITHOUT erasing: a blind CUU+EL now would wipe one of THEIR
    // lines. The next reading pins a fresh pair.
    pinReserved = false;
}

void show(const char* name, int rowNode, const char* value, const char* value2) {
    const char* nameText = (name != nullptr) ? name : "";
    bool haveV1 = (value != nullptr && value[0] != '\0');
    bool haveV2 = (value2 != nullptr && value2[0] != '\0');
    bool haveValues = (haveV1 || haveV2);

    // Row label for the header's right side (row number / node name). Only
    // meaningful next to a reading - a name-only screen renders large and
    // centered with nothing beside it.
    char rowBuf[16] = "";
    const char* rowLabel = nullptr;
    if (haveValues && rowNode > 0) {
        snprintf(rowBuf, sizeof(rowBuf), "%s", definesToChar(rowNode, 0));
        if (rowBuf[0] != '\0') {
            rowLabel = rowBuf;
        }
    }

    // One composed line drives both the dedupe check and the serial output.
    char line[LINE_CAP];
    size_t len = 0;
    line[0] = '\0';
    appendField(line, len, "", nameText);
    appendField(line, len, "  ", rowLabel);
    appendField(line, len, "  ", value);
    appendField(line, len, "  ", value2);

    if (strcmp(line, lastLine) == 0) {
        return;  // already on screen
    }
    strncpy(lastLine, line, LINE_CAP - 1);
    lastLine[LINE_CAP - 1] = '\0';

    emitLiveSerialLine(line);

    // Value rows honor the configured font family at the closest point size;
    // the header uses Andale Mono 5pt (index 12) - the smallest font on board -
    // because most families have no readable sub-8pt cut and three
    // family-sized rows don't fit 32px.
    FontFamily fam = mapConfigValueToFontFamily(jumperlessConfig.top_oled.font);
    int16_t medFont = (int16_t)FontManager::getFontForPointSize(fam, 8);
    int16_t labelFont = 12;  // Andale Mono 5pt

    // Single value / name-only rows render as big as actually FITS: a fixed
    // 12pt overflowed the panel on long words ("FLOATING").
    auto bestFitFont = [&](const char* text) -> int16_t {
        uint8_t pt = FontManager::findBestFitPointSize(fam, text, 120, 12, 6);
        return (int16_t)FontManager::getFontForPointSize(fam, pt);
    };

    int16_t nameFont = haveValues ? labelFont : bestFitFont(nameText);
    int16_t valueFont;
    if (haveV1 && haveV2) {
        valueFont = medFont;
    } else {
        valueFont = bestFitFont(haveV1 ? value : value2);
    }

    OledTextRow rows[3] = {};
    int n = 0;
    rows[n].segs[0] = {nameText, nameFont, OLED_ALIGN_INHERIT};
    rows[n].segCount = 1;
    if (haveValues && rowLabel != nullptr) {
        rows[n].segs[1] = {rowLabel, labelFont, OLED_ALIGN_RIGHT};
        rows[n].segCount = 2;
    }
    rows[n].align = haveValues ? OLED_ALIGN_LEFT : OLED_ALIGN_CENTER;
    // Fixed header height: measured ink boxes vary per string (descenders,
    // ascender mixes), which made some labels render 2px lower than others.
    // Pinning the row to the 5pt cap height gives every header the same
    // baseline; descender tails hang into the row gap below.
    if (haveValues) {
        rows[n].fixedH = 7;
    }
    n++;
    if (haveV1) {
        rows[n].segs[0] = {value, valueFont, OLED_ALIGN_INHERIT};
        rows[n].segCount = 1;
        rows[n].align = OLED_ALIGN_CENTER;
        n++;
    }
    if (haveV2) {
        rows[n].segs[0] = {value2, valueFont, OLED_ALIGN_INHERIT};
        rows[n].segCount = 1;
        rows[n].align = OLED_ALIGN_CENTER;
        n++;
    }
    // Top-anchored: header hugs the top edge, readings fill the remaining
    // height. rowGap 1 keeps the three-row stack inside 32px so the bottom
    // reading's descenders don't clip.
    oled.clearPrintShowRich(rows, n, 1, true, true, haveValues);
}

}  // namespace ReadingDisplay
