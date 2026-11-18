// The click menu as inline terminal columns. See MenuTermView.h for the tour.

#include "MenuTermView.h"
#include "Tui.h" // fg / bold / dim / inverse / resetColor / rainbowColor / pump

namespace MenuTermView {

namespace {

struct Drawn {
    int16_t id, count, selected, hue;
    int16_t top;    // first visible row (lists longer than the block scroll)
    int16_t x;      // column offset from the left edge
    int16_t width;  // cell width, without the one-column gap after it
    bool active;    // the rightmost column: the one the wheel is in
    bool visible;   // false when it would run past MAX_WIDTH
};

bool s_open = false;
int s_rows = 0;                  // list lines in the block (the hint line is one more)
int s_nDrawn = 0;
Drawn s_drawn[ MAX_COLUMNS ];

const int kLabelMax = 40;
const int kHintColor = 244;      // the config TUI's footer grey
const int kCursorColor = 231;    // ... and its selection bar

// Visible columns of a UTF-8 string: code points, like Tui::printClipped.
int textWidth( const char* s ) {
    int w = 0;
    for ( ; *s; s++ ) {
        if ( ( (uint8_t)*s & 0xC0 ) != 0x80 ) w++;
    }
    return w;
}

void up( Stream& out, int n ) { if ( n > 0 ) { out.print( "\x1b[" ); out.print( n ); out.print( "A" ); } }
void down( Stream& out, int n ) { if ( n > 0 ) { out.print( "\x1b[" ); out.print( n ); out.print( "B" ); } }
void right( Stream& out, int n ) { if ( n > 0 ) { out.print( "\x1b[" ); out.print( n ); out.print( "C" ); } }

// The cursor rests on the block's first line, column 0, between paints.
void gotoCell( Stream& out, int line, int x ) {
    down( out, line );
    out.print( "\r" );
    right( out, x );
}
void park( Stream& out, int line ) {
    out.print( "\r" );
    up( out, line );
}

int rowColor( const Drawn& d, int row ) { return Tui::rainbowColor( d.hue + row ); }

// One cell, cursor already on it: " label   > " as a bar when it is the lit
// row. The wheel's column gets the white bar; the columns it came through
// keep a bar in the row's own colour and dim the rows not taken.
void paintCell( Stream& out, const Drawn& d, int col, int line, LabelFn label, bool eraseRest ) {
    int row = d.top + line;
    if ( row < d.count ) {
        char text[ kLabelMax ];
        text[ 0 ] = '\0';
        bool opens = label( col, row, text, sizeof( text ) );
        bool lit = ( row == d.selected );
        if ( lit ) {
            Tui::inverse( true, &out );
            if ( d.active ) Tui::bold( true, &out );
        } else if ( !d.active ) {
            Tui::dim( true, &out );
        }
        Tui::fg( ( lit && d.active ) ? kCursorColor : rowColor( d, row ), &out );
        out.print( ' ' );
        out.print( text );
        for ( int i = textWidth( text ); i < d.width - 3; i++ ) out.print( ' ' );
        out.print( opens ? " \xE2\x80\xBA" : "  " ); // ›
        Tui::resetColor( &out );
        out.print( ' ' ); // the gap, so a narrower repaint leaves nothing behind
    } else {
        for ( int i = 0; i <= d.width; i++ ) out.print( ' ' );
    }
    if ( eraseRest ) out.print( "\x1b[K" );
}

void paintColumn( Stream& out, const Drawn& d, int col, LabelFn label, bool eraseRest ) {
    for ( int line = 0; line < s_rows; line++ ) {
        gotoCell( out, line, d.x );
        paintCell( out, d, col, line, label, eraseRest );
        park( out, line );
        Tui::pump( ); // a column is more than the CDC TX FIFO holds
    }
}

void paintRow( Stream& out, const Drawn& d, int col, int row, LabelFn label ) {
    int line = row - d.top;
    if ( line < 0 || line >= s_rows ) return;
    gotoCell( out, line, d.x );
    paintCell( out, d, col, line, label, false );
    park( out, line );
}

void eraseFrom( Stream& out, int x ) {
    for ( int line = 0; line < s_rows; line++ ) {
        gotoCell( out, line, x );
        out.print( "\x1b[K" );
        park( out, line );
    }
    Tui::pump( );
}

void paintHint( Stream& out ) {
    gotoCell( out, s_rows, 1 );
    Tui::fg( kHintColor, &out );
    out.print( "\xE2\x86\x91\xE2\x86\x93 move   \xE2\x86\x92 open   \xE2\x86\x90 back   q close" ); // ↑↓ → ←
    Tui::resetColor( &out );
    out.print( "\x1b[K" );
    park( out, s_rows );
}

// Where a column sits and how wide it is: as wide as its longest row.
void layout( Drawn& d, const Column& c, int col, int x, LabelFn label ) {
    d.id = c.id;
    d.count = c.count;
    d.selected = c.selected;
    d.hue = c.hue;
    d.x = x;
    int widest = 0;
    char text[ kLabelMax ];
    for ( int row = 0; row < c.count; row++ ) {
        text[ 0 ] = '\0';
        label( col, row, text, sizeof( text ) );
        int w = textWidth( text );
        if ( w > widest ) widest = w;
    }
    d.width = widest + 3; // a space before, a space and the mark after
    d.top = 0;
    if ( c.count > s_rows ) {
        d.top = c.selected - s_rows / 2;
        if ( d.top > c.count - s_rows ) d.top = c.count - s_rows;
        if ( d.top < 0 ) d.top = 0;
    }
    // ponytail: a column that would cross MAX_WIDTH is not drawn (the engine
    // still moves in it). The shipped tree is ~55 columns at its deepest; the
    // upgrade path is a max depth that shifts the columns left.
    d.visible = ( x + d.width < MAX_WIDTH );
}

} // namespace

bool isOpen( ) { return s_open; }

void show( Stream& out, const Column* cols, int nCols, int rows, LabelFn label ) {
    if ( nCols < 1 || label == nullptr ) return;
    if ( nCols > MAX_COLUMNS ) nCols = MAX_COLUMNS;

    if ( !s_open ) {
        s_rows = rows < 1 ? 1 : ( rows > MAX_ROWS ? MAX_ROWS : rows );
        s_nDrawn = 0;
        // Reserve the block: the newlines scroll the terminal if it has to,
        // then back up to the first line. From here on nothing scrolls.
        out.print( "\r" );
        for ( int i = 0; i < s_rows; i++ ) out.print( "\r\n" );
        up( out, s_rows );
        paintHint( out );
        s_open = true;
    }

    int x = 0;
    bool repaintRest = false; // a column moved or changed: everything right of it is stale
    for ( int c = 0; c < nCols; c++ ) {
        Drawn was = s_drawn[ c ];
        bool had = ( c < s_nDrawn );
        Drawn& d = s_drawn[ c ];
        layout( d, cols[ c ], c, x, label );
        d.active = ( c == nCols - 1 );
        x += d.width + 1;

        bool full = repaintRest || !had || was.id != d.id || was.count != d.count ||
                    was.hue != d.hue || was.x != d.x || was.width != d.width ||
                    was.top != d.top || was.active != d.active || was.visible != d.visible;
        if ( !d.visible ) {
            if ( full ) eraseFrom( out, d.x < MAX_WIDTH ? d.x : MAX_WIDTH - 1 );
            repaintRest = true;
            continue;
        }
        if ( full ) {
            paintColumn( out, d, c, label, /*eraseRest=*/c == nCols - 1 );
            repaintRest = true;
        } else if ( was.selected != d.selected ) {
            paintRow( out, d, c, was.selected, label ); // the row we left, unlit now
            paintRow( out, d, c, d.selected, label );
        }
    }
    // Backed out of a submenu and nothing left of it was repainted.
    if ( nCols < s_nDrawn && !repaintRest ) eraseFrom( out, x );
    s_nDrawn = nCols;
    out.flush( );
}

void close( Stream& out, const Column* cols, int nCols, LabelFn label ) {
    if ( !s_open ) return;
    for ( int line = 0; line <= s_rows; line++ ) {
        out.print( "\r\x1b[2K" );
        if ( line < s_rows ) down( out, 1 );
    }
    park( out, s_rows );
    Tui::pump( );
    s_open = false;
    s_nDrawn = 0;

    if ( nCols > MAX_COLUMNS ) nCols = MAX_COLUMNS;
    if ( nCols > 0 && label != nullptr ) {
        char text[ kLabelMax ];
        for ( int c = 0; c < nCols; c++ ) {
            if ( c > 0 ) {
                Tui::fg( kHintColor, &out );
                out.print( " > " );
            }
            text[ 0 ] = '\0';
            label( c, cols[ c ].selected, text, sizeof( text ) );
            Tui::fg( Tui::rainbowColor( cols[ c ].hue + cols[ c ].selected ), &out );
            out.print( text );
        }
        Tui::resetColor( &out );
        out.print( "\r\n" );
    }
    out.flush( );
}

} // namespace MenuTermView
