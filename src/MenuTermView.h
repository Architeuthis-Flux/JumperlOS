// ============================================================================
// MenuTermView - the click menu drawn as columns, inline in the terminal.
//
// The root list stays on the left and each submenu entered adds a column to
// its right. Nothing here walks the menu: Menus.cpp's wheel engine does, and
// hands over what it is showing (one Column per level, plus a callback for
// the row text). This file only lays that out and keeps the terminal in step.
//
// Inline means no alternate screen and no clear: the block is reserved under
// the cursor once, repainted in place with relative cursor moves (CR, CUU /
// CUD / CUF, erase-to-EOL - no ESC[s / ESC[u, which Terminal.app ignores),
// and on close it is erased down to one line holding the picked path in the
// colours it had in the menu, so the pick stays readable in the scrollback.
//
// No text is stored: rows are asked for again whenever they are painted, so
// the state is a few ints per column (the OG's heap floor is tight).
// ============================================================================
#pragma once

#include <Arduino.h>

namespace MenuTermView {

struct Column {
    int16_t id;        // which list this is - a different id repaints the column
    int16_t count;     // rows in the list
    int16_t selected;  // the lit row, 0..count-1
    int16_t hue;       // rainbow index of row 0: a submenu carries on from its parent's colour
};

// Text of one row into `out` (UTF-8, NUL-terminated). True when the row leads
// somewhere (a submenu, or a value to set), which draws a trailing mark.
typedef bool ( *LabelFn )( int col, int row, char* out, int outLen );

const int MAX_COLUMNS = 8;
const int MAX_ROWS = 16;    // list rows in the block; longer lists scroll
const int MAX_WIDTH = 80;   // assumed terminal width (nothing answers a size probe through the app)

// Draw the columns, or bring what is on screen up to date with them. The
// first call reserves `rows` list lines plus a hint line under the cursor.
void show( Stream& out, const Column* cols, int nCols, int rows, LabelFn label );

// Erase the block. With nCols > 0 the picked path is left on one line
// ("OLED > Show in Term > On"); with 0 nothing is.
void close( Stream& out, const Column* cols, int nCols, LabelFn label );

bool isOpen( );

} // namespace MenuTermView
