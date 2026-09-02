// Reusable serial-terminal UI primitives. See Tui.h for the tour.

#include "Tui.h"
#include "Graphics.h"   // highSaturationSpectrumColors + disableTerminalColors
#include "Jerial.h"     // setTerminalLineBuffering / pushLineBufferingToApp
#include "JumperlOS.h"  // jOS.serviceInner() - the modal-loop service contract

namespace Tui {

static Stream* defOut() { return &Serial; }
static Stream* defIn()  { return &Serial; }

void idle() {
    // The inner set carries TinyUSB, ProbeButton, MpRemote, Peripherals and
    // AsyncPassthrough; delay(1) additionally yields to arduino-pico's
    // background hooks (delayMicroseconds does NOT).
    jOS.serviceInner();
    delay(1);
}

void pump() {
    jOS.serviceInner();
}

// ---------------------------------------------------------------------------
// Key decoder
// ---------------------------------------------------------------------------
static int s_lastChar = -1;
int lastChar() { return s_lastChar; }

void drainInput(Stream* in) {
    if (!in) in = defIn();
    while (in->available()) in->read();
}

// USB CDC likes to split ESC sequences across packets; wait a tolerant beat
// for the continuation bytes (the File Manager measured ~25 ms worst case).
static int readByteWait(Stream* in, uint32_t waitMs) {
    if (in->available()) return in->read();
    if (waitMs == 0) return -1;
    uint32_t start = millis();
    while ((millis() - start) < waitMs) {
        if (in->available()) return in->read();
        idle();  // keep USB alive while waiting for split escape sequences
    }
    return -1;
}

Key readKey(Stream* in) {
    if (!in) in = defIn();
    if (!in->available()) return KEY_NONE;

    int c = in->read();

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 0x7F || c == 0x08) return KEY_BACKSPACE;

    if (c != 0x1b) {
        if (c >= 32 && c <= 126) { s_lastChar = c; return KEY_CHAR; }
        return KEY_NONE;
    }

    // ESC ... : decode CSI / SS3, or report a bare ESC press.
    int b1 = readByteWait(in, 25);
    if (b1 < 0) return KEY_ESC;

    if (b1 == 'O') {  // SS3 (application cursor keys)
        int b2 = readByteWait(in, 25);
        switch (b2) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            default:  return KEY_NONE;
        }
    }
    if (b1 != '[') {
        // ESC + printable (Alt+key): deliver the key instead of eating it as ESC
        if (b1 >= 32 && b1 <= 126) { s_lastChar = b1; return KEY_CHAR; }
        s_lastChar = b1; return KEY_ESC;
    }

    // CSI: collect parameter bytes up to the final letter/tilde.
    char params[8];
    uint8_t n = 0;
    int fin = -1;
    for (;;) {
        int b = readByteWait(in, 25);
        if (b < 0) return KEY_NONE;
        if ((b >= '0' && b <= '9') || b == ';') {
            if (n < sizeof(params) - 1) params[n++] = (char)b;
            continue;
        }
        fin = b;
        break;
    }
    params[n] = '\0';

    switch (fin) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'R': return KEY_NONE;   // stray cursor-position report - swallow
        case '~':
            switch (atoi(params)) {
                case 1: case 7: return KEY_HOME;
                case 4: case 8: return KEY_END;
                case 5: return KEY_PGUP;
                case 6: return KEY_PGDN;
                case 3: return KEY_BACKSPACE;  // Delete: same intent in menus
                default: return KEY_NONE;
            }
        default: return KEY_NONE;
    }
}

