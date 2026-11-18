#include "StatsPages.h"

#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"   // jumperlessConfig
#include "oled.h"     // oled, getDisplay, fontList

#if defined(OG_JUMPERLESS)

// ponytail: V5 only. Every page reads a V5 sensor set (the lazy ADC cache,
// INA1, the ADC6 supply sense, the routable GPIO bank) the OG doesn't have;
// give the OG its own page table when it has something to show.
namespace StatsPages {
int count(void) { return 0; }
void show(int) {}
void hide(void) {}
void service(void) {}
}  // namespace StatsPages

#else

#include "Commands.h"              // refreshInProgress / refreshLocalInProgress
#include "InfraPaths.h"            // infraProbePowerSource
#include "JumperlOS.h"             // jOS, contextManager
#include "JumperlessDefines.h"
#include "NetVoltageScan.h"        // pathCurrentRaw_mA
#include "NetsToChipConnections.h" // numberOfPaths
#include "OledGui.h"               // the idle screen yields the panel to a page
#include "Peripherals.h"           // adcReadings, showADCreadings, DAC/rail volts
#include "Probing.h"               // probing, switchPosition, probeActive
#include "ReadingDisplay.h"        // the pinned serial lines
#include "RoutableGpio.h"          // gpioDef, gpioNet, gpioReading
#include "States.h"                // globalState
#include "WaveGen.h"

extern WaveGen wavegen;              // main.cpp
extern float supplySense;            // main.cpp - ADC6, the charge pump's + rail, refreshed on core 1
extern const char firmwareVersion[]; // main.cpp
extern int startupDisplayed;         // oled.cpp - showJogo32h's "startup bitmap already shown" latch
bool looksLikeFilePath( const char* str ); // oled.cpp

