#ifndef STATSPAGES_H
#define STATSPAGES_H

/**
 * The clickwheel scroll's stops between the last row and the first (Kevin,
 * 2026-11-10): as the wheel loops round the board it passes a few OLED pages
 * of live board status - every supply's volts and current, every GPIO, every
 * ADC, the board itself - instead of jumping straight from the Nano header
 * back to row 1. Nothing is highlighted on these stops; they are the scroll's
 * "slot 0".
 *
 * [clickwheel] stats_pages picks the set: on = the pages, logo = the pages
 * plus the startup logo, off = one stop that shows the Jumperless logo.
 *
 * Highlighting owns WHERE the scroll is; this owns what the panel shows there
 * and keeps a page live until anything else paints.
 */
namespace StatsPages {

/// Stops available right now. 0 = none (no OLED, or a board without the
/// sensors the pages read): the scroll wraps straight to row 1 as it always did.
int count(void);

/// Land on stop `page` (0..count()-1): paint it now and keep it refreshing.
void show(int page);

/// Something else owns the panel: stop refreshing. Safe to call anytime.
void hide(void);

/// Live refresh. Call from the main loop; self-throttled, no-op when hidden.
void service(void);

}  // namespace StatsPages

#endif  // STATSPAGES_H
