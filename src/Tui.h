// ============================================================================
// Tui - reusable serial-terminal UI primitives for JumperlOS.
//
// One place for the things every interactive screen used to reimplement:
//   - a session bracket (alternate screen, cursor hide, raw-key mode from the
//     companion app, full restore on exit)
//   - ONE escape-sequence key decoder (arrows, Enter, ESC, Home/End, PgUp/Dn)
//     with the USB-CDC split-packet tolerance the File Manager learned the
//     hard way
//   - cursor/box/color drawing helpers on top of the xterm CSI set
//   - a Theme where every role is either a fixed 256-color or RAINBOW, which
//     steps the same highSaturationBrightColors palette the ~ config
//     printout uses
//   - scroll/selection math for lists, word wrap for description panes
//
// Composition pattern for live "triggers": give your item model onChange /
// onFocus callbacks and call them from your key loop - the config TUI
// (ConfigTui.cpp) is the reference consumer. The framework stays ignorant of
// what the items mean.
// ============================================================================
#pragma once

#include <Arduino.h>

namespace Tui {

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
enum Key : uint8_t {
    KEY_NONE = 0,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_CHAR,   // printable - fetch it with lastChar()
};

// Decode one key from the stream (non-blocking; returns KEY_NONE when idle).
// Handles ESC [ A/B/C/D, ESC O A/B, ESC [ 1~/4~/5~/6~/H/F, bare ESC, and
// swallows stray CPR responses (ESC [ r;c R) so a size probe can't leak
// garbage into navigation.
Key readKey(Stream* in = nullptr);
int lastChar();   // the printable char behind the most recent KEY_CHAR

// Call this in EVERY key-wait loop iteration. Pumps jOS.serviceInner() -
// the inner service set includes the mutex-guarded TinyUSB pump, so a modal
// loop that skips it starves USB CDC and the terminal goes silent (the same
// contract probeMode() and the click menus honor).
void idle();

// The draw-path version: pump the inner set WITHOUT the delay. Full-screen
// paints are thousands of bytes - more than the CDC TX FIFO - and Serial
// writes block once it fills, so anything painting more than a row or two
// must pump between rows or the whole board freezes mid-paint (bench-
// observed: output stops partway through a pane clear, forever).
void pump();

// Drain any pending input (call before waiting for a fresh keypress).
void drainInput(Stream* in = nullptr);

// ---------------------------------------------------------------------------
// Session bracket
// ---------------------------------------------------------------------------
// begin(): probe the terminal size, switch to the alternate screen buffer
// (CSI ?1049h - the user's scrollback comes back intact on exit), hide the
// cursor, clear, and put the companion app in raw-key mode.
// end(): undo all of it, restoring the line-buffering state we found.
//
// maxRows/maxCols CAP the layout so screens render the same size everywhere:
// the size probe only succeeds on terminals that answer CSI 6n, and a layout
// that swings between "whatever the probe said" and the fallback feels
// broken. The probe still SHRINKS the layout for genuinely small terminals.
class Session {
public:
    bool begin(Stream* out = nullptr, Stream* in = nullptr,
               int maxRows = 32, int maxCols = 84);
    void end();
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    Stream* out() const { return out_; }
    Stream* in() const { return in_; }

private:
    Stream* out_ = nullptr;
    Stream* in_ = nullptr;
    int rows_ = 24;
    int cols_ = 80;
    bool prevLineBuffering_ = true;
    bool active_ = false;
};

// Terminal size via cursor-position report (move to 9999;9999, CSI 6n).
// Returns false (and leaves rows/cols untouched) when the terminal never
// answers - callers keep their 80x24 default.
bool probeTerminalSize(int& rows, int& cols, Stream* out = nullptr, Stream* in = nullptr);

// ---------------------------------------------------------------------------
// Drawing (all write to `out`, default Serial; colors respect
// disableTerminalColors via changeTerminalColor's global)
// ---------------------------------------------------------------------------
void clearScreen(Stream* out = nullptr);
void at(int row, int col, Stream* out = nullptr);
void eraseLine(Stream* out = nullptr);        // CSI 2K + CR
void eraseToEnd(Stream* out = nullptr);       // CSI 0K
void cursorHide(Stream* out = nullptr);
void cursorShow(Stream* out = nullptr);

void fg(int color256, Stream* out = nullptr); // -1 = reset
void bg(int color256, Stream* out = nullptr);
void resetColor(Stream* out = nullptr);
void bold(bool on, Stream* out = nullptr);
void dim(bool on, Stream* out = nullptr);
void inverse(bool on, Stream* out = nullptr);

enum BorderStyle : uint8_t { BORDER_LIGHT, BORDER_ROUNDED, BORDER_HEAVY, BORDER_DOUBLE, BORDER_ASCII };
void box(int r1, int c1, int r2, int c2, BorderStyle style = BORDER_ROUNDED,
         int color256 = -1, Stream* out = nullptr);
void hline(int row, int c1, int c2, BorderStyle style = BORDER_LIGHT,
           int color256 = -1, Stream* out = nullptr);

// Print at most maxWidth visible characters (UTF-8 aware enough for the
// box-drawing set: counts code points, not bytes).
void printClipped(const char* text, int maxWidth, Stream* out = nullptr);

// ---------------------------------------------------------------------------
// Theme - every role is a 256-color or RAINBOW
// ---------------------------------------------------------------------------
constexpr int RAINBOW = -2;   // step the spectrum palette per item
constexpr int DEFAULT_COLOR = -1;

struct Theme {
    int border   = RAINBOW;
    int title    = RAINBOW;
    int category = RAINBOW;
    int key      = RAINBOW;
    int value    = 231;      // near-white values read well against rainbow keys
    int desc     = 250;      // light gray body text
    int hint     = 244;      // dimmer footer hints
    int selected = 231;      // fg used inside the inverse-video selection bar
};

// The palette step for RAINBOW roles: item index -> spectrum color.
int rainbowColor(int index);
// Resolve a theme role for an item: fixed color passes through, RAINBOW
// steps the palette by index.
int roleColor(int role, int index);

// ---------------------------------------------------------------------------
// List selection/scroll math (render however you like)
// ---------------------------------------------------------------------------
struct ListView {
    int count = 0;
    int selected = 0;
    int top = 0;        // first visible index
    int visible = 10;   // rows in the viewport

    // Optional: notified after the selection actually moves (onFocus-style
    // trigger hook; ctx is yours).
    void (*onSelectionChanged)(void* ctx, int selected) = nullptr;
    void* ctx = nullptr;

    void clamp();
    void move(int delta);          // +-1 with wraparound
    void page(int dir);            // +-1 page
    void home();
    void end();
    bool isVisible(int i) const { return i >= top && i < top + visible; }
};

// ---------------------------------------------------------------------------
// Word wrap for description panes. Computes the Nth wrapped line of `text`
// at `width` into out; returns false when past the end.
// ponytail: O(n) per line so O(n^2) per pane - description strings are a
// sentence long, so recompute beats buffering; upgrade path is caching the
// break offsets if panes ever hold documents.
// ---------------------------------------------------------------------------
bool wrapLine(const char* text, int width, int lineIndex, char* out, int outLen);

} // namespace Tui
