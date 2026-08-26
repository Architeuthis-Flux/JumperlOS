#include "ReadingDisplay.h"

#include <Arduino.h>
#include <string.h>

#include "Jerial.h"       // input-line width for the pin guard
#include "NetManager.h"  // definesToChar
#include "config.h"      // jumperlessConfig
#include "oled.h"        // oled, OledTextRow, FontManager, mapConfigValueToFontFamily

// ----------------------------------------------------------------------------
// Port-1 linefeed counter - the pin's invalidation signal
// ----------------------------------------------------------------------------
// Every write to a CDC port funnels through tud_cdc_n_write() (both
// Adafruit_USBD_CDC::write overloads call it), so wrapping it (-Wl,--wrap in
// platformio.ini) sees every byte that reaches the user's main terminal,
// including raw Serial prints that bypass Jerial. Only LINEFEEDS are counted:
// a '\n' is what scrolls the terminal and moves the pinned rows, while echoed
// keystrokes, CRs and cursor-movement escapes leave them where they are. The
// pin logic below snapshots this counter after each paint; a later mismatch
// means someone else has scrolled the terminal since, so the anchor is stale
// and the next reading must pin fresh rows (and an exit-erase must not fire -
// it would wipe a line that is no longer ours).
//
// This replaces most of the guesswork resetLastShown() call sites used to do:
// the terminal owners (menus, probeMode, apps, command output) all print
// linefeeds, so the pin now invalidates itself exactly when one of them
// actually prints - not when a poll loop merely might have.
//
// KNOWN BLIND SPOT: output that scrolls WITHOUT a linefeed - a long non-LF
// write autowrapping past the terminal edge. The width guard below covers the
// one common case (the user's own typed line); other writers emitting 80+
// LF-free columns are rare enough to accept.
//
// Counted at write-call time on core 0 only (TinyUSB is only ever serviced
// from core 0 - see loop1()'s notes), so a plain volatile is enough.
static volatile uint32_t s_port1LineFeeds = 0;

extern "C" {
uint32_t __real_tud_cdc_n_write(uint8_t itf, const void* buffer, uint32_t bufsize);

uint32_t __wrap_tud_cdc_n_write(uint8_t itf, const void* buffer, uint32_t bufsize) {
    uint32_t n = __real_tud_cdc_n_write(itf, buffer, bufsize);
    if (itf == 0 && n > 0) {
        const uint8_t* p = (const uint8_t*)buffer;
        uint32_t lf = 0;
        for (uint32_t i = 0; i < n; i++) {
            lf += (p[i] == '\n');
        }
        if (lf) {
            s_port1LineFeeds += lf;
        }
    }
    return n;
}
}  // extern "C"

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

// findBestFitPointSize re-measures the string at every point size from 12
// down - up to seven full glyph walks - and show() reaches it on every repaint
// that gets past the dedupe, including the ~10 Hz live-value refreshes where a
// settled reading dithers between a handful of strings. Keyed on the exact
// text so the cached answer is always the one the search would have returned;
// the family is part of the key, and the width/size bounds are literals at the
// single call site.
constexpr int FIT_CACHE_ENTRIES = 4;
constexpr size_t FIT_CACHE_TEXT = 24;

struct FitCacheEntry {
    char text[FIT_CACHE_TEXT];
    FontFamily family;
    int16_t font;
    bool valid;
};
FitCacheEntry fitCache[FIT_CACHE_ENTRIES] = {};
int fitCacheNext = 0;

