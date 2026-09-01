// Host-side self-check for the SIP module record anchor in
// src/PartsApp.cpp (partsSipAnchor). anchor() below mirrors that function
// exactly - if you change one, change both.
//
// Run:  cc -o /tmp/sip_anchor_check sip_anchor_check.c && /tmp/sip_anchor_check
//
// The contract: the measured power rows are the truth. A SIP record fits a
// probed cluster only when its own GND/VCC pin offsets (og/ov, 0-based)
// land EXACTLY on the measured rows in one direction, the whole run stays
// on the half (x-pin columns included - a leg can sit on 29/30), and the
// run covers every probed row [lo..hi]. At most one direction can solve
// (both would need og == ov, and no record has one pin as both rails), so
// a fit fixes pin 1's row AND the direction - no pin-1 tap needed.

#include <assert.h>
#include <stdio.h>

static int anchor( int og, int ov, int gndRow, int vddRow, int pinCount,
                   int lo, int hi, int halfLo, int halfHi, int *pin1Out,
                   int *dirOut ) {
    for ( int dir = 1; dir >= -1; dir -= 2 ) {
        int p1 = gndRow - dir * og;
        if ( p1 + dir * ov != vddRow ) continue;
        int eLo = ( dir > 0 ) ? p1 : p1 - ( pinCount - 1 );
        int eHi = eLo + pinCount - 1;
        if ( eLo < halfLo || eHi > halfHi ) continue;
        if ( eLo > lo || eHi < hi ) continue;
        *pin1Out = p1;
        *dirOut = dir;
        return 1;
    }
    return 0;
}

int main( void ) {
    int p1, dir;

    // The bench case (2026-08-30): ST7789 sip7 [GND VCC SCL SDA RES DC BL]
    // on rows 22-28, census face 22/23/24, rails gnd 22 vdd 23. Record
    // og=0 ov=1 - ascending, pin 1 at 22, extent 22-28 covers the cluster
    // even before the member probe grows it.
    assert( anchor( 0, 1, 22, 23, 7, 22, 24, 1, 30, &p1, &dir ) );
    assert( p1 == 22 && dir == 1 );

    // Same module plugged flipped (BL at 22, GND at 28): rails 28/27.
    assert( anchor( 0, 1, 28, 27, 7, 26, 28, 1, 30, &p1, &dir ) );
    assert( p1 == 28 && dir == -1 );

    // A grown cluster excludes short records: sip4 (SSD1306, og=0 ov=1)
    // cannot cover a cluster probed out to row 27.
    assert( !anchor( 0, 1, 22, 23, 4, 22, 27, 1, 30, &p1, &dir ) );
    // ...but fits the ungrown 3-row face - the picker's job, not the math's.
    assert( anchor( 0, 1, 22, 23, 4, 22, 24, 1, 30, &p1, &dir ) );
    assert( p1 == 22 && dir == 1 );

    // Power pins are NOT always at the pin-1 end: ws2812 sip3 [DIN VCC GND]
    // (og=2 ov=1) with gnd 24 / vdd 23 sits DIN-first ascending from 22.
    assert( anchor( 2, 1, 24, 23, 3, 22, 24, 1, 30, &p1, &dir ) );
    assert( p1 == 22 && dir == 1 );

    // VCC-first records (ili9341 sip9 [VCC GND ...], og=1 ov=0).
    assert( anchor( 1, 0, 23, 22, 9, 22, 27, 1, 30, &p1, &dir ) );
    assert( p1 == 22 && dir == 1 );

    // The run may reach onto the x-pin columns (29/30 hold legs)...
    assert( anchor( 0, 1, 23, 24, 7, 23, 28, 1, 30, &p1, &dir ) );
    assert( p1 == 23 && dir == 1 ); // extent 23-29
    // ...but never across the ravine: sip16 anchored at 22 runs past 30.
    assert( !anchor( 0, 1, 22, 23, 16, 22, 24, 1, 30, &p1, &dir ) );

    // Bottom half uses its own band.
    assert( anchor( 0, 1, 51, 52, 7, 51, 53, 31, 60, &p1, &dir ) );
    assert( p1 == 51 && dir == 1 );
    assert( !anchor( 0, 1, 51, 52, 7, 51, 53, 1, 30, &p1, &dir ) );

    // Swapped rails (the clamp map misread gnd/vdd) do not fit - the
    // record's GND must land on the measured gnd row, exactly like the
    // DIP orient rule. Honest refusal, not a guess.
    assert( !anchor( 0, 1, 23, 22, 7, 22, 24, 1, 30, &p1, &dir ) );

    // Rails 2 apart never fit adjacent power pins.
    assert( !anchor( 0, 1, 22, 24, 7, 22, 24, 1, 30, &p1, &dir ) );

    printf( "sip_anchor_check: PASS\n" );
    return 0;
}
