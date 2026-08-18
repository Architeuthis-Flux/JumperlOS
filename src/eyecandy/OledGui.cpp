#include "OledGui.h"

#include "Peripherals.h"
#include "oled.h"
#include "JumperlOS.h"   // ContextManager for auto-suspend under modal UI
#include "States.h"
#include "WaveGen.h"     // wavegen.isRunning() - shared I2C0 bus arbitration
#include "FilesystemStuff.h" // safeFileOpen/safeFileClose (fs_mutex + core pause)
#include <FatFS.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// WaveGen instance (defined in main.cpp). On rev7 it streams the MCP4728 DAC
// over the SAME I2C0/Wire bus the OLED uses, from core1. We must not drive the
// OLED while it's running, or a multi-ms OLED frame collides with the DAC
// stream on the shared bus (and corrupts the waveform).
extern WaveGen wavegen;

// The display driver instance (declared in oled.h as `extern class oled oled;`).
// Reference it as the global object below.

// jl_* data accessors are defined with C linkage in JumperlessMicroPythonAPI.cpp.
extern "C" {
    int   jl_gpio_get( int pin );
    float jl_adc_get( int channel );
    float jl_dac_get( int channel );
}

// Globals used by oledPeriodic to gate display access during refreshes.
extern volatile bool refreshInProgress;
extern volatile bool refreshLocalInProgress;
extern volatile bool core1busy;
// core2busy is raised by core1 while it does hardware work (ADC reads via
// updateLazyAdcReadings/showLEDmeasurements, LED net rendering). The GUI render
// runs on core0 and drives the OLED; if it fires while core1 is mid-ADC-read
// (which shares the I2C0 bus / hardware on rev7 and churns the heap), the two
// collide. We must defer the render while core1 is busy - this is why an active
// ADC channel (more core1 work) was what tipped it into a crash.
extern volatile bool core2busy;

// Probe mode flag (defined in main.cpp, declared in Probing.h).
extern volatile int probeActive;

// Live hardware values come from caches that core1 ("core2") maintains
// (adcReadings[] via showLEDmeasurements, getDacVoltage() from globalState).
// The render path therefore never does a blocking hardware read on the shared
// I2C bus - it only reads RAM and flushes the framebuffer like oled_print.

// =============================================================================
// Variable registry
// =============================================================================

namespace {

#define OLED_VAR_MAX        24
#define OLED_VAR_NAME_MAX   24
#define OLED_VAR_VALUE_MAX  48

struct PushedVar {
    bool used;
    char name[OLED_VAR_NAME_MAX];
    char value[OLED_VAR_VALUE_MAX];
};

PushedVar g_vars[OLED_VAR_MAX] = {};

PushedVar* findVar( const char* name ) {
    for ( int i = 0; i < OLED_VAR_MAX; i++ ) {
        if ( g_vars[i].used && strcmp( g_vars[i].name, name ) == 0 ) return &g_vars[i];
    }
    return nullptr;
}

PushedVar* findOrAllocVar( const char* name ) {
    PushedVar* v = findVar( name );
    if ( v ) return v;
    for ( int i = 0; i < OLED_VAR_MAX; i++ ) {
        if ( !g_vars[i].used ) {
            g_vars[i].used = true;
            strncpy( g_vars[i].name, name, OLED_VAR_NAME_MAX - 1 );
            g_vars[i].name[OLED_VAR_NAME_MAX - 1] = '\0';
            g_vars[i].value[0] = '\0';
            return &g_vars[i];
        }
    }
    return nullptr;
}

} // namespace

void OledVars::setStr( const char* name, const char* value ) {
    if ( !name || !name[0] ) return;
    PushedVar* v = findOrAllocVar( name );
    if ( !v ) return;
    strncpy( v->value, value ? value : "", OLED_VAR_VALUE_MAX - 1 );
    v->value[OLED_VAR_VALUE_MAX - 1] = '\0';
}

void OledVars::setNum( const char* name, float value ) {
    char buf[OLED_VAR_VALUE_MAX];
    // %g drops trailing zeros and keeps integers clean.
    snprintf( buf, sizeof( buf ), "%g", (double)value );
    setStr( name, buf );
}

bool OledVars::getStr( const char* name, char* out, size_t outSize ) {
    if ( !out || outSize == 0 ) return false;
    PushedVar* v = findVar( name );
    if ( !v ) { out[0] = '\0'; return false; }
    strncpy( out, v->value, outSize - 1 );
    out[outSize - 1] = '\0';
    return true;
}