int16_t bestFitFontFor(FontFamily family, const char* text) {
    if (text != nullptr) {
        for (int i = 0; i < FIT_CACHE_ENTRIES; i++) {
            if (fitCache[i].valid && fitCache[i].family == family &&
                strcmp(fitCache[i].text, text) == 0) {
                return fitCache[i].font;
            }
        }
    }
    uint8_t pt = FontManager::findBestFitPointSize(family, text, 120, 12, 6);
    int16_t font = (int16_t)FontManager::getFontForPointSize(family, pt);
    // Only a key we can hold WHOLE goes in: a truncated copy would match the
    // wrong string later and hand back a font measured for something else.
    if (text != nullptr && strlen(text) < FIT_CACHE_TEXT) {
        FitCacheEntry& e = fitCache[fitCacheNext];
        fitCacheNext = (fitCacheNext + 1) % FIT_CACHE_ENTRIES;
        strncpy(e.text, text, FIT_CACHE_TEXT - 1);
        e.text[FIT_CACHE_TEXT - 1] = '\0';
        e.family = family;
        e.font = font;
        e.valid = true;
    }
    return font;
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
// it. Whether it HAS moved is decided by the port-1 linefeed counter at the
// top of this file, not by the callers: a paint whose snapshot still matches
// repaints in place, one whose snapshot is stale pins a fresh pair of rows
// below whatever scrolled. Blocking contexts that take over the terminal
// (probeMode, menus, apps) additionally run their loops on
// jOS.serviceInner(), which dispatches only the inner set - so no
// reading can repaint mid-takeover - and announce themselves through
// resetLastShown(), which now only drops the OLED dedupe.

namespace {
bool pinReserved = false;
// s_port1LineFeeds as of our last paint. While it still matches, nothing has
// scrolled the terminal and the pinned rows are exactly where we left them.
uint32_t pinLineFeedSnapshot = 0;
// Set when the width guard released the pin. Kept separate from lastLine (the
// OLED dedupe key) so releasing the serial pin never forces an OLED repaint.
bool serialNeedsRepin = false;

// The reserved rows are still ours: reserved, and nobody has printed a
// linefeed on port 1 since we last painted them.
bool pinStillValid(void) {
    return pinReserved && (s_port1LineFeeds == pinLineFeedSnapshot);
}

void pinCursorToLiveRow(void) {
    Serial.print("\x1b" "7");   // DECSC - save the user's input-line cursor
    Serial.print("\x1b[2A");    // CUU 2 - climb to the reading row
    Serial.print("\r\x1b[2K");  // column 0, then EL 2 (erase the whole row)
}
}  // namespace

// Past this many columns a typed line is about to wrap on an 80-column
// terminal, and the "two rows up" cursor math below would land on the wrong
// row. Release the pin instead of guessing; the next reading re-pins once the
// line is submitted (handleEnter drops the anchor) or shortened. No terminal
// width query (CSI 6 n) - those block 20-200 ms and eat typed bytes.
constexpr int kMaxInputColsForPin = 70;

void emitLiveSerialLine(const char* line) {
    if (Jerial.getInputLineColumns() >= kMaxInputColsForPin) {
        // Leave the terminal alone until the line is shorter. Drop the pin but
        // NOT lastLine: lastLine is also the OLED dedupe key, and clearing it
        // made every caller that fires per loop pass (VoltageAdjuster) repaint
        // a full OLED frame - a blocking I2C write plus a font-fit search -
        // on every pass for as long as the typed line stayed long. The separate
        // flag below is what forces the serial side to re-pin.
        pinReserved = false;
        serialNeedsRepin = true;
        return;
    }
    // Never start a write the CDC FIFO cannot take. Adafruit_USBD_CDC::write()
    // loops in yield() until a CONNECTED host drains, with no timeout, and
    // this runs BEFORE the OLED write - so a terminal that stopped reading
    // wedges the panel too. The reading is live: drop this frame's serial
    // repaint and the next value puts it back ~40 ms later. Deliberately does
    // NOT set serialNeedsRepin - that bypasses the OLED dedupe below, which
    // would repaint a 12 ms I2C frame every pass for as long as the host
    // stayed stalled.
    size_t needed = strlen(line) + 16;  // DECSC + CUU + CR + EL + DECRC
    if (!pinStillValid()) {
        // plus the re-pin's two CRLFs and Jerial's input-line redraw (its
        // visible columns and an allowance for the prompt/highlight escapes)
        needed += 4 + (size_t)Jerial.getInputLineColumns() + 32;
    }
    if ((size_t)Serial.availableForWrite() < needed) {
        return;
    }
    if (!pinStillValid()) {
        // Scroll two fresh rows into place so the reading has somewhere to
        // live that isn't the user's input line. "\n\r", not bare "\n": over
        // a raw serial link LF moves down but holds the column, which would
        // leave the input line - and every later DECRC - at a stale column.
        Serial.print("\n\r\n\r");
        // Those two newlines moved the user's input line down two rows, and the
        // cursor now sits at column 0 of a blank row - so the DECSC below would
        // capture the wrong position and the CUU+EL would erase the row the
        // input line just moved to. Repaint it first: that puts the text back
        // and leaves the cursor where the user actually is, mid-word.
        Jerial.redrawInputLine();
        pinReserved = true;
    }
    serialNeedsRepin = false;
    pinCursorToLiveRow();
    Serial.print(line);
    Serial.print("\x1b" "8");   // DECRC - back to the input line
    Serial.flush();
    // Snapshot AFTER our own writes: the re-pin above shipped exactly two
    // linefeeds, a repaint shipped none, and either way the count now on
    // record is "the terminal as we left it".
    pinLineFeedSnapshot = s_port1LineFeeds;
}

void clearLiveSerialLine(void) {
    if (!pinReserved) {
        return;
    }
    pinReserved = false;
    if (s_port1LineFeeds != pinLineFeedSnapshot) {
        // The terminal scrolled since our last paint: the reading is already
        // somewhere in the scrollback and "two rows above the cursor" is one
        // of somebody else's lines now. Erasing would wipe THEIR output.
        return;
    }
    // Wipe on the way out: the value is live, and one left frozen on screen
    // reads as current when it no longer is.
    pinCursorToLiveRow();
    Serial.print("\x1b" "8");
    Serial.flush();
}

void resetLastShown(void) {
    // Something else has painted over the OLED: drop the dedupe key so the
    // next reading repaints even if its text is unchanged.
    lastLine[0] = '\0';
    // The SERIAL pin is deliberately NOT dropped here. Whether the pinned
    // rows are still ours is not something callers can know - a poll loop
    // that merely might own the terminal is not a scroll - so the pin
    // invalidates itself instead, off the port-1 linefeed counter above:
    // if the caller really printed, the next paint sees the count moved and
    // pins fresh rows below their output; if it printed nothing, the reading
    // keeps repainting in place.
}

void show(const char* name, int rowNode, const char* value, const char* value2,
          const char* hint) {
    // Copy the name first: definesToChar() below returns a pointer into ONE
    // shared static buffer for values it can't map, so a caller that passed a
    // definesToChar() result as `name` would see it replaced by the row label.
    char nameBuf[48];
    snprintf(nameBuf, sizeof(nameBuf), "%s", (name != nullptr) ? name : "");
    const char* nameText = nameBuf;
    bool haveV1 = (value != nullptr && value[0] != '\0');
    bool haveV2 = (value2 != nullptr && value2[0] != '\0');
    bool haveValues = (haveV1 || haveV2);
    // The hint only makes sense next to a reading; a name-only screen has no
    // bottom row to tag it onto.
    bool haveHint = haveValues && (hint != nullptr && hint[0] != '\0');

    // Row label for the header's right side (row number / node name). Only
    // meaningful next to a reading - a name-only screen renders large and
    // centered with nothing beside it.
    char rowBuf[16] = "";
    const char* rowLabel = nullptr;
    if (haveValues && rowNode > 0) {
        snprintf(rowBuf, sizeof(rowBuf), "%s", definesToChar(rowNode, 0));
        // A label identical to the name is the net's own namesake pad
        // (tapping the GND pad renders "GND  GND" otherwise) - drop it.
        if (rowBuf[0] != '\0' && strcmp(rowBuf, nameText) != 0) {
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
    // The hint is part of the composed line, so it keys the dedupe (an OLED
    // arriving or the config flag flipping mid-highlight must repaint) and
    // rides the serial live line, where "adjust?" reads as "click the wheel".
    if (haveHint) {
        appendField(line, len, "  ", hint);
    }

    // A stale pin (someone scrolled since our last paint) bypasses the
    // dedupe: the reading is buried in scrollback, so resurface it below the
    // new output even if the text itself is unchanged.
    bool serialPinStale = pinReserved && (s_port1LineFeeds != pinLineFeedSnapshot);
    if (!serialNeedsRepin && !serialPinStale && strcmp(line, lastLine) == 0) {
        return;  // already on screen, and the serial pin is intact
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
        return bestFitFontFor(fam, text);
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
    // The hint tags the bottom value row, right-aligned in the 5pt label
    // font - the same idiom as the header's row label. The reading on that
    // row goes flush-left when the hint is present: centered, a wide value
    // ("10.0 mA") ran into the right-anchored tag.
    if (haveHint && n > 1) {
        rows[n - 1].segs[1] = {hint, labelFont, OLED_ALIGN_RIGHT};
        rows[n - 1].segCount = 2;
        rows[n - 1].align = OLED_ALIGN_LEFT;
    }
    // Top-anchored: header hugs the top edge, readings fill the remaining
    // height. rowGap 1 keeps the three-row stack inside 32px so the bottom
    // reading's descenders don't clip.
    oled.clearPrintShowRich(rows, n, 1, true, true, haveValues);
}

}  // namespace ReadingDisplay
