// SPDX-License-Identifier: MIT
#include "KickGap.h"
#include <Arduino.h>

KickGapStats kickGap[ 2 ] = { };

static const char* siteName( uint8_t s ) {
    switch ( s ) {
    case KICK_LOOP0: return "loop0";
    case KICK_INNER: return "inner";
    case KICK_LOOP1: return "loop1";
    default:         return "?";
    }
}

void kickGapReset( void ) {
    for ( int c = 0; c < 2; c++ ) {
        kickGap[ c ].maxGapUs = 0;
        kickGap[ c ].maxAtMs = 0;
        kickGap[ c ].maxFromSite = 0;
        kickGap[ c ].maxToSite = 0;
    }
}

void kickGapPrint( Stream* target ) {
    if ( target == nullptr ) return;
    target->println( "watchdog (measure-only, nothing enabled): longest gap between would-be kicks" );
    for ( int c = 0; c < 2; c++ ) {
        const KickGapStats& k = kickGap[ c ];
        if ( !k.armed ) {
            target->printf( "  core %d: no stamps yet\n\r", c );
            continue;
        }
        // Include the gap that is still open right now (a core that stopped
        // stamping is exactly the case a watchdog is for).
        uint32_t openUs = time_us_32( ) - k.lastUs;
        target->printf( "  core %d: max %lu.%03lu ms (%s -> %s, at %lu s)  open now %lu.%03lu ms  stamps %lu\n\r",
                        c,
                        (unsigned long)( k.maxGapUs / 1000 ), (unsigned long)( k.maxGapUs % 1000 ),
                        siteName( k.maxFromSite ), siteName( k.maxToSite ),
                        (unsigned long)( k.maxAtMs / 1000 ),
                        (unsigned long)( openUs / 1000 ), (unsigned long)( openUs % 1000 ),
                        (unsigned long)k.stamps );
    }
    target->println( "  (X! resets the maxima)" );
}
