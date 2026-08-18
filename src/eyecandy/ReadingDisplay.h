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
 *
 * Repeat calls with identical content are dropped, so callers that fire every
 * loop don't flicker the panel. Call resetLastShown() when something else has
 * painted over the display.
 */
void show(const char* name, int rowNode, const char* value = nullptr,
          const char* value2 = nullptr);

/// Name-only screen (no measurement): renders as large as it fits.
inline void showName(const char* name) { show(name, -1); }

/// The serial half, kept narrow on purpose: the pinned-live-line manager
/// reimplements these two and nothing else.
void emitLiveSerialLine(const char* line);
void clearLiveSerialLine(void);

/// Forget what was last shown, so the next show() always repaints.
void resetLastShown(void);

}  // namespace ReadingDisplay

#endif  // READINGDISPLAY_H
