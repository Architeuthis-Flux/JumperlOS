#ifndef OLEDGUI_H
#define OLEDGUI_H

// =============================================================================
// OledGui - retained-mode OLED screen system
// =============================================================================
// A lightweight retained scene-graph layered on top of the existing `oled`
// driver. A screen holds a flat, z-ordered list of elements (Text + Shape).
// Text elements may contain {token} templates that are resolved every tick
// against a small variable registry (built-in computed sources + values
// pushed from C++/MicroPython). Rendering and binding refresh are driven by
// OledGuiService (see JumperlOS.h); the immediate-mode toast path
// (clearPrintShowRich) is unchanged.
//
// Design notes:
//  - Elements are fixed-size PODs so a baked screen needs no heap.
//  - Positioning is either absolute (x/y top-left) or anchored
//    (h-align x v-align), so width-adaptive layouts (e.g. toast-style) can be
//    designed and the useful align/font/size values read back out of the JSON.
//  - The whole subsystem is inert until a screen is activated, so it never
//    perturbs existing display behavior on its own.

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

class oled;  // forward decl - the display driver

// ---- Element model ----------------------------------------------------------

enum OledElemType : uint8_t {
    OLED_ELEM_TEXT  = 0,
    OLED_ELEM_SHAPE = 1,
};

enum OledShapeKind : uint8_t {
    OLED_SHAPE_LINE        = 0,  // (x,y) -> (x+w, y+h)
    OLED_SHAPE_RECT        = 1,  // outline w x h at (x,y)
    OLED_SHAPE_FILLED_RECT = 2,  // filled w x h at (x,y)
};

// Per-axis alignment. Used when useAnchor is true; each axis independently
// either snaps to an edge/center or uses the element's absolute x/y (FREE).
// FREE lets you e.g. anchor an element to the BOTTOM line while sliding it
// continuously along that line via x. When useAnchor is false, both axes are
// treated as FREE (legacy absolute x/y placement).
enum OledHAlign : uint8_t { OLED_H_LEFT = 0, OLED_H_CENTER = 1, OLED_H_RIGHT = 2, OLED_H_FREE = 3 };
enum OledVAlign : uint8_t { OLED_V_TOP = 0, OLED_V_MIDDLE = 1, OLED_V_BOTTOM = 2, OLED_V_FREE = 3 };

#define OLED_GUI_MAX_ELEMENTS 16
#define OLED_GUI_TEXT_MAX     64
#define OLED_GUI_FONT_MAX     20
#define OLED_GUI_RESOLVED_MAX 96

struct OledElement {
    OledElemType type;
    bool         visible;
    bool         useAnchor;   // true: anchor by halign/valign; false: absolute x/y
    int16_t      x, y;        // absolute position (top-left) when !useAnchor
    int16_t      w, h;        // shape size; line delta; optional text box (unused for text v1)
    int8_t       z;           // z-order, lower drawn first (further back)
    OledHAlign   halign;
    OledVAlign   valign;

    // Text
    char    text[OLED_GUI_TEXT_MAX];   // static string or {token} template
    char    fontName[OLED_GUI_FONT_MAX];
    uint8_t pointSize;
    // Cached resolved FontFamily for fontName (stored as int; -1 = not resolved
    // yet). oled::getFontFamily() is very heap-heavy (it builds an Arduino
    // String for every font in the table on every call), so resolving it once
    // per font change instead of every render frame is essential - otherwise a
    // live screen does thousands of String alloc/frees per second on core0 and
    // starves/clashes with core1's heap use. Invalidated whenever fontName set.
    int16_t fontFamilyCache;

    // Shape
    OledShapeKind shape;
    bool          filled;

    // When true, draw small L-shaped corner marks around this element's
    // rendered bounds (used to highlight the "selected" element in editors).
    bool          box;

    // Render cache (resolved template text) for dirty detection
    char resolved[OLED_GUI_RESOLVED_MAX];
};

// ---- Screen -----------------------------------------------------------------

class OledScreen {
public:
    OledScreen();

    void clearElements();                 // remove all elements

    // Returns element index (>=0) or -1 if full.
    int addText(const char* text, int16_t x, int16_t y,
                const char* fontName, uint8_t pointSize);
    int addShape(OledShapeKind kind, int16_t x, int16_t y,
                 int16_t w, int16_t h, bool filled);

    int          count() const { return count_; }
    OledElement* element(int idx);        // nullptr if out of range

    // Property setters (all mark the screen dirty). Return false on bad index.
    bool setText(int idx, const char* text);
    bool setFont(int idx, const char* fontName, uint8_t pointSize);
    bool setPos(int idx, int16_t x, int16_t y);      // absolute (clears useAnchor)
    bool setSize(int idx, int16_t w, int16_t h);
    bool setZ(int idx, int8_t z);
    bool setVisible(int idx, bool visible);
    bool setAnchor(int idx, OledHAlign h, OledVAlign v);  // sets useAnchor=true
    bool setShape(int idx, OledShapeKind kind, bool filled);
    bool setIntProp(int idx, const char* prop, int value);   // generic (for MP bridge)
    bool setStrProp(int idx, const char* prop, const char* value);

    void markDirty()       { dirty_ = true; }
    bool isDirty() const   { return dirty_; }
    void clearDirty()      { dirty_ = false; }

    // Re-resolve {token} templates for all text elements. Returns true if any
    // resolved string changed since last call.
    bool resolveBindings();

