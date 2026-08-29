#ifndef READINGDISPLAY_H
#define READINGDISPLAY_H

/**
 * The ONE place a measurement becomes pixels + serial text.
 *
 * Everything that shows a reading to the user - probe/encoder highlighting,
 * measure mode, the rail/DAC adjuster, the probe cursor - renders through
 * show(), so a voltage looks the same wherever it came from. Callers supply
 * only strings and a row node; layout, fonts and the serial line live here.
 */
namespace ReadingDisplay {

/**
 * Render a reading.
 *
 * OLED layout:
 *   [ name (5pt, left)              row label (5pt, right) ]
 *   [               value  (family font, centered)         ]
 *   [               value2 (family font, centered)         ]
 *
 * @param name    Header text ("Top Rail", "I Sense +", "GPIO 3 input", ...)
 * @param rowNode Node whose label goes top-right; <= 0 for none. Only shown
 *                alongside a value - a name-only screen renders large and centered.
 * @param value   First reading ("3.30 V"), nullptr/"" to omit
 * @param value2  Second reading ("4.8 mA"), nullptr/"" to omit
 * @param hint    Action prompt ("adjust?"), nullptr/"" to omit. Rendered as a
 *                small right-aligned tag on the bottom value row (there is no
 *                vertical room for a fourth row on the 32px panel), and part
 *                of the dedupe key so gating changes repaint.
 *
 * Repeat calls with identical content are dropped, so callers that fire every
 * loop don't flicker the panel. Call resetLastShown() when something else has
 * painted over the display.
 */
void show(const char* name, int rowNode, const char* value = nullptr,
          const char* value2 = nullptr, const char* hint = nullptr);

/// Name-only screen (no measurement): renders as large as it fits.
inline void showName(const char* name) { show(name, -1); }

/// The serial half, kept narrow on purpose: the pinned-live-line manager
/// reimplements these and nothing else.
void emitLiveSerialLine(const char* line);
void clearLiveSerialLine(void);

/// One-shot selection trace (the part scroll's PARTSEL line), pinned one row
/// ABOVE the live reading and rewritten in place - a raw print here would
/// scroll the terminal on every encoder detent AND knock the reading pin
/// loose. Clamped to one 80-column row. Also blanks the reading row, so emit
/// the status BEFORE the reading that belongs with it.
void emitLiveStatusLine(const char* line);
/// Blank the status row (highlight cleared / moved on) without disturbing
/// the reading below it. Safe to call anytime; no-op if the row isn't ours.
void clearLiveStatusLine(void);

/// Forget what was last shown, so the next show() always repaints.
void resetLastShown(void);

}  // namespace ReadingDisplay

#endif  // READINGDISPLAY_H
