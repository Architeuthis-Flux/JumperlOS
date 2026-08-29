// Host-side self-check for the detent rest re-anchor in
// src/RotaryEncoder.cpp (rotaryEncoderStuffLocked, motion-out-of-rest
// branch). anchor() below mirrors that block's arithmetic exactly - if you
// change one, change both.
//
// Run:  cc -o /tmp/detent_anchor_check detent_anchor_check.c && /tmp/detent_anchor_check
//
// The contract: a shaft at rest sits in a physical notch. If the hysteresis
// baseline (lastDetentRaw) is within a quarter-divider of that notch, snap
// it onto the notch so trip points fire exactly at notch pull-in. Larger
// residuals are deliberate mid-travel holds and must NOT be swallowed.

#include <assert.h>
#include <stdio.h>

static long anchor( long lastDetentRaw, long restRaw, int rotaryDivider ) {
    if ( rotaryDivider > 1 ) {
        long r = ( restRaw - lastDetentRaw ) % (long)rotaryDivider;
        if ( r < 0 ) r += rotaryDivider;
        long tol = rotaryDivider / 4;
        if ( r <= tol || r >= (long)rotaryDivider - tol ) {
            return restRaw;
        }
    }
    return lastDetentRaw;
}

int main( void ) {
    // divider 8 (the V5 encoder: 8 raw counts per detent), tol = 2.
    // Residuals 0,1,2 and 6,7 snap; 3,4,5 (mid-travel hold) keep.
    for ( long r = 0; r < 8; r++ ) {
        long snapped = anchor( 100, 100 + r, 8 );
        if ( r <= 2 || r >= 6 ) assert( snapped == 100 + r );
        else                    assert( snapped == 100 );
    }

    // Negative deltas (baseline AHEAD of the rest position) normalize the
    // same way: rest 2 counts behind baseline == residual 6 -> snap.
    assert( anchor( 100, 98, 8 ) == 98 );   // r = 6: snap
    assert( anchor( 100, 96, 8 ) == 100 );  // r = 4: keep
    assert( anchor( 100, 99, 8 ) == 99 );   // r = 7: snap

    // Grid many detents away still only looks at the residual.
    assert( anchor( 0, 8 * 100 + 1, 8 ) == 801 ); // r = 1: snap
    assert( anchor( 0, 8 * 100 + 4, 8 ) == 0 );   // r = 4: keep

    // Drift discharge scenario: grid 2 counts late (baseline 0, notch at -2
    // wrapped forward => rest lands at 6). Snap puts the next trip exactly
    // one full detent (8 counts) from the notch.
    long base = anchor( 0, 6, 8 );
    assert( base == 6 );
    assert( ( 6 + 8 ) - base == 8 ); // next notch trips exactly at pull-in

    // divider 3 (probe cursor): tol = 0, only an exact-grid rest re-anchors.
    assert( anchor( 10, 12, 3 ) == 10 );  // r = 2: keep
    assert( anchor( 10, 13, 3 ) == 13 );  // r = 0: snap
    // divider 16 (history scrubber): tol = 4.
    assert( anchor( 0, 4, 16 ) == 4 );    // r = 4: snap
    assert( anchor( 0, 5, 16 ) == 0 );    // r = 5: keep
    assert( anchor( 0, 12, 16 ) == 12 );  // r = 12: snap
    // divider <= 1 never snaps (guards the modulo).
    assert( anchor( 0, 5, 1 ) == 0 );
    assert( anchor( 0, 5, 0 ) == 0 );

    printf( "detent_anchor_check: PASS\n" );
    return 0;
}
