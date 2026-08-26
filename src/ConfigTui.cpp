// ============================================================================
// ConfigTui - the interactive config editor.
//
// Generated ENTIRELY from the descriptor table in configManager.cpp: category
// list on the left, one-sentence description + range + default on the right,
// arrow keys to navigate and change values. Every change goes through
// configSetValue(live=true), so the same hooks the paste path uses fire here
// too - brightness sliders repaint the board while you hold the arrow key,
// menu FX changes retint the next transition, the OLED font redraws, etc.
//
// Opened by a bare ` at the main menu (readConfigFromSerial falls through to
// configTuiRun when nothing is pasted).
//
// Keys:
//   Up/Down       move        Left/Right   change value (step / cycle)
//   Enter         edit inline (numbers, hex, strings) or toggle (bools)
//   d             reset the highlighted option to its default
//   PgUp/PgDn/Home/End  scroll     Esc/q    back / exit
// ============================================================================

#include <Arduino.h>
#include "configManager.h"
#include "Tui.h"
#include "Graphics.h"
#include "Jerial.h"
#include "PersistentStuff.h"  // readSettingsFromConfig

// Menu FX tuner (Debugs.cpp) - drives the real breadboard menu live.
void action_menuTransitionTuner(void);

namespace {

using namespace Tui;

// ---------------------------------------------------------------------------
// Category model: the visible sections plus All / Menu FX / Reset entries.
// ---------------------------------------------------------------------------
enum CatAction : uint8_t { CAT_SECTION, CAT_ALL, CAT_MENUFX, CAT_RESET, CAT_EXIT };

struct Category {
    const char* title;
    const char* desc;
    CatAction action;
    int8_t section;       // when CAT_SECTION
    uint16_t extraFlag;   // cross-listed [debug] options (JLC_SHOW_*)
};

// Order: the new "what you actually touch" categories first, plumbing later.
static const Category kCategories[] = {
    { "All",          "Every option in one flat list.",                          CAT_ALL,     -1, 0 },
    { "Probe",        nullptr, CAT_SECTION, JLSECT_probe,       JLC_SHOW_PROBE },
    { "Clickwheel",   nullptr, CAT_SECTION, JLSECT_clickwheel,  0 },
    { "Measurement",  nullptr, CAT_SECTION, JLSECT_measurement, JLC_SHOW_MEASURE },
    { "Terminal",     nullptr, CAT_SECTION, JLSECT_terminal,    0 },
    { "Undo",         nullptr, CAT_SECTION, JLSECT_undo,        0 },
    { "Display",      nullptr, CAT_SECTION, JLSECT_display,     0 },
    { "DACs",         nullptr, CAT_SECTION, JLSECT_dacs,        0 },
    { "Routing",      nullptr, CAT_SECTION, JLSECT_routing,     0 },
    { "Slots",        nullptr, CAT_SECTION, JLSECT_slots,       0 },
    { "Logo Pads",    nullptr, CAT_SECTION, JLSECT_logo_pads,   0 },
    { "OLED",         nullptr, CAT_SECTION, JLSECT_top_oled,    0 },
    { "Serial 1",     nullptr, CAT_SECTION, JLSECT_serial_1,    0 },
    { "Serial 2",     nullptr, CAT_SECTION, JLSECT_serial_2,    0 },
    { "USB CDC",      nullptr, CAT_SECTION, JLSECT_usb_cdc,     0 },
    { "USB Audio",    nullptr, CAT_SECTION, JLSECT_usb_audio,   0 },
    { "Debug",        nullptr, CAT_SECTION, JLSECT_debug,       0 },
    { "Calibration",  nullptr, CAT_SECTION, JLSECT_calibration, 0 },
    { "Hardware",     nullptr, CAT_SECTION, JLSECT_hardware,    0 },
    { "Menu FX Tuner","Open the live frame-transition tuner (drives the real breadboard menu).", CAT_MENUFX, -1, 0 },
    { "Reset to defaults", "Reset every option to its default, keeping calibration and hardware identity.", CAT_RESET, -1, 0 },
    { "Exit",         "Close the config menu (left arrow and q work too).", CAT_EXIT, -1, 0 },
};
static const int kCategoryCount = (int)(sizeof(kCategories) / sizeof(kCategories[0]));

static const char* categoryDesc(const Category& cat) {
    if (cat.desc) return cat.desc;
    if (cat.action == CAT_SECTION) return jlConfigSections[cat.section].desc;
    return "";
}

// ---------------------------------------------------------------------------
// Option index for a category (section members + cross-listed debug flags)
// ---------------------------------------------------------------------------
static int buildOptionList(const Category& cat, int16_t* out, int maxOut) {
    int n = 0;
    for (int i = 0; i < jlConfigOptionCount && n < maxOut; i++) {
        const ConfigOptionDesc* o = &jlConfigOptions[i];
        if (o->flags & JLC_HIDDEN) continue;
        if (cat.action == CAT_ALL) { out[n++] = (int16_t)i; continue; }
        if (cat.action != CAT_SECTION) continue;
        if (o->section == cat.section) { out[n++] = (int16_t)i; continue; }
        if (cat.extraFlag && (o->flags & cat.extraFlag)) { out[n++] = (int16_t)i; continue; }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Enum cycling: step to the next/prev DISTINCT value in the option's table
// (tables carry aliases, so walk unique values in first-appearance order).
// ---------------------------------------------------------------------------
static int enumStep(const ConfigOptionDesc* o, int current, int dir) {
    // Sized for the biggest table (arbitraryFunctionTable: ~50 distinct
    // values). A smaller cap here silently made everything past it
    // unreachable by cycling - isense_pos/isense- on the logo pads could be
    // left but never re-entered.
    int uniq[80];
    int nu = 0;
    for (int i = 0; i < o->table.n && nu < 80; i++) {
        bool seen = false;
        for (int j = 0; j < nu; j++) if (uniq[j] == o->table.t[i].value) { seen = true; break; }
        if (!seen) uniq[nu++] = o->table.t[i].value;
    }
    if (nu == 0) return current + dir;
    int at = -1;
    for (int i = 0; i < nu; i++) if (uniq[i] == current) { at = i; break; }
    if (at < 0) return uniq[0];  // current isn't a named value - snap to the first
    at = (at + dir + nu) % nu;
    return uniq[at];
}

// Apply a new numeric/enum value through the same path paste edits use, so
// hooks + LED refresh triggers fire. Debounced flash save.
static void applyValue(const ConfigOptionDesc* o, const char* valueStr) {
    if (configSetValue(o, valueStr, /*liveApply=*/true)) {
        configChanged = true;
        requestConfigSave();
    }
}

static void stepOption(const ConfigOptionDesc* o, int dir) {
    char buf[64];
    switch (o->type) {
        case JLT_BOOL:
            snprintf(buf, sizeof(buf), "%d", (*(bool*)o->ptr) ? 0 : 1);
            break;
        case JLT_FLOAT: {
            float step = (o->step > 0.0f) ? o->step : 0.01f;
            snprintf(buf, sizeof(buf), "%.4f", (double)(*(float*)o->ptr + dir * step));
            break;
        }
        case JLT_FONT: {
            int v = *(int*)o->ptr + dir;
            snprintf(buf, sizeof(buf), "%d", v < 0 ? 0 : v);
            break;
        }
        case JLT_STR16:
        case JLT_STR33:
            return;  // strings edit inline, not by stepping
        default: {
            int cur = (o->type == JLT_VINT) ? *(volatile int*)o->ptr : *(int*)o->ptr;
            int v;
            if (o->table.t != nullptr) v = enumStep(o, cur, dir);
            else v = cur + dir * (o->step >= 1.0f ? (int)o->step : 1);
            snprintf(buf, sizeof(buf), "%d", v);
            break;
        }
    }
    applyValue(o, buf);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
struct Layout {
    int rows, cols;
    int listTop, listBottom;   // inner rows of the left pane
    int leftC1, leftC2;        // left pane box columns
    int rightC1, rightC2;      // right pane box columns
    int innerLeftW;
    int innerRightW;
};

static Layout computeLayout(const Session& s) {
    Layout L;
    L.rows = s.rows();
    L.cols = s.cols();
    int split = (L.cols * 48) / 100;
    if (split < 34) split = 34;
    if (split > L.cols - 30) split = L.cols - 30;
    L.leftC1 = 1;
    L.leftC2 = split;
    L.rightC1 = split + 1;
    L.rightC2 = L.cols;
    L.listTop = 3;
    L.listBottom = L.rows - 2;
    L.innerLeftW = L.leftC2 - L.leftC1 - 3;
    L.innerRightW = L.rightC2 - L.rightC1 - 3;
    return L;
}

static void drawChrome(const Session& s, const Layout& L, const char* title, const Theme& th) {
    Stream* out = s.out();
    clearScreen(out);
    box(L.listTop - 1, L.leftC1, L.listBottom + 1, L.leftC2, BORDER_ROUNDED, roleColor(th.border, 0), out);
    box(L.listTop - 1, L.rightC1, L.listBottom + 1, L.rightC2, BORDER_ROUNDED, roleColor(th.border, 3), out);
    // Title in the top border
    at(L.listTop - 1, L.leftC1 + 2, out);
    fg(roleColor(th.title, 1), out);
    bold(true, out);
    out->print(" ");
    printClipped(title, L.leftC2 - L.leftC1 - 6, out);
    out->print(" ");
    bold(false, out);
    // Header row
    at(1, 2, out);
    fg(roleColor(th.title, 2), out);
    bold(true, out);
    out->print("Jumperless Config");
    bold(false, out);
    resetColor(out);
}

static void drawFooter(const Session& s, const Layout& L, const char* hint, const Theme& th) {
    Stream* out = s.out();
    at(L.rows, 2, out);
    eraseToEnd(out);
    fg(th.hint, out);
    printClipped(hint, L.cols - 3, out);
    resetColor(out);
}

// Right pane: description + type info for the highlighted option.
static void drawOptionInfo(const Session& s, const Layout& L, const ConfigOptionDesc* o, const Theme& th) {
    Stream* out = s.out();
    int r = L.listTop;
    int w = L.innerRightW;
    // clear the pane interior
    for (int row = L.listTop; row <= L.listBottom; row++) {
        at(row, L.rightC1 + 1, out);
        for (int c = 0; c < w + 2; c++) out->print(' ');
        pump();  // pane clears outrun the CDC TX FIFO without this
    }
    if (!o) {
        // The "< Back" row is selected.
        at(L.listTop, L.rightC1 + 2, out);
        fg(th.hint, out);
        printClipped("Return to the category list.", w, out);
        resetColor(out);
        out->flush();
        return;
    }

    at(r, L.rightC1 + 2, out);
    fg(roleColor(th.category, 5), out);
    bold(true, out);
    // Clip the WHOLE header - an unclipped "[measurement] crosspoint_..."
    // painted past the border and left droppings outside the box.
    char headBuf[64];
    snprintf(headBuf, sizeof(headBuf), "[%s] %s", jlConfigSections[o->section].name, o->key);
    printClipped(headBuf, w, out);
    bold(false, out);
    r += 2;

    // Wrapped description
    fg(th.desc, out);
    char lineBuf[96];
    for (int li = 0; r <= L.listBottom - 4 && wrapLine(o->desc, w, li, lineBuf, sizeof(lineBuf)); li++, r++) {
        at(r, L.rightC1 + 2, out);
        printClipped(lineBuf, w, out);
        pump();
    }
    r++;

    // Range / allowed values
    fg(th.hint, out);
    if (o->table.t != nullptr && r <= L.listBottom - 2) {
        at(r, L.rightC1 + 2, out);
        out->print("values: ");
        int shown = 0;
        int col = 8;
        for (int i = 0; i < o->table.n && r <= L.listBottom - 2; i++) {
            bool dup = false;
            for (int j = 0; j < i; j++) if (o->table.t[j].value == o->table.t[i].value) { dup = true; break; }
            if (dup) continue;  // aliases: show only the first name per value
            int nameLen = (int)strlen(o->table.t[i].name);
            // Separator stays attached to the PREVIOUS name so wrapped lines
            // never start with a dangling ", ".
            if (shown && col + 2 + nameLen > w) {
                out->print(",");
                r++; at(r, L.rightC1 + 4, out); col = 2;
            } else if (shown) {
                out->print(", ");
                col += 2;
            }
            printClipped(o->table.t[i].name, w - col + 2, out);
            col += nameLen;
            shown++;
            if ((shown & 7) == 0) pump();
        }
        r++;
    } else if ((o->minv != 0.0f || o->maxv != 0.0f) && r <= L.listBottom - 1) {
        at(r, L.rightC1 + 2, out);
        char rangeBuf[48];
        if (o->type == JLT_FLOAT) snprintf(rangeBuf, sizeof(rangeBuf), "range: %.2f to %.2f", (double)o->minv, (double)o->maxv);
        else snprintf(rangeBuf, sizeof(rangeBuf), "range: %d to %d", (int)o->minv, (int)o->maxv);
        printClipped(rangeBuf, w, out);
        r++;
    }

    // Default + boot-only / calibration notes (all clipped to the pane)
    if (r <= L.listBottom) {
        at(r, L.rightC1 + 2, out);
        printClipped(configOptionIsDefault(o) ? "default: (current)"
                                              : "default: press d to restore", w, out);
        r++;
    }
    if ((o->flags & JLC_BOOT_ONLY) && r <= L.listBottom) {
        at(r, L.rightC1 + 2, out);
        fg(214, out);
        printClipped("takes effect on next boot", w, out);
        r++;
    }
    if ((o->flags & JLC_CAL) && r <= L.listBottom) {
        at(r, L.rightC1 + 2, out);
        fg(114, out);
        printClipped("calibration: survives resets + updates", w, out);
        r++;
    }
    resetColor(out);
    out->flush();
}

static void drawCategoryInfo(const Session& s, const Layout& L, const Category& cat, const Theme& th) {
    Stream* out = s.out();
    int w = L.innerRightW;
    for (int row = L.listTop; row <= L.listBottom; row++) {
        at(row, L.rightC1 + 1, out);
        for (int c = 0; c < w + 2; c++) out->print(' ');
        pump();  // pane clears outrun the CDC TX FIFO without this
    }
    int r = L.listTop;
    at(r, L.rightC1 + 2, out);
    fg(roleColor(th.category, 5), out);
    bold(true, out);
    printClipped(cat.title, w, out);
    bold(false, out);
    r += 2;
    fg(th.desc, out);
    char lineBuf[96];
    for (int li = 0; r <= L.listBottom && wrapLine(categoryDesc(cat), w, li, lineBuf, sizeof(lineBuf)); li++, r++) {
        at(r, L.rightC1 + 2, out);
        printClipped(lineBuf, w, out);
        pump();
    }
    resetColor(out);
    out->flush();
}

// Left pane rows for the option list: key on the left, value right-aligned
// to the pane edge (no "=" - the columns are the affordance). When the row
// is being EDITED the highlight moves off the row bar and onto the value.
static void drawOptionRow(const Session& s, const Layout& L, const ConfigOptionDesc* o,
                          int row, int rainbowIdx, bool selected, bool editing,
                          bool showSection, const Theme& th) {
    Stream* out = s.out();
    at(row, L.leftC1 + 1, out);
    for (int c = 0; c < L.leftC2 - L.leftC1 - 1; c++) out->print(' ');
    at(row, L.leftC1 + 2, out);

    char valBuf[48];
    configFormatValue(o, valBuf, sizeof(valBuf), true);

    char keyBuf[64];
    if (showSection) snprintf(keyBuf, sizeof(keyBuf), "%s.%s", jlConfigSections[o->section].name, o->key);
    else if (o->section == JLSECT_debug) snprintf(keyBuf, sizeof(keyBuf), "debug.%s", o->key);
    else snprintf(keyBuf, sizeof(keyBuf), "%s", o->key);

    int vlen = (int)strlen(valBuf);
    int keyW = L.innerLeftW - vlen - 2;
    if (keyW < 8) keyW = 8;

    bool bar = selected && !editing;
    if (bar) inverse(true, out);
    fg(bar ? th.selected : roleColor(th.key, rainbowIdx), out);
    printClipped(keyBuf, keyW, out);
    int printed = (int)strlen(keyBuf) < keyW ? (int)strlen(keyBuf) : keyW;
    for (int c = printed; c < keyW + 2; c++) out->print(' ');
    if (selected && editing) { inverse(true, out); bold(true, out); }
    fg(selected ? th.selected : th.value, out);
    printClipped(valBuf, L.innerLeftW - keyW - 2, out);
    if (selected && editing) { bold(false, out); inverse(false, out); }
    if (bar) inverse(false, out);
    resetColor(out);
}

// Row 0 of every options screen: the way back for terminals whose ESC never
// arrives (the desktop app eats it).
static void drawBackRow(const Session& s, const Layout& L, int row, bool selected, const Theme& th) {
    Stream* out = s.out();
    at(row, L.leftC1 + 1, out);
    for (int c = 0; c < L.leftC2 - L.leftC1 - 1; c++) out->print(' ');
    at(row, L.leftC1 + 2, out);
    if (selected) inverse(true, out);
    fg(selected ? th.selected : th.hint, out);
    bold(true, out);
    printClipped("< Back", L.innerLeftW, out);
    if (selected) {
        for (int c = 6; c < L.innerLeftW; c++) out->print(' ');
        inverse(false, out);
    }
    bold(false, out);
    resetColor(out);
}

static void drawCategoryRow(const Session& s, const Layout& L, const Category& cat,
                            int row, int rainbowIdx, bool selected, const Theme& th) {
    Stream* out = s.out();
    at(row, L.leftC1 + 1, out);
    for (int c = 0; c < L.leftC2 - L.leftC1 - 1; c++) out->print(' ');
    at(row, L.leftC1 + 2, out);
    if (selected) inverse(true, out);
    fg(selected ? th.selected : roleColor(th.category, rainbowIdx), out);
    bool isAction = (cat.action == CAT_MENUFX || cat.action == CAT_RESET);
    if (isAction) bold(true, out);
    printClipped(cat.title, L.innerLeftW, out);
    if (selected) {
        for (int c = strlen(cat.title); c < L.innerLeftW; c++) out->print(' ');
        inverse(false, out);
    }
    if (isAction) bold(false, out);
    resetColor(out);
}

// ---------------------------------------------------------------------------
// Inline value entry (numbers, hex, strings) on the footer row
// ---------------------------------------------------------------------------
static bool inlineEdit(Session& s, const Layout& L, const ConfigOptionDesc* o, const Theme& th) {
    Stream* out = s.out();
    char buf[48];
    configFormatValue(o, buf, sizeof(buf), true);
    int len = strlen(buf);

    at(L.rows, 2, out);
    eraseToEnd(out);
    fg(roleColor(th.category, 7), out);
    out->print(o->key);
    out->print(" = ");
    fg(th.value, out);
    out->print(buf);
    cursorShow(out);
    out->flush();

    for (;;) {
        Key k = readKey(s.in());
        if (k == KEY_NONE) { Tui::idle(); continue; }
        if (k == KEY_ENTER) {
            cursorHide(out);
            buf[len] = '\0';
            applyValue(o, buf);
            return true;
        }
        if (k == KEY_ESC) {
            cursorHide(out);
            return false;
        }
        if (k == KEY_BACKSPACE) {
            if (len > 0) { len--; out->print("\b \b"); out->flush(); }
            continue;
        }
        if (k == KEY_CHAR && len < (int)sizeof(buf) - 1) {
            buf[len++] = (char)lastChar();
            out->write((char)lastChar());
            out->flush();
        }
    }
}

// ---------------------------------------------------------------------------
// The options screen for one category. Returns when the user backs out.
//
// Row 0 is "< Back". Navigation model (the app never delivers ESC, so left
// is the back key): up/down move, LEFT = back, ENTER (or RIGHT) selects an
// option for editing - the value highlights, LEFT/RIGHT change it live,
// ENTER commits and drops back to navigation. Strings and exact numeric
// entry go through the inline line editor ('e' while editing, or ENTER on
// a string option).
// ---------------------------------------------------------------------------
static void runOptionsScreen(Session& s, const Category& cat, const Theme& th) {
    int16_t idx[192];
    int n = buildOptionList(cat, idx, 192);

    Layout L = computeLayout(s);
    ListView lv;
    lv.count = n + 1;  // +1: the "< Back" row at index 0
    lv.visible = L.listBottom - L.listTop + 1;
    lv.clamp();

    bool full = true;
    bool editing = false;
    int lastSelected = -1;
    bool lastEditing = false;
    bool showSection = (cat.action == CAT_ALL);

    auto optionAt = [&](int sel) -> const ConfigOptionDesc* {
        return (sel >= 1 && sel <= n) ? &jlConfigOptions[idx[sel - 1]] : nullptr;
    };

    for (;;) {
        if (full) {
            drawChrome(s, L, cat.title, th);
        }
        if (full || lv.selected != lastSelected || editing != lastEditing) {
            drawFooter(s, L, editing
                ? "left/right change value   e type exact value   d default   enter done"
                : "up/down move   enter change value   left back   d default", th);
            for (int i = lv.top; i < lv.top + lv.visible && i < lv.count; i++) {
                int row = L.listTop + (i - lv.top);
                if (i == 0) drawBackRow(s, L, row, lv.selected == 0, th);
                else drawOptionRow(s, L, optionAt(i), row, i,
                                   i == lv.selected, editing && i == lv.selected,
                                   showSection, th);
                pump();
            }
            drawOptionInfo(s, L, optionAt(lv.selected), th);
            lastSelected = lv.selected;
            lastEditing = editing;
            full = false;
            s.out()->flush();
        }

        Key k = readKey(s.in());
        if (k == KEY_NONE) { Tui::idle(); continue; }
        const ConfigOptionDesc* o = optionAt(lv.selected);

        if (editing) {
            // --- edit mode: the highlighted VALUE is live ---
            switch (k) {
                case KEY_LEFT:  if (o) { stepOption(o, -1); lastSelected = -1; } break;
                case KEY_RIGHT: if (o) { stepOption(o, +1); lastSelected = -1; } break;
                case KEY_ENTER:
                case KEY_ESC:
                    editing = false;
                    break;
                case KEY_UP:    { editing = false; int t = lv.top; lv.move(-1); if (lv.top != t) full = true; break; }
                case KEY_DOWN:  { editing = false; int t = lv.top; lv.move(+1); if (lv.top != t) full = true; break; }
                case KEY_CHAR:
                    if (lastChar() == 'e' && o) {
                        inlineEdit(s, L, o, th);
                        editing = false;
                        full = true;
                    } else if (lastChar() == 'd' && o) {
                        configResetOptionToDefault(o, /*liveApply=*/true);
                        configChanged = true;
                        requestConfigSave();
                        lastSelected = -1;
                    } else if (lastChar() == 'q') {
                        return;
                    }
                    break;
                default: break;
            }
            continue;
        }

        // --- navigation mode ---
        switch (k) {
            case KEY_UP:    { int t = lv.top; lv.move(-1); if (lv.top != t) full = true; break; }
            case KEY_DOWN:  { int t = lv.top; lv.move(+1); if (lv.top != t) full = true; break; }
            case KEY_PGUP:  { lv.page(-1); full = true; break; }
            case KEY_PGDN:  { lv.page(+1); full = true; break; }
            case KEY_HOME:  { lv.home(); full = true; break; }
            case KEY_END:   { lv.end(); full = true; break; }
            case KEY_LEFT:
            case KEY_ESC:
                return;  // back to categories (left = back; the app eats ESC)
            case KEY_ENTER:
            case KEY_RIGHT:
                if (lv.selected == 0) return;  // "< Back"
                if (!o) break;
                if (o->type == JLT_STR16 || o->type == JLT_STR33) {
                    inlineEdit(s, L, o, th);   // strings: straight to typing
                    full = true;
                } else {
                    editing = true;            // highlight the value; arrows change it
                }
                break;
            case KEY_CHAR:
                if (lastChar() == 'q') return;
                if (lastChar() == 'd' && o) {
                    configResetOptionToDefault(o, /*liveApply=*/true);
                    configChanged = true;
                    requestConfigSave();
                    lastSelected = -1;
                }
                if (lastChar() == 'j') { int t = lv.top; lv.move(+1); if (lv.top != t) full = true; }
                if (lastChar() == 'k') { int t = lv.top; lv.move(-1); if (lv.top != t) full = true; }
                break;
            default:
                break;
        }
    }
}

static bool confirmReset(Session& s, const Layout& L, const Theme& th) {
    drawFooter(s, L, "Reset all settings to defaults? Calibration and hardware are kept.  y = yes, any other key = no", th);
    s.out()->flush();
    for (;;) {
        Key k = readKey(s.in());
        if (k == KEY_NONE) { Tui::idle(); continue; }
        return (k == KEY_CHAR && (lastChar() == 'y' || lastChar() == 'Y'));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point (called from readConfigFromSerial on a bare `)
// ---------------------------------------------------------------------------
void configTuiRun(void) {
    Tui::Session s;
    s.begin();

    Tui::Theme th;  // rainbow defaults; assign fixed colors here to override

    Tui::ListView lv;
    lv.count = kCategoryCount;

    bool full = true;
    int lastSelected = -1;

    for (;;) {
        Layout L = computeLayout(s);
        lv.visible = L.listBottom - L.listTop + 1;
        lv.clamp();

        if (full) {
            drawChrome(s, L, "Categories", th);
            drawFooter(s, L, "up/down move   enter open   left/q exit", th);
        }
        if (full || lv.selected != lastSelected) {
            for (int i = lv.top; i < lv.top + lv.visible && i < lv.count; i++) {
                drawCategoryRow(s, L, kCategories[i], L.listTop + (i - lv.top), i, i == lv.selected, th);
                pump();
            }
            drawCategoryInfo(s, L, kCategories[lv.selected], th);
            lastSelected = lv.selected;
            full = false;
            s.out()->flush();
        }

        Tui::Key k = Tui::readKey(s.in());
        if (k == Tui::KEY_NONE) { Tui::idle(); continue; }

        switch (k) {
            case Tui::KEY_UP:   { int t = lv.top; lv.move(-1); if (lv.top != t) full = true; break; }
            case Tui::KEY_DOWN: { int t = lv.top; lv.move(+1); if (lv.top != t) full = true; break; }
            case Tui::KEY_HOME: lv.home(); full = true; break;
            case Tui::KEY_END:  lv.end(); full = true; break;
            case Tui::KEY_ENTER:
            case Tui::KEY_RIGHT: {
                const Category& cat = kCategories[lv.selected];
                if (cat.action == CAT_MENUFX) {
                    // The tuner owns the whole terminal + breadboard; leave
                    // the alternate screen so its output lands normally.
                    // (The tuner persists its own changes on exit.)
                    s.end();
                    action_menuTransitionTuner();
                    // Scrub the NORMAL screen before re-entering the alt
                    // screen: whatever is on it now (tuner panel + menu echo)
                    // is what the user lands on when they finally exit the
                    // TUI, so leave it clean.
                    s.out()->print("\x1b[2J\x1b[H");
                    Tui::drainInput(s.in());
                    s.begin();
                    full = true;
                    lastSelected = -1;
                } else if (cat.action == CAT_RESET) {
                    if (confirmReset(s, L, th)) {
                        resetConfigToDefaults(0, 0);
                        saveConfig();
                        drawFooter(s, L, "Settings reset to defaults (calibration + hardware kept).", th);
                        delay(900);
                    }
                    full = true;
                    lastSelected = -1;
                } else if (cat.action == CAT_EXIT) {
                    s.end();
                    return;
                } else {
                    runOptionsScreen(s, cat, th);
                    full = true;
                    lastSelected = -1;
                }
                break;
            }
            case Tui::KEY_CHAR:
                if (Tui::lastChar() == 'q') { s.end(); return; }
                if (Tui::lastChar() == 'j') { int t = lv.top; lv.move(+1); if (lv.top != t) full = true; }
                if (Tui::lastChar() == 'k') { int t = lv.top; lv.move(-1); if (lv.top != t) full = true; }
                break;
            case Tui::KEY_LEFT:   // left = back; at the top level back = exit
            case Tui::KEY_ESC:
                s.end();
                return;
            default:
                break;
        }
    }
}