namespace {

// Fonts (fontList[] indices). Both are fixed-pitch, so a column of numbers
// lines up on its decimal points just by right-aligning the strings. Both
// ink their baseline row, so a glyph is one row taller than its cap offset:
// Andale stacks four rows on an 8 px pitch with a clear row between them,
// Pragmatism 6pt three on an 11 px pitch.
constexpr int16_t kLabel = 12; // Andale Mono 5pt:  6 px pitch, 7 rows of ink
constexpr int16_t kP6 = 17;    // Pragmatism 6pt:   6 px pitch, 9 rows of ink

// The pages are drawn for 128x32; a taller panel centers that band.
int s_y0 = 0;

// ---- drawing ---------------------------------------------------------------

int textW( int16_t font, const char* s ) {
    const GFXfont* f = fontList[ font ].font;
    return (int)strlen( s ) * f->glyph[ '0' - f->first ].xAdvance; // fixed pitch
}
void textAt( int16_t font, int x, int baseline, const char* s ) {
    getDisplay( ).setFont( fontList[ font ].font );
    getDisplay( ).setCursor( x, s_y0 + baseline );
    getDisplay( ).print( s );
}
void textRight( int16_t font, int xRight, int baseline, const char* s ) {
    textAt( font, xRight - textW( font, s ), baseline, s );
}
void textCenter( int16_t font, int xCenter, int baseline, const char* s ) {
    textAt( font, xCenter - textW( font, s ) / 2, baseline, s );
}
void hline( int x, int y, int w ) { getDisplay( ).drawFastHLine( x, s_y0 + y, w, SSD1306_WHITE ); }
void vline( int x, int y, int h ) { getDisplay( ).drawFastVLine( x, s_y0 + y, h, SSD1306_WHITE ); }

// Append to the page's one-line serial twin.
void say( char* line, size_t cap, const char* fmt, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
void say( char* line, size_t cap, const char* fmt, ... ) {
    size_t len = strlen( line );
    if ( len + 1 >= cap ) return;
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( line + len, cap - len, fmt, ap );
    va_end( ap );
}

// ---- power: every supply's volts and total current --------------------------

// Net current OUT of a supply: the signed sum over every routed path that
// starts at its node. (netCurrent_mA() is the net's single busiest path - on
// a rail feeding three rows that is one row's share, not the rail's load.)
// false = the supply has paths but the scan has nothing fresh for them.
bool sourceCurrent_mA( int net, int sourceNode, float* out ) {
    float sum = 0.0f;
    int touching = 0, known = 0;
    for ( int i = 0; i < numberOfPaths && i < MAX_BRIDGES; i++ ) {
        const pathStruct& p = globalState.connections.paths[ i ];
        if ( p.net != net || p.duplicate != 0 ) continue; // duplicates fold into their main path
        if ( p.node1 != sourceNode && p.node2 != sourceNode ) continue;
        touching++;
        if ( !pathCurrentKnown( i ) ) continue;
        known++;
        float c = pathCurrentRaw_mA( i ); // + = node1 -> node2
        sum += ( p.node1 == sourceNode ) ? c : -c;
    }
    *out = sum;
    return touching == 0 || known > 0; // nothing routed = a true zero
}

void drawPower( char* line, size_t cap ) {
    // Capitals on purpose: four rows fill the panel, and a descender would
    // touch the row below.
    static const struct {
        const char* name;
        const char* tag;
        int net, node;
    } kSupplies[ 4 ] = {
        { "TOP", "top", 2, TOP_RAIL },
        { "BOT", "bot", 3, BOTTOM_RAIL },
        { "DAC0", "dac0", 4, DAC0 },
        { "DAC1", "dac1", 5, DAC1 },
    };
    for ( int i = 0; i < 4; i++ ) {
        // Hardware truth for the rails (a save=0 write moves the pin without
        // touching the saved state) - the same sources the reading cards use.
        float v = ( i < 2 ) ? getDacHardwareVoltage( i + 2 ) : getDacVoltage( i - 2 );
        float mA = 0.0f;
        bool known = true;
        if ( i == 2 ) {
            // DAC 0 is the one supply with a real ammeter: INA1's shunt sits
            // in its output, so this is everything it sources, probe feed
            // included - measured, not estimated from crosspoint drops.
            mA = probing.checkProbeCurrentRaw( ) - jumperlessConfig.probe.current_zero;
        } else {
            known = sourceCurrent_mA( kSupplies[ i ].net, kSupplies[ i ].node, &mA );
        }
        char vBuf[ 12 ], aBuf[ 12 ];
        snprintf( vBuf, sizeof( vBuf ), "%.2f", (double)v );
        if ( known ) snprintf( aBuf, sizeof( aBuf ), "%.1f", (double)mA );
        else snprintf( aBuf, sizeof( aBuf ), "- -" );
        // name | volts | milliamps: the numbers right-aligned so the points
        // line up down the column, each unit set off by 2 px.
        int base = 6 + i * 8;
        textAt( kLabel, 0, base, kSupplies[ i ].name );
        textRight( kLabel, 66, base, vBuf );
        textAt( kLabel, 68, base, "V" );
        textRight( kLabel, 110, base, aBuf );
        textAt( kLabel, 112, base, "mA" );
        say( line, cap, "%s%s %sV %smA", i ? "  " : "", kSupplies[ i ].tag, vBuf, known ? aBuf : "?" );
    }
}

// ---- gpio: every routable pin's direction and level --------------------------

void drawGpio( char* line, size_t cap ) {
    for ( int i = 0; i < 10; i++ ) {
        uint pin = (uint)gpioDef[ i ][ 0 ];
        gpio_function_t fn = routableGpioFunction( i );
        bool drives = false;
        char role = 'i';
        if ( fn == GPIO_FUNC_SIO ) {
            drives = gpio_get_dir( pin );
            role = drives ? 'o' : 'i';
        } else if ( fn == GPIO_FUNC_UART ) {
            drives = ( i == 8 ); // Tx
            role = 'U';
        } else if ( fn == GPIO_FUNC_I2C ) {
            role = ( pin & 1 ) ? 'C' : 'D'; // the silicon's fixed map: odd = SCL, even = SDA
        } else if ( fn == GPIO_FUNC_PWM ) {
            drives = true;
            role = 'P';
        } else {
            role = '?';
        }
        // 0 low, 1 high, 2 floating, 3 = an input on no net (nothing to read)
        int level;
        if ( fn == GPIO_FUNC_SIO && drives ) {
            level = gpio_get_out_level( pin ) ? 1 : 0;
        } else if ( fn == GPIO_FUNC_SIO && i < 8 ) {
            level = ( gpioNet[ i ] >= 0 && gpioReading[ i ] <= 2 ) ? gpioReading[ i ] : 3;
        } else {
            level = gpio_get( pin ) ? 1 : 0;
        }

        char name[ 4 ];
        if ( i < 8 ) snprintf( name, sizeof( name ), "%d", i + 1 );
        else snprintf( name, sizeof( name ), "%s", i == 8 ? "Tx" : "Rx" );
        // GPIO 1-8 on an 11 px pitch, then the two UART pins set apart
        int xc = ( i < 8 ) ? 5 + i * 11 : 100 + ( i - 8 ) * 15;
        textCenter( kLabel, xc, 6, name );
        int bx = xc - 4, by = s_y0 + 9;
        if ( level == 1 ) {
            getDisplay( ).fillRect( bx, by, 9, 9, SSD1306_WHITE );
        } else if ( level == 0 ) {
            getDisplay( ).drawRect( bx, by, 9, 9, SSD1306_WHITE );
        } else if ( level == 2 ) { // floating: the box's corners and a dash
            getDisplay( ).drawPixel( bx, by, SSD1306_WHITE );
            getDisplay( ).drawPixel( bx + 8, by, SSD1306_WHITE );
            getDisplay( ).drawPixel( bx, by + 8, SSD1306_WHITE );
            getDisplay( ).drawPixel( bx + 8, by + 8, SSD1306_WHITE );
            getDisplay( ).drawFastHLine( xc - 1, by + 4, 3, SSD1306_WHITE );
        } else { // on no net: a dot where the box would be
            getDisplay( ).drawPixel( xc, by + 4, SSD1306_WHITE );
        }
        char roleStr[ 2 ] = { role, '\0' };
        textCenter( kLabel, xc, 27, roleStr );
        if ( drives ) hline( bx, 30, 9 ); // underlined = this pin drives

        static const char kLevelChar[ 4 ] = { 'L', 'H', '~', '-' };
        say( line, cap, "%s%s:%c%c", i ? " " : "", name, role, kLevelChar[ level ] );
    }
}

// ---- adc: every channel that is on a net -------------------------------------

// One label / value cell of the 2 x 3 grid the adc page is laid out on.
void gridCell( int idx, const char* label, const char* value ) {
    int x0 = ( idx < 3 ) ? 0 : 65;
    int base = 9 + ( idx % 3 ) * 11;
    textAt( kLabel, x0, base - 1, label );
    textRight( kP6, x0 + 58, base, value );
}

void drawAdc( char* line, size_t cap ) {
    // 0-3 are the +-8 V inputs, 4 the 0-5 V one; the cache is kept fresh on
    // core 1 (updateLazyAdcReadings), so nothing here touches the converter.
    for ( int i = 0; i < 5; i++ ) {
        char label[ 4 ], value[ 12 ];
        snprintf( label, sizeof( label ), "A%d", i );
        bool onNet = showADCreadings[ i ] > 0;
        if ( onNet ) snprintf( value, sizeof( value ), "%.2f V", (double)adcReadings[ i ] );
        else snprintf( value, sizeof( value ), "- -" );
        gridCell( i, label, value );
        say( line, cap, "%sA%d %s", i ? "  " : "", i, onNet ? value : "-" );
    }
    // The probe tip is a meter only with the switch on MEASURE.
    char tip[ 12 ];
    bool measuring = ( switchPosition == 0 );
    if ( measuring ) snprintf( tip, sizeof( tip ), "%.2f V", (double)adcReadings[ 7 ] );
    else snprintf( tip, sizeof( tip ), "- -" );
    gridCell( 5, "Tip", tip );
    say( line, cap, "  tip %s", measuring ? tip : "-" );
    vline( 62, 2, 28 );
}

// ---- system: the board itself -------------------------------------------------

void drawSystem( char* line, size_t cap ) {
    // Probe: switch position, and the feed current when INA1 can see it (its
    // shunt sits in DAC 0's output, so a GPIO-fed probe has no reading).
    const char* sw = ( switchPosition == 0 ) ? "measure" : ( switchPosition == 1 ) ? "select" : "?";
    char probeBuf[ 12 ];
    bool fedByDac0 = ( infraProbePowerSource( ) == DAC0 );
    if ( fedByDac0 ) {
        snprintf( probeBuf, sizeof( probeBuf ), "%.1f mA",
                  (double)( probing.checkProbeCurrentRaw( ) - jumperlessConfig.probe.current_zero ) );
    } else {
        snprintf( probeBuf, sizeof( probeBuf ), "- - mA" );
    }
    textAt( kLabel, 0, 7, "Probe" );
    textAt( kP6, 34, 8, sw );
    textRight( kP6, 123, 8, probeBuf );

    // The charge pump: ADC6 senses the + rail; the - rail mirrors it.
    char psu[ 12 ];
    snprintf( psu, sizeof( psu ), "+%.1f V", (double)supplySense );
    textAt( kLabel, 0, 18, "PSU" );
    int px = 58 - textW( kP6, psu );
    textAt( kP6, px, 19, psu );
    hline( px, 19, 5 ); // a bar under the plus: +/-

    unsigned long s = millis( ) / 1000UL;
    char up[ 12 ];
    if ( s < 36000UL ) snprintf( up, sizeof( up ), "%lu:%02lu:%02lu", s / 3600UL, ( s / 60UL ) % 60UL, s % 60UL );
    else snprintf( up, sizeof( up ), "%luh%02lum", s / 3600UL, ( s / 60UL ) % 60UL );
    textAt( kLabel, 66, 18, "Up" );
    textRight( kP6, 123, 19, up );

    char ram[ 12 ];
    snprintf( ram, sizeof( ram ), "%lu kB", (unsigned long)( rp2040.getFreeHeap( ) / 1024 ) );
    textAt( kLabel, 0, 29, "RAM" );
    textRight( kP6, 58, 30, ram );

    char ver[ 12 ];
    snprintf( ver, sizeof( ver ), "v%s", firmwareVersion );
    textRight( kP6, 123, 30, ver );
    vline( 62, 12, 20 );

    say( line, cap, "probe %s %s  psu +-%.1fV  up %s  ram %s  %s", sw, fedByDac0 ? probeBuf : "?",
         (double)supplySense, up, ram, ver );
}

// ---- the page table -----------------------------------------------------------
// One row per page, in scroll order. Adding a page is one draw function and
// one row here; each draws at 128x32 and appends its serial twin to `line`.
const struct {
    const char* name;
    void ( *draw )( char* line, size_t cap );
} kPages[] = {
    { "power", drawPower },
    { "gpio", drawGpio },
    { "adc", drawAdc },
    { "system", drawSystem },
};
constexpr int kNumPages = (int)( sizeof( kPages ) / sizeof( kPages[ 0 ] ) );

enum { MODE_OFF = 0, MODE_ON = 1, MODE_LOGO = 2 }; // statsPagesTable

// ---- state --------------------------------------------------------------------

int8_t s_live = -1;           // the page that owns the panel, -1 = none
uint32_t s_frameHash = 0;     // what the panel shows, to skip identical frames
uint32_t s_lineHash = 0;      // ... and the serial twin
unsigned long s_lastPaintMs = 0;
bool s_wasHeld = false;       // a toast held the panel since our last paint

constexpr unsigned long kRefreshMs = 250;

uint32_t fnv1a( const uint8_t* p, size_t n ) {
    uint32_t h = 2166136261u;
    while ( n-- ) h = ( h ^ *p++ ) * 16777619u;
    return h;
}

// Slim page marker down the right edge: one tick per stop, this one solid.
void drawPager( int page, int n ) {
    if ( n < 2 ) return;
    int seg = ( 32 - ( n - 1 ) ) / n;
    int y = ( 32 - ( n * seg + ( n - 1 ) ) ) / 2;
    for ( int i = 0; i < n; i++ ) {
        if ( i == page ) getDisplay( ).fillRect( 126, s_y0 + y, 2, seg, SSD1306_WHITE );
        else vline( 127, y, seg );
        y += seg + 1;
    }
}

bool panelUsable( void ) {
    return oled.oledConnected && jumperlessConfig.top_oled.enabled != 0 &&
           getDisplay( ).getBuffer( ) != nullptr;
}

void paint( int page, bool force ) {
    if ( !panelUsable( ) ) return;
    Adafruit_SSD1306& d = getDisplay( );
    d.clearDisplay( );
    d.setTextWrap( false );
    d.setTextSize( 1 );
    d.setTextColor( SSD1306_WHITE );
    s_y0 = ( oled.displayHeight > 32 ) ? ( oled.displayHeight - 32 ) / 2 : 0;

    char line[ 96 ] = "";
    kPages[ page ].draw( line, sizeof( line ) );
    drawPager( page, StatsPages::count( ) );

    // Hand the GFX font back the way the rest of the firmware left it.
    d.setFont( oled.currentFont );
    d.setTextSize( oled.currentTextSize );

    // Only a frame that CHANGED goes out: a flush is a blocking ~12 ms I2C
    // write, and most refreshes of a settled board redraw the same pixels.
    uint32_t h = fnv1a( d.getBuffer( ), (size_t)oled.displayWidth * (size_t)oled.displayHeight / 8 );
    if ( force || h != s_frameHash ) {
        s_frameHash = h;
        oled.priorityFlushHeldFrame( );
    }
    uint32_t lh = fnv1a( (const uint8_t*)line, strlen( line ) );
    if ( force || lh != s_lineHash ) {
        s_lineHash = lh;
        ReadingDisplay::emitLiveSerialLine( line );
    }
}

// The logo stop. `startup` = what the board shows at boot (the configured
// startup image or message); otherwise the plain idle logo.
void showLogo( bool startup ) {
    const char* msg = jumperlessConfig.top_oled.startup_message;
    if ( startup && msg[ 0 ] != '\0' && !looksLikeFilePath( msg ) ) {
        oled.clearPrintShow( msg, 2, true, true, true );
        return;
    }
    if ( startup ) startupDisplayed = 0; // showJogo32h loads the startup bitmap only while this is 0
    oled.showJogo32h( );
}

}  // namespace

namespace StatsPages {

int count( void ) {
    if ( !panelUsable( ) ) return 0; // nothing to show them on: no dead detents
    switch ( jumperlessConfig.clickwheel.stats_pages ) {
    case MODE_OFF:  return 1;             // the logo
    case MODE_LOGO: return kNumPages + 1; // the pages, then the startup logo
    default:        return kNumPages;
    }
}

void hide( void ) { s_live = -1; }

void show( int page ) {
    int n = count( );
    if ( page < 0 || page >= n ) return;
    int mode = jumperlessConfig.clickwheel.stats_pages;
    bool isLogo = ( mode == MODE_OFF ) || page >= kNumPages;

    // The stop's trace on the pinned status row, the PARTSEL idiom - the
    // terminal's view of where the wheel is. Before the paint: it blanks the
    // reading row, which the page's own line then fills.
    char status[ 48 ];
    snprintf( status, sizeof( status ), "STATS page=%s %d/%d", isLogo ? "logo" : kPages[ page ].name, page + 1, n );
    ReadingDisplay::emitLiveStatusLine( status );

    // A persistent idle screen steps aside like it does for any other paint
    // (this also runs hide(), so it has to come before s_live is set).
    OledGui::getInstance( ).notePanelTakenByOther( );
    if ( isLogo ) {
        s_live = -1; // a still image: nothing to refresh
        showLogo( mode == MODE_LOGO );
        return;
    }
    s_live = (int8_t)page;
    s_wasHeld = false;
    s_lastPaintMs = millis( );
    paint( page, true ); // a landing always paints, over a toast too - like every reading card
}

void service( void ) {
    if ( s_live < 0 ) return;
    unsigned long now = millis( );
    if ( now - s_lastPaintMs < kRefreshMs ) return;
    if ( s_live >= count( ) || jumperlessConfig.clickwheel.stats_pages == MODE_OFF ) {
        hide( ); // the setting changed under the page, or the panel went away
        return;
    }
    // A modal UI or a probe session owns the panel now (OledGui's gates).
    ContextType ctx = contextManager.currentContext( );
    if ( jOS.isUiModal( ) || probeActive || ctx == ContextType::FILE_MANAGER ||
         ctx == ContextType::EKILO_EDITOR || ctx == ContextType::HELP_DOCS ||
         ctx == ContextType::DEBUG_MENU || ctx == ContextType::PROBING ||
         ctx == ContextType::CLICKWHEEL_MENU ) {
        hide( );
        return;
    }
    // A script's oledgui screen is up (or the persistent idle screen took the
    // panel back): its render path is OledGui's own and never announces
    // itself the way a toast or a reading card does.
    OledGui& gui = OledGui::getInstance( );
    if ( gui.active( ) != nullptr && ( gui.active( ) != gui.idleScreen( ) || gui.ownsPanel( ) ) ) {
        hide( );
        return;
    }
    // A toast is up: leave it alone, and repaint once it lets go - the hold's
    // release puts the pre-toast frame back, which is not this page's.
    if ( oled.oledIsHeld( ) ) {
        s_wasHeld = true;
        return;
    }
    // Never mid-refresh, never while core 1 is on the shared hardware, never
    // while the wavegen streams the DAC on the panel's I2C bus.
    if ( refreshInProgress || refreshLocalInProgress || core1busy || core2busy || wavegen.isRunning( ) ) return;

    s_lastPaintMs = now;
    paint( s_live, s_wasHeld );
    s_wasHeld = false;
}

}  // namespace StatsPages

#endif  // OG_JUMPERLESS