    // Render the whole screen into the oled framebuffer (does NOT flush).
    // `class oled&` (elaborated) is required because a global variable named
    // `oled` shadows the type name in this scope.
    void render(class oled& display);

    int16_t w = 128;
    int16_t h = 32;

private:
    OledElement elements_[OLED_GUI_MAX_ELEMENTS];
    int         count_;
    bool        dirty_;
};

// ---- Variable registry ------------------------------------------------------
// Built-in computed sources resolve on read (no Python callback ever made).
// Pushed values are cached strings, safe to set from any context.
namespace OledVars {
    void setStr(const char* name, const char* value);
    void setNum(const char* name, float value);
    bool getStr(const char* name, char* out, size_t outSize);  // pushed-var lookup
    void clearAll();

    // Resolve a single token ("name" or "name:arg") into `out`.
    // Returns true if it was a known builtin or a pushed var.
    bool resolveToken(const char* token, char* out, size_t outSize);

    // Expand a template ("{a}: {adc:0} V") into `out`. Unknown tokens are
    // left empty. Always NUL-terminates.
    void expand(const char* templ, char* out, size_t outSize);
}

// ---- Manager ----------------------------------------------------------------

class OledGui {
public:
    static OledGui& getInstance();

    // Ownership / lifecycle
    // `persist` registers the screen as the idle screen: it takes the place of
    // the boot logo (showJogo32h) when the UI returns to idle, and survives the
    // Python script/REPL that created it. Non-persistent screens behave like a
    // foreground app - they free-run via the render service while active and are
    // torn down when the script ends.
    void        activate(OledScreen* screen, bool persist = false);
    void        deactivate();
    void        hideAndClear();   // deactivate + blank the panel (visually remove)
    OledScreen* active() const { return active_; }
    void        suspend() { suspended_ = true; }
    void        resume()  { suspended_ = false; }
    bool        isSuspended() const { return suspended_; }

    // --- Idle screen (logo replacement) -------------------------------------
    // Render the registered persistent idle screen now (re-acquiring the panel)
    // and return true. Returns false if no idle screen is registered, so the
    // caller (showJogo32h) can fall back to the logo.
    bool        showIdle();
    OledScreen* idleScreen() const { return idleScreen_; }
    void        clearIdle() { idleScreen_ = nullptr; }      // forget the idle screen
    void        forgetIdle(OledScreen* s) { if ( idleScreen_ == s ) idleScreen_ = nullptr; }

    // Called by any non-GUI code that draws its own content to the panel
    // (clearPrintShow / clearPrintShowRich). The persistent idle screen yields
    // the panel so it stops fighting that content; it returns at the next
    // showIdle()/showJogo32h(). Foreground (non-persistent) screens are left
    // alone so they keep free-running, exactly as before.
    void        notePanelTakenByOther();
    bool        ownsPanel() const { return ownsPanel_; }

    // Called by OledGuiService each loop (internally rate-limited ~30 Hz).
    void tick();

    // Render the active screen now and flush to the panel. Safe to call from
    // the MicroPython/core0 thread (same path as oled_print). `force` bypasses
    // the throttle, the dirty check, and the idle-screen yield. No-op if there
    // is no active screen, the panel isn't ready, or a modal UI is on top. Has
    // a re-entrancy guard.
    void renderNow(bool force);

    // Force a re-render+flush on the next tick.
    void requestRender();

private:
    OledGui() : active_(nullptr), idleScreen_(nullptr),
                suspended_(false), ownsPanel_(false), lastTickMs_(0) {}
    OledScreen* active_;
    OledScreen* idleScreen_;   // persistent screen shown in place of the logo
    bool        suspended_;
    bool        ownsPanel_;    // is the GUI the current panel content?
    uint32_t    lastTickMs_;

    // Single eagerly-constructed instance. getInstance() now hands back this
    // global rather than a function-local static: clearPrintShow() (which calls
    // getInstance) runs on BOTH cores, and a Meyers singleton's lazy-init guard
    // can deadlock/race when two cores hit the first call concurrently on
    // rp2350. A plain global is constructed during single-threaded static init.
    static OledGui instance_;
};

// ---- Handle registry (for the MicroPython flat bridge) ----------------------
// Screens are heap-allocated and addressed by a small 1-based integer handle.
int         oledGuiCreateScreen();          // returns handle >=1, or 0 on failure
void        oledGuiDestroyScreen(int handle);
OledScreen* oledGuiGetScreen(int handle);   // nullptr if invalid

// Deactivate the active screen, free every screen handle (including any
// persistent idle screen), and blank the panel. This is the full "clean slate"
// used by oledgui.reset() at the start of a script.
void        oledGuiShutdownAll();

// Free every transient screen handle but KEEP the persistent idle screen (the
// one registered via show(persist=True)) registered and rendered. Used when a
// MicroPython script/REPL session ends so a foreground app's leftover screens
// are released while a deliberately-persistent stats/idle page survives.
void        oledGuiShutdownTransient();

// ---- Serialization ----------------------------------------------------------
// Single self-contained JSON file per screen at /screens/<name>.json.
bool oledGuiSaveScreen(OledScreen* screen, const char* name);
// Loads into a freshly created screen; returns its handle (>=1) or 0.
int  oledGuiLoadScreen(const char* name);

#endif // OLEDGUI_H