// ---------------------------------------------------------------------------
// Terminal size probe (CSI 6n cursor-position report)
// ---------------------------------------------------------------------------
bool probeTerminalSize(int& rows, int& cols, Stream* out, Stream* in) {
    if (!out) out = defOut();
    if (!in) in = defIn();

    drainInput(in);
    out->print("\x1b[s");            // save cursor
    out->print("\x1b[9999;9999H");   // slam to bottom-right
    out->print("\x1b[6n");           // ask where we ended up
    out->flush();

    char buf[24];
    size_t n = 0;
    uint32_t t0 = millis();
    bool sawEsc = false;
    while ((millis() - t0) < 400 && n < sizeof(buf) - 1) {
        if (!in->available()) { idle(); continue; }
        char ch = (char)in->read();
        if (!sawEsc) {
            if (ch != '\x1b') continue;
            sawEsc = true;
        }
        buf[n++] = ch;
        if (ch == 'R') break;
    }
    buf[n] = '\0';
    out->print("\x1b[u");            // restore cursor
    out->flush();
    drainInput(in);

    int r = 0, c = 0;
    if (sscanf(buf, "\x1b[%d;%dR", &r, &c) == 2 && r > 0 && c > 0) {
        rows = r;
        cols = c;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
bool Session::begin(Stream* out, Stream* in, int maxRows, int maxCols) {
    out_ = out ? out : defOut();
    in_ = in ? in : defIn();

    // Raw keys from the companion app (SO byte); remember what to restore.
    prevLineBuffering_ = setTerminalLineBuffering(true);
    delay(10);

    // Fixed-size layout: start at the cap, and only let a SUCCESSFUL probe
    // shrink it (a terminal that answers CSI 6n and is smaller than the cap).
    // Terminals that never answer - the desktop app - get the cap instead of
    // an arbitrary fallback, so the menu doesn't flip between huge and tiny
    // depending on whether the probe reply arrived.
    rows_ = maxRows;
    cols_ = maxCols;
    int probedRows = 0, probedCols = 0;
    if (probeTerminalSize(probedRows, probedCols, out_, in_)) {
        if (probedRows < rows_) rows_ = probedRows;
        if (probedCols < cols_) cols_ = probedCols;
    }
    if (rows_ < 12) rows_ = 12;
    if (cols_ < 40) cols_ = 40;

    out_->print("\x1b[?1049h");  // alternate screen - scrollback survives us
    cursorHide(out_);
    clearScreen(out_);
    active_ = true;
    return true;
}

void Session::end() {
    if (!active_) return;
    resetColor(out_);
    out_->print("\x1b[?1049l");  // back to the normal screen buffer
    cursorShow(out_);
    out_->flush();
    setTerminalLineBuffering(prevLineBuffering_);
    active_ = false;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void clearScreen(Stream* out) { if (!out) out = defOut(); out->print("\x1b[2J\x1b[H"); }
void at(int row, int col, Stream* out) {
    if (!out) out = defOut();
    char b[16];
    snprintf(b, sizeof(b), "\x1b[%d;%dH", row, col);
    out->print(b);
}
void eraseLine(Stream* out)  { if (!out) out = defOut(); out->print("\x1b[2K\r"); }
void eraseToEnd(Stream* out) { if (!out) out = defOut(); out->print("\x1b[0K"); }
void cursorHide(Stream* out) { if (!out) out = defOut(); out->print("\x1b[?25l"); }
void cursorShow(Stream* out) { if (!out) out = defOut(); out->print("\x1b[?25h"); }

void fg(int color256, Stream* out) {
    if (!out) out = defOut();
    if (disableTerminalColors) return;
    if (color256 < 0) { out->print("\x1b[39m"); return; }
    char b[16];
    snprintf(b, sizeof(b), "\x1b[38;5;%dm", color256);
    out->print(b);
}
void bg(int color256, Stream* out) {
    if (!out) out = defOut();
    if (disableTerminalColors) return;
    if (color256 < 0) { out->print("\x1b[49m"); return; }
    char b[16];
    snprintf(b, sizeof(b), "\x1b[48;5;%dm", color256);
    out->print(b);
}
void resetColor(Stream* out) { if (!out) out = defOut(); out->print("\x1b[0m"); }
void bold(bool on, Stream* out)    { if (!out) out = defOut(); if (!disableTerminalColors) out->print(on ? "\x1b[1m" : "\x1b[22m"); }
void dim(bool on, Stream* out)     { if (!out) out = defOut(); if (!disableTerminalColors) out->print(on ? "\x1b[2m" : "\x1b[22m"); }
void inverse(bool on, Stream* out) { if (!out) out = defOut(); out->print(on ? "\x1b[7m" : "\x1b[27m"); }

struct BorderGlyphs { const char *h, *v, *tl, *tr, *bl, *br; };
static BorderGlyphs glyphsFor(BorderStyle s) {
    switch (s) {
        case BORDER_ROUNDED: return { "─", "│", "╭", "╮", "╰", "╯" };
        case BORDER_HEAVY:   return { "━", "┃", "┏", "┓", "┗", "┛" };
        case BORDER_DOUBLE:  return { "═", "║", "╔", "╗", "╚", "╝" };
        case BORDER_ASCII:   return { "-", "|", "+", "+", "+", "+" };
        case BORDER_LIGHT:
        default:             return { "─", "│", "┌", "┐", "└", "┘" };
    }
}

void box(int r1, int c1, int r2, int c2, BorderStyle style, int color256, Stream* out) {
    if (!out) out = defOut();
    BorderGlyphs g = glyphsFor(style);
    if (color256 >= 0) fg(color256, out);
    at(r1, c1, out); out->print(g.tl);
    for (int c = c1 + 1; c < c2; c++) out->print(g.h);
    out->print(g.tr);
    pump();
    for (int r = r1 + 1; r < r2; r++) {
        at(r, c1, out); out->print(g.v);
        at(r, c2, out); out->print(g.v);
        if ((r & 3) == 0) pump();  // keep the CDC TX FIFO drained mid-paint
    }
    at(r2, c1, out); out->print(g.bl);
    for (int c = c1 + 1; c < c2; c++) out->print(g.h);
    out->print(g.br);
    if (color256 >= 0) fg(-1, out);
    pump();
}

void hline(int row, int c1, int c2, BorderStyle style, int color256, Stream* out) {
    if (!out) out = defOut();
    BorderGlyphs g = glyphsFor(style);
    if (color256 >= 0) fg(color256, out);
    at(row, c1, out);
    for (int c = c1; c <= c2; c++) out->print(g.h);
    if (color256 >= 0) fg(-1, out);
}

void printClipped(const char* text, int maxWidth, Stream* out) {
    if (!out) out = defOut();
    int shown = 0;
    for (const char* p = text; *p && shown < maxWidth; ) {
        // UTF-8: one visible column per code point (good enough for the
        // box-drawing + text set this firmware ships)
        uint8_t b = (uint8_t)*p;
        int len = (b < 0x80) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
        for (int i = 0; i < len && *p; i++) out->write(*p++);
        shown++;
    }
}

// ---------------------------------------------------------------------------
// Theme / rainbow
// ---------------------------------------------------------------------------
int rainbowColor(int index) {
    if (index < 0) index = -index;
    // The BRIGHT high-sat palette only (same set the ~ printout cycles) -
    // the full spectrum array dips through dim/muddy entries that read badly
    // as UI chrome.
    return highSaturationBrightColors[index % highSaturationBrightColorsCount];
}
int roleColor(int role, int index) {
    return (role == RAINBOW) ? rainbowColor(index) : role;
}

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------
void ListView::clamp() {
    if (count <= 0) { selected = 0; top = 0; return; }
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    if (selected < top) top = selected;
    if (selected >= top + visible) top = selected - visible + 1;
    if (top < 0) top = 0;
    if (top > count - 1) top = count - 1;
}

void ListView::move(int delta) {
    if (count <= 0) return;
    int prev = selected;
    selected += delta;
    // wrap around the ends
    if (selected < 0) selected = count - 1;
    if (selected >= count) selected = 0;
    clamp();
    if (selected != prev && onSelectionChanged) onSelectionChanged(ctx, selected);
}

void ListView::page(int dir) {
    if (count <= 0) return;
    int prev = selected;
    selected += dir * visible;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    clamp();
    if (selected != prev && onSelectionChanged) onSelectionChanged(ctx, selected);
}

void ListView::home() {
    int prev = selected;
    selected = 0;
    clamp();
    if (selected != prev && onSelectionChanged) onSelectionChanged(ctx, selected);
}

void ListView::end() {
    int prev = selected;
    selected = count > 0 ? count - 1 : 0;
    clamp();
    if (selected != prev && onSelectionChanged) onSelectionChanged(ctx, selected);
}

// ---------------------------------------------------------------------------
// Word wrap
// ---------------------------------------------------------------------------
bool wrapLine(const char* text, int width, int lineIndex, char* out, int outLen) {
    if (width < 1 || !text) return false;
    const char* p = text;
    for (int line = 0; ; line++) {
        while (*p == ' ') p++;                 // eat leading spaces per line
        if (*p == '\0') return false;          // ran out before lineIndex
        // find how much fits: last space within width, or hard cut
        int len = 0;
        int lastSpace = -1;
        while (p[len] && len < width) {
            if (p[len] == ' ') lastSpace = len;
            len++;
        }
        int take = len;
        if (p[len] != '\0' && lastSpace > 0) take = lastSpace;  // soft break
        if (line == lineIndex) {
            int n = take < outLen - 1 ? take : outLen - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        p += take;
    }
}

} // namespace Tui