void OledVars::clearAll() {
    memset( g_vars, 0, sizeof( g_vars ) );
}

bool OledVars::resolveToken( const char* token, char* out, size_t outSize ) {
    if ( !out || outSize == 0 ) return false;
    out[0] = '\0';
    if ( !token || !token[0] ) return false;

    // Split into base name + optional ":arg".
    char base[OLED_VAR_NAME_MAX];
    strncpy( base, token, sizeof( base ) - 1 );
    base[sizeof( base ) - 1] = '\0';
    char* colon = strchr( base, ':' );
    const char* arg = nullptr;
    if ( colon ) { *colon = '\0'; arg = colon + 1; }
    int argN = arg ? atoi( arg ) : 0;

    // Built-in computed sources (resolved here, never via Python).
    if ( strcmp( base, "gpio" ) == 0 ) {
        int v = jl_gpio_get( argN );
        const char* s = ( v == 1 ) ? "HIGH" : ( v == 0 ) ? "LOW" : "?";
        strncpy( out, s, outSize - 1 ); out[outSize - 1] = '\0';
        return true;
    }
    if ( strcmp( base, "adc" ) == 0 ) {
        // Cached value maintained by core1's showLEDmeasurements() - no live
        // read here, so the render never touches the shared ADC/I2C hardware.
        if ( argN < 0 || argN >= 8 ) { strncpy( out, "?", outSize - 1 ); out[outSize - 1] = '\0'; return true; }
        snprintf( out, outSize, "%.2f", (double)adcReadings[argN] );
        return true;
    }
    if ( strcmp( base, "dac" ) == 0 ) {
        // getDacVoltage() reads the cached DAC state (no hardware transaction).
        if ( argN < 0 || argN >= 4 ) { strncpy( out, "?", outSize - 1 ); out[outSize - 1] = '\0'; return true; }
        snprintf( out, outSize, "%.2f", (double)getDacVoltage( argN ) );
        return true;
    }
    if ( strcmp( base, "uptime" ) == 0 ) {
        unsigned long s = millis() / 1000UL;
        snprintf( out, outSize, "%lu:%02lu", s / 60UL, s % 60UL );
        return true;
    }
    if ( strcmp( base, "millis" ) == 0 ) {
        snprintf( out, outSize, "%lu", millis() );
        return true;
    }
    if ( strcmp( base, "freemem" ) == 0 ) {
        snprintf( out, outSize, "%lu", (unsigned long)rp2040.getFreeHeap() );
        return true;
    }

    // Pushed variables: try the full token first (so "undo" / custom names
    // with no arg work), then the base name.
    if ( getStr( token, out, outSize ) ) return true;
    if ( colon && getStr( base, out, outSize ) ) return true;
    return false;
}

void OledVars::expand( const char* templ, char* out, size_t outSize ) {
    if ( !out || outSize == 0 ) return;
    out[0] = '\0';
    if ( !templ ) return;

    size_t oi = 0;
    const char* p = templ;
    while ( *p && oi < outSize - 1 ) {
        if ( *p == '{' ) {
            const char* close = strchr( p, '}' );
            if ( close ) {
                char token[OLED_GUI_TEXT_MAX];
                size_t tlen = (size_t)( close - p - 1 );
                if ( tlen >= sizeof( token ) ) tlen = sizeof( token ) - 1;
                memcpy( token, p + 1, tlen );
                token[tlen] = '\0';

                char val[OLED_GUI_RESOLVED_MAX];
                resolveToken( token, val, sizeof( val ) );
                for ( size_t i = 0; val[i] && oi < outSize - 1; i++ ) out[oi++] = val[i];
                p = close + 1;
                continue;
            }
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
}

// =============================================================================
// OledScreen
// =============================================================================

OledScreen::OledScreen() : count_( 0 ), dirty_( true ) {
    memset( elements_, 0, sizeof( elements_ ) );
}

void OledScreen::clearElements() {
    count_ = 0;
    memset( elements_, 0, sizeof( elements_ ) );
    dirty_ = true;
}

OledElement* OledScreen::element( int idx ) {
    if ( idx < 0 || idx >= count_ ) return nullptr;
    return &elements_[idx];
}

int OledScreen::addText( const char* text, int16_t x, int16_t y,
                         const char* fontName, uint8_t pointSize ) {
    if ( count_ >= OLED_GUI_MAX_ELEMENTS ) return -1;
    OledElement& e = elements_[count_];
    memset( &e, 0, sizeof( e ) );
    e.type = OLED_ELEM_TEXT;
    e.visible = true;
    e.useAnchor = false;
    e.x = x; e.y = y;
    e.z = 0;
    e.halign = OLED_H_LEFT; e.valign = OLED_V_TOP;
    strncpy( e.text, text ? text : "", OLED_GUI_TEXT_MAX - 1 );
    e.text[OLED_GUI_TEXT_MAX - 1] = '\0';
    strncpy( e.fontName, fontName ? fontName : "Pragmatism", OLED_GUI_FONT_MAX - 1 );
    e.fontName[OLED_GUI_FONT_MAX - 1] = '\0';
    e.pointSize = pointSize ? pointSize : 8;
    e.fontFamilyCache = -1;   // resolve lazily on first render
    e.resolved[0] = '\0';
    dirty_ = true;
    return count_++;
}

int OledScreen::addShape( OledShapeKind kind, int16_t x, int16_t y,
                          int16_t w, int16_t h, bool filled ) {
    if ( count_ >= OLED_GUI_MAX_ELEMENTS ) return -1;
    OledElement& e = elements_[count_];
    memset( &e, 0, sizeof( e ) );
    e.type = OLED_ELEM_SHAPE;
    e.visible = true;
    e.useAnchor = false;
    e.x = x; e.y = y; e.w = w; e.h = h;
    e.z = 0;
    e.halign = OLED_H_LEFT; e.valign = OLED_V_TOP;
    e.shape = kind;
    e.filled = filled || ( kind == OLED_SHAPE_FILLED_RECT );
    dirty_ = true;
    return count_++;
}

bool OledScreen::setText( int idx, const char* text ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    strncpy( e->text, text ? text : "", OLED_GUI_TEXT_MAX - 1 );
    e->text[OLED_GUI_TEXT_MAX - 1] = '\0';
    dirty_ = true;
    return true;
}

bool OledScreen::setFont( int idx, const char* fontName, uint8_t pointSize ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    if ( fontName ) {
        strncpy( e->fontName, fontName, OLED_GUI_FONT_MAX - 1 );
        e->fontName[OLED_GUI_FONT_MAX - 1] = '\0';
        e->fontFamilyCache = -1;   // font changed - re-resolve on next render
    }
    if ( pointSize ) e->pointSize = pointSize;
    dirty_ = true;
    return true;
}

bool OledScreen::setPos( int idx, int16_t x, int16_t y ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    e->x = x; e->y = y; e->useAnchor = false;
    dirty_ = true;
    return true;
}

bool OledScreen::setSize( int idx, int16_t w, int16_t h ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    e->w = w; e->h = h;
    dirty_ = true;
    return true;
}

bool OledScreen::setZ( int idx, int8_t z ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    e->z = z;
    dirty_ = true;
    return true;
}

bool OledScreen::setVisible( int idx, bool visible ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    e->visible = visible;
    dirty_ = true;
    return true;
}

bool OledScreen::setAnchor( int idx, OledHAlign h, OledVAlign v ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    e->halign = h; e->valign = v; e->useAnchor = true;
    dirty_ = true;
    return true;
}

bool OledScreen::setShape( int idx, OledShapeKind kind, bool filled ) {
    OledElement* e = element( idx );
    if ( !e ) return false;
    e->shape = kind; e->filled = filled;
    dirty_ = true;
    return true;
}

bool OledScreen::setIntProp( int idx, const char* prop, int value ) {
    OledElement* e = element( idx );
    if ( !e || !prop ) return false;
    if      ( strcmp( prop, "x" ) == 0 )       e->x = (int16_t)value;
    else if ( strcmp( prop, "y" ) == 0 )       e->y = (int16_t)value;
    else if ( strcmp( prop, "w" ) == 0 )       e->w = (int16_t)value;
    else if ( strcmp( prop, "h" ) == 0 )       e->h = (int16_t)value;
    else if ( strcmp( prop, "z" ) == 0 )       e->z = (int8_t)value;
    else if ( strcmp( prop, "size" ) == 0 )    e->pointSize = (uint8_t)value;
    else if ( strcmp( prop, "visible" ) == 0 ) e->visible = ( value != 0 );
    else if ( strcmp( prop, "anchor" ) == 0 )  e->useAnchor = ( value != 0 );
    else if ( strcmp( prop, "halign" ) == 0 )  e->halign = (OledHAlign)( value & 3 );
    else if ( strcmp( prop, "valign" ) == 0 )  e->valign = (OledVAlign)( value & 3 );
    else if ( strcmp( prop, "shape" ) == 0 )   e->shape = (OledShapeKind)( value & 3 );
    else if ( strcmp( prop, "filled" ) == 0 )  e->filled = ( value != 0 );
    else if ( strcmp( prop, "box" ) == 0 )     e->box = ( value != 0 );
    else return false;
    dirty_ = true;
    return true;
}

bool OledScreen::setStrProp( int idx, const char* prop, const char* value ) {
    if ( !prop ) return false;
    if ( strcmp( prop, "text" ) == 0 ) return setText( idx, value );
    if ( strcmp( prop, "font" ) == 0 ) return setFont( idx, value, 0 );
    return false;
}

bool OledScreen::resolveBindings() {
    bool changed = false;
    for ( int i = 0; i < count_; i++ ) {
        OledElement& e = elements_[i];
        if ( e.type != OLED_ELEM_TEXT ) continue;
        char buf[OLED_GUI_RESOLVED_MAX];
        OledVars::expand( e.text, buf, sizeof( buf ) );
        if ( strcmp( buf, e.resolved ) != 0 ) {
            strncpy( e.resolved, buf, OLED_GUI_RESOLVED_MAX - 1 );
            e.resolved[OLED_GUI_RESOLVED_MAX - 1] = '\0';
            changed = true;
        }
    }
    return changed;
}

void OledScreen::render( class oled& display ) {
    const int W = display.displayWidth;
    const int H = display.displayHeight;

    // Save font state - other code paths depend on it not changing.
    const GFXfont* savedFont = display.currentFont;
    FontFamily savedFamily = display.currentFontFamily;
    uint8_t savedTextSize = display.currentTextSize;

    display.clearFramebuffer();

    // Draw in z-order (stable). Small N, so a simple selection over z is fine.
    int order[OLED_GUI_MAX_ELEMENTS];
    for ( int i = 0; i < count_; i++ ) order[i] = i;
    for ( int a = 0; a < count_ - 1; a++ ) {
        for ( int b = 0; b < count_ - 1 - a; b++ ) {
            if ( elements_[order[b]].z > elements_[order[b + 1]].z ) {
                int t = order[b]; order[b] = order[b + 1]; order[b + 1] = t;
            }
        }
    }

    for ( int oi = 0; oi < count_; oi++ ) {
        OledElement& e = elements_[order[oi]];
        if ( !e.visible ) continue;

        if ( e.type == OLED_ELEM_TEXT ) {
            const char* str = e.resolved[0] ? e.resolved : e.text;
            if ( !str[0] ) continue;

            // Resolve the font family ONCE per font (getFontFamily allocates a
            // String per table entry, so calling it every frame is what made a
            // live screen thrash the heap on core0). Cache the result.
            if ( e.fontFamilyCache < 0 ) {
                e.fontFamilyCache = (int16_t)display.getFontFamily( e.fontName );
            }
            FontFamily fam = (FontFamily)e.fontFamilyCache;
            display.setFontPointSize( fam, e.pointSize );
            display.setTextSize( 1 );

            TextBounds tb = display.getTextBounds( str );
            int tw = tb.width;
            int th = tb.height;
            // Adafruit's getTextBounds returns the INK bounding box: the first
            // glyph's left bearing means the drawn pixels start at cursorX + x1,
            // not at cursorX. `left` below is the cursor, so the rendered ink
            // spans [left + x1, left + x1 + tw]. Edge-anchored placement must
            // subtract x1, otherwise right-aligned text overruns the panel by
            // x1 px (the last char falls off) and centered text sits x1 px right
            // of center. FREE/absolute keeps the editor's cursor x verbatim.
            int x1 = tb.x1;

            // Per-axis placement. FREE (or !useAnchor) uses absolute x/y so the
            // element can slide continuously; LEFT/CENTER/RIGHT and TOP/MIDDLE/
            // BOTTOM snap to the panel edges/center.
            int left, top;
            if ( !e.useAnchor || e.halign == OLED_H_FREE ) {
                left = e.x;
            } else if ( e.halign == OLED_H_CENTER ) {
                left = ( W - tw ) / 2 - x1;
            } else if ( e.halign == OLED_H_RIGHT ) {
                left = W - tw - x1;
            } else {
                left = -x1;   // flush left: pull the bearing back to the edge
            }
            if ( !e.useAnchor || e.valign == OLED_V_FREE ) {
                top = e.y;
            } else if ( e.valign == OLED_V_MIDDLE ) {
                top = ( H - th ) / 2;
            } else if ( e.valign == OLED_V_BOTTOM ) {
                top = H - th;
            } else {
                top = 0;
            }
            // Clamp on the INK edge (left + x1), not the cursor, so the x1
            // corrections above aren't clobbered: keep the first drawn pixel
            // on-panel rather than forcing the (possibly off-screen-left)
            // cursor to 0.
            if ( left + x1 < 0 ) left = -x1;
            if ( top < 0 ) top = 0;

            // Baseline = top + ASCENT (not top + full height). getTextBounds'
            // height includes the descender, and glyphs are drawn from
            // (baseline - ascent) down to (baseline + descent). Using top + th
            // as the baseline pushes the whole glyph down by `descent`, so any
            // text with a descender (g/j/p/q/y - e.g. the "p" in "Top Rail")
            // renders ~2-3 px too low and clips at the bottom edge. Anchoring on
            // the ascent puts the ink top exactly at `top` and the descender
            // bottom at top + th, keeping the glyph inside its measured box.
            int ascent = tb.ascent > 0 ? tb.ascent : th;
            display.setCursor( left, top + ascent, POS_BASELINE );
            display.print( str );

            // Optional selection highlight: small L-shaped corner marks around
            // the element's measured bounds (less visually heavy than a full box).
            // Wrap the actual ink (left + x1), not the bare cursor.
            if ( e.box ) {
                int bx = left + x1 - 2;    if ( bx < 0 ) bx = 0;
                int by = top - 1;         if ( by < 0 ) by = 0;
                int bw = tw + 3;          if ( bx + bw > W ) bw = W - bx;
                int bh = th + 2;          if ( by + bh > H ) bh = H - by;
                if ( bw > 3 && bh > 3 ) {
                    int m = 3;                       // corner mark length
                    if ( m > bw / 2 ) m = bw / 2;
                    if ( m > bh / 2 ) m = bh / 2;
                    int rx = bx + bw - 1;            // right edge
                    int yb = by + bh - 1;            // bottom edge
                    // top-left
                    display.drawLine( bx, by, bx + m, by, 1 );
                    display.drawLine( bx, by, bx, by + m, 1 );
                    // top-right
                    display.drawLine( rx, by, rx - m, by, 1 );
                    display.drawLine( rx, by, rx, by + m, 1 );
                    // bottom-left
                    display.drawLine( bx, yb, bx + m, yb, 1 );
                    display.drawLine( bx, yb, bx, yb - m, 1 );
                    // bottom-right
                    display.drawLine( rx, yb, rx - m, yb, 1 );
                    display.drawLine( rx, yb, rx, yb - m, 1 );
                }
            }

        } else { // OLED_ELEM_SHAPE
            int sw = e.w, sh = e.h;
            int left, top;
            if ( !e.useAnchor || e.shape == OLED_SHAPE_LINE || e.halign == OLED_H_FREE ) {
                left = e.x;
            } else if ( e.halign == OLED_H_CENTER ) {
                left = ( W - sw ) / 2;
            } else if ( e.halign == OLED_H_RIGHT ) {
                left = W - sw;
            } else {
                left = 0;
            }
            if ( !e.useAnchor || e.shape == OLED_SHAPE_LINE || e.valign == OLED_V_FREE ) {
                top = e.y;
            } else if ( e.valign == OLED_V_MIDDLE ) {
                top = ( H - sh ) / 2;
            } else if ( e.valign == OLED_V_BOTTOM ) {
                top = H - sh;
            } else {
                top = 0;
            }

            switch ( e.shape ) {
                case OLED_SHAPE_LINE:
                    display.drawLine( left, top, left + sw, top + sh, 1 );
                    break;
                case OLED_SHAPE_FILLED_RECT:
                    display.fillRect( left, top, sw, sh, 1 );
                    break;
                case OLED_SHAPE_RECT:
                default:
                    if ( sw > 0 && sh > 0 ) {
                        display.drawLine( left, top, left + sw - 1, top, 1 );                 // top
                        display.drawLine( left, top + sh - 1, left + sw - 1, top + sh - 1, 1 ); // bottom
                        display.drawLine( left, top, left, top + sh - 1, 1 );                 // left
                        display.drawLine( left + sw - 1, top, left + sw - 1, top + sh - 1, 1 ); // right
                    }
                    break;
            }
        }
    }

    // Restore prior font state.
    display.currentFont = savedFont;
    display.currentFontFamily = savedFamily;
    display.currentTextSize = savedTextSize;
    display.setFont( savedFont );
    display.setTextSize( savedTextSize );
}

// =============================================================================
// OledGui manager
// =============================================================================

// Eagerly constructed at static-init time (single-threaded), so getInstance()
// never runs a cross-core lazy-init guard. See the note in OledGui.h.
OledGui OledGui::instance_;

OledGui& OledGui::getInstance() {
    return instance_;
}

void OledGui::activate( OledScreen* screen, bool persist ) {
    active_ = screen;
    suspended_ = false;
    ownsPanel_ = true;          // an explicit show() always re-takes the panel
    // Persistent screens become the idle/logo replacement and survive the
    // script that made them. A non-persistent show() does NOT clear an existing
    // idle registration, so a foreground app shown on top of a persistent page
    // still falls back to that page when it exits.
    if ( persist ) idleScreen_ = screen;
    if ( screen ) screen->markDirty();
}

void OledGui::deactivate() {
    active_ = nullptr;
    ownsPanel_ = false;
}

bool OledGui::showIdle() {
    if ( !idleScreen_ ) return false;
    active_ = idleScreen_;
    suspended_ = false;
    ownsPanel_ = true;
    idleScreen_->markDirty();
    // Deliberately do NOT render here. showJogo32h() (our only caller besides
    // shutdownTransient) is reached from BOTH cores - e.g. core1 logo
    // swirls/animations and OLED reconnect. Rendering a screen allocates
    // (String for font lookup) and drives the shared I2C0 bus; doing that off
    // core1 while core0 also touches the heap/bus corrupts state. Instead we
    // just re-take ownership + mark dirty and let OledGuiService::tick() draw
    // it on core0, where every GUI render already happens safely.
    return true;
}

void OledGui::notePanelTakenByOther() {
    // Only the persistent idle screen steps aside for other content. Foreground
    // (non-persistent) screens keep free-running, matching prior behavior, so
    // interactive apps like the layout editor aren't disturbed.
    if ( idleScreen_ && active_ == idleScreen_ ) ownsPanel_ = false;
}

// Blank the panel so a removed screen doesn't linger. Safe no-op if the OLED
// isn't connected or a toast hold is in progress (don't fight the hold).
static void oledGuiClearPanel() {
    if ( !oled.isConnected() ) return;
    if ( oled.oledIsHeld() ) return;
    oled.clearFramebuffer();
    oled.show();
}

void OledGui::hideAndClear() {
    active_ = nullptr;
    ownsPanel_ = false;
    // NOTE: idleScreen_ is intentionally left registered - destroying a
    // foreground screen shouldn't forget a separately-registered idle page.
    // Callers that mean "stop showing the idle page too" call clearIdle().
    oledGuiClearPanel();
}

void OledGui::requestRender() {
    if ( active_ ) active_->markDirty();
}

void OledGui::renderNow( bool force ) {
    // Re-entrancy guard: render()/show() must never recurse (e.g. if a flush
    // path ends up pumping services again). Single-core cooperative, so a
    // plain static flag is sufficient.
    static bool inRender = false;
    if ( inRender ) return;

    if ( !active_ || suspended_ ) return;

    // The persistent idle screen yields the panel whenever other code draws its
    // own content (see notePanelTakenByOther). While yielded, the background
    // service and set_var()/property renders must NOT redraw it - otherwise it
    // flickers back over menus/toasts. It returns via showIdle()/showJogo32h(),
    // which re-set ownsPanel_. `force` (explicit show()) always draws.
    if ( !force && active_ == idleScreen_ && !ownsPanel_ ) return;

    // Probe mode owns the panel.
    if ( probeActive ) return;

    // Don't fight a modal full-screen UI; auto-resume when it exits.
    ContextType ctx = contextManager.currentContext();
    if ( ctx == ContextType::FILE_MANAGER || ctx == ContextType::EKILO_EDITOR ||
         ctx == ContextType::HELP_DOCS    || ctx == ContextType::DEBUG_MENU   ||
         ctx == ContextType::PROBING      || ctx == ContextType::CLICKWHEEL_MENU ) {
        return;
    }

    // Mirror oledPeriodic's refresh gating - never touch the panel mid-refresh.
    // core2busy covers core1's ADC reads + LED rendering, which share hardware
    // with the OLED; rendering through it is what crashed under ADC activity.
    if ( refreshInProgress || refreshLocalInProgress || core1busy || core2busy ) return;

    if ( !oled.isConnected() ) return;

    // Don't touch the shared I2C0 bus while wavegen streams the DAC from core1
    // (rev7). Defer instead: keep the screen dirty so it flushes once wavegen
    // stops. This also covers the OLED-vs-DAC bus collision on that bus.
    if ( wavegen.isRunning() ) {
        if ( active_ ) active_->markDirty();
        return;
    }

    // Throttle non-forced renders so rapid setter/var calls coalesce. The
    // persistent idle screen (stats page) is a low-information-rate display, and
    // its {adc}/{uptime} tokens change on essentially every tick from ADC noise;
    // rendering that at the foreground rate hammered the OLED bus and "ran super
    // often, interfering with everything else." Cap it to a calm cadence (~6 Hz)
    // while leaving interactive foreground screens (e.g. the layout editor)
    // responsive at the fast rate.
    uint32_t now = millis();
    uint32_t minInterval = ( active_ == idleScreen_ ) ? 160 : 15;
    if ( !force && ( now - lastTickMs_ ) < minInterval ) return;

    bool changed = active_->resolveBindings();
    if ( !force && !changed && !active_->isDirty() ) return;

    inRender = true;
    active_->render( oled );
    oled.show();   // respects the hold gate, so toasts overlay correctly
    active_->clearDirty();
    lastTickMs_ = now;
    inRender = false;
}

void OledGui::tick() {
    renderNow( false );
}

// =============================================================================
// Handle registry (for the MicroPython flat bridge)
// =============================================================================

namespace {
#define OLED_GUI_MAX_SCREENS 8
OledScreen* g_screens[OLED_GUI_MAX_SCREENS] = {};
} // namespace

int oledGuiCreateScreen() {
    for ( int i = 0; i < OLED_GUI_MAX_SCREENS; i++ ) {
        if ( g_screens[i] == nullptr ) {
            g_screens[i] = new OledScreen();
            if ( !g_screens[i] ) return 0;
            return i + 1;   // 1-based handle
        }
    }
    return 0;
}

OledScreen* oledGuiGetScreen( int handle ) {
    if ( handle < 1 || handle > OLED_GUI_MAX_SCREENS ) return nullptr;
    return g_screens[handle - 1];
}

void oledGuiDestroyScreen( int handle ) {
    OledScreen* s = oledGuiGetScreen( handle );
    if ( !s ) return;
    OledGui& gui = OledGui::getInstance();
    // If we're freeing the screen that's currently on the panel, stop the
    // render service AND blank the panel before deleting, so nothing can race
    // on (or render) a freed screen and no stale image is left behind.
    if ( gui.active() == s ) gui.hideAndClear();
    // Never leave the idle registration dangling at a freed screen.
    gui.forgetIdle( s );
    delete s;
    g_screens[handle - 1] = nullptr;
}

void oledGuiShutdownAll() {
    // Full clean slate: stop rendering, blank the panel, forget the idle screen,
    // then release every handle. Called by oledgui.reset() at script start.
    OledGui::getInstance().hideAndClear();
    OledGui::getInstance().clearIdle();
    for ( int i = 0; i < OLED_GUI_MAX_SCREENS; i++ ) {
        if ( g_screens[i] ) {
            delete g_screens[i];
            g_screens[i] = nullptr;
        }
    }
}

void oledGuiShutdownTransient() {
    OledGui& gui = OledGui::getInstance();
    OledScreen* keep = gui.idleScreen();

    // No persistent idle page to preserve - behave like a full shutdown.
    if ( !keep ) { oledGuiShutdownAll(); return; }

    // Free every screen except the persistent idle page.
    for ( int i = 0; i < OLED_GUI_MAX_SCREENS; i++ ) {
        if ( g_screens[i] && g_screens[i] != keep ) {
            delete g_screens[i];
            g_screens[i] = nullptr;
        }
    }
    // Make the surviving idle page the active display (it took the logo's place)
    // so it keeps live-updating after the script/REPL that built it goes away.
    gui.showIdle();
}

// =============================================================================
// Serialization - single self-contained JSON per screen
// =============================================================================

namespace {

void screenJsonPath( const char* name, char* out, size_t cap ) {
    // Strip any directory components / extension the caller may have included.
    const char* base = name ? name : "screen";
    const char* slash = strrchr( base, '/' );
    if ( slash ) base = slash + 1;
    char tmp[48];
    strncpy( tmp, base, sizeof( tmp ) - 1 );
    tmp[sizeof( tmp ) - 1] = '\0';
    char* dot = strstr( tmp, ".json" );
    if ( dot ) *dot = '\0';
    snprintf( out, cap, "/screens/%s.json", tmp[0] ? tmp : "screen" );
}

} // namespace

bool oledGuiSaveScreen( OledScreen* screen, const char* name ) {
    if ( !screen ) return false;

    if ( !safeFileExists( "/screens" ) ) safeMkdir( "/screens" );

    char path[80];
    screenJsonPath( name, path, sizeof( path ) );

    JsonDocument doc;
    doc["version"] = 1;
    doc["w"] = screen->w;
    doc["h"] = screen->h;
    JsonArray arr = doc["elements"].to<JsonArray>();

    for ( int i = 0; i < screen->count(); i++ ) {
        OledElement* e = screen->element( i );
        if ( !e ) continue;
        JsonObject o = arr.add<JsonObject>();
        o["x"] = e->x;
        o["y"] = e->y;
        o["w"] = e->w;
        o["h"] = e->h;
        o["z"] = e->z;
        o["visible"] = e->visible ? 1 : 0;
        o["anchor"] = e->useAnchor ? 1 : 0;
        o["halign"] = (int)e->halign;
        o["valign"] = (int)e->valign;
        if ( e->type == OLED_ELEM_TEXT ) {
            o["type"] = "text";
            o["text"] = e->text;
            o["font"] = e->fontName;
            o["size"] = e->pointSize;
        } else {
            o["type"] = "shape";
            o["shape"] = (int)e->shape;
            o["filled"] = e->filled ? 1 : 0;
        }
        // "bitmap" (base64) reserved here for the deferred Image element type.
    }

    // safeFileOpen/safeFileClose: fs_mutex + Core-1 pause, like every other
    // firmware writer (raw FatFS.open here raced USB MSC / Python FS access).
    File f = safeFileOpen( path, "w" );
    if ( !f ) return false;
    serializeJsonPretty( doc, f );
    safeFileClose( f, true );
    return true;
}

int oledGuiLoadScreen( const char* name ) {
    char path[80];
    screenJsonPath( name, path, sizeof( path ) );

    File f = safeFileOpen( path, "r" );
    if ( !f ) return 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson( doc, f );
    safeFileClose( f, false );
    if ( err ) return 0;

    int handle = oledGuiCreateScreen();
    if ( !handle ) return 0;
    OledScreen* s = oledGuiGetScreen( handle );
    if ( !s ) return 0;

    s->w = doc["w"] | 128;
    s->h = doc["h"] | 32;

    for ( JsonObject o : doc["elements"].as<JsonArray>() ) {
        const char* type = o["type"] | "text";
        int x = o["x"] | 0;
        int y = o["y"] | 0;
        int w = o["w"] | 0;
        int h = o["h"] | 0;
        int idx;
        if ( strcmp( type, "shape" ) == 0 ) {
            int kind = o["shape"] | (int)OLED_SHAPE_RECT;
            int filled = o["filled"] | 0;
            idx = s->addShape( (OledShapeKind)kind, x, y, w, h, filled != 0 );
        } else {
            const char* text = o["text"] | "";
            const char* font = o["font"] | "Pragmatism";
            int size = o["size"] | 8;
            idx = s->addText( text, x, y, font, (uint8_t)size );
        }
        if ( idx < 0 ) break;
        s->setSize( idx, w, h );
        s->setZ( idx, (int8_t)( o["z"] | 0 ) );
        s->setVisible( idx, ( o["visible"] | 1 ) != 0 );
        int halign = o["halign"] | 0;
        int valign = o["valign"] | 0;
        if ( ( o["anchor"] | 0 ) != 0 ) {
            s->setAnchor( idx, (OledHAlign)halign, (OledVAlign)valign );
        }
    }

    return handle;
}
