// SPDX-License-Identifier: MIT
#ifndef KICKGAP_H
#define KICKGAP_H

// ---------------------------------------------------------------------------
// Watchdog, measure-only stage (C11 / T1.6, SCHEDULER_AND_HARDWARE_OFFLOAD.md).
//
// No watchdog is enabled here. This records where the watchdog kicks WOULD go
// - the top of loop()'s busy-loop pass on core 0, jOS.serviceInner() (the
// modal loops), the top of loop1() on core 1 - and the longest gap between
// consecutive stamps on each core, with which two sites bracketed it and when.
// `X` prints it; `X!` resets the maxima so one blocker at a time can be timed
// (a flash write, a wavegen stream, a compute-bound MicroPython script, ...).
// The enable decision (timeout, which loops must kick, WaveGen's core-1
// capture) is made on these numbers, not before.
//
// Cost: one time_us_32() read and a few compares per stamp. Each core writes
// only its own slot; core 0 reads core 1's slot for the printout (a stat, so
// the odd torn read is fine).
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <Arduino.h>  // Stream (for kickGapPrint)
#include "hardware/timer.h"

enum KickSite : uint8_t {
    KICK_LOOP0 = 0,   // top of loop()'s busy-loop pass (core 0)
    KICK_INNER = 1,   // jOS.serviceInner() - inside the modal loops (core 0)
    KICK_LOOP1 = 2,   // top of loop1() (core 1)
    KICK_VM = 3,      // MicroPython VM loop hook, mp_hal_check_interrupt() (core 0, ~1 ms throttle)
    KICK_WAVEGEN = 4, // WaveGen's core-1 streaming loop, once per DAC sample (core 1)
    KICK_SITE_COUNT = 5
};

struct KickGapStats {
    uint32_t lastUs;      // time_us_32() of the last stamp
    uint32_t maxGapUs;    // longest gap between two consecutive stamps
    uint32_t maxAtMs;     // millis() when that max was observed
    uint32_t stamps;      // stamps since boot (wraps)
    uint8_t  maxFromSite; // the stamp before the max gap
    uint8_t  maxToSite;   // the stamp that closed the max gap
    uint8_t  lastSite;
    bool     armed;       // first stamp seen (boot init is not a gap)
};

extern KickGapStats kickGap[ 2 ]; // [core]

static inline void kickGapStamp( uint8_t core, uint8_t site ) {
    KickGapStats& k = kickGap[ core ];
    uint32_t now = time_us_32( );
    if ( k.armed ) {
        uint32_t gap = now - k.lastUs;
        if ( gap > k.maxGapUs ) {
            k.maxGapUs = gap;
            k.maxFromSite = k.lastSite;
            k.maxToSite = site;
            k.maxAtMs = (uint32_t)( time_us_64( ) / 1000u ); // only on a new max
        }
    }
    k.armed = true;
    k.lastUs = now;
    k.lastSite = site;
    k.stamps++;
}

// Reset both cores' maxima (keeps the stamp counters and the last stamp).
void kickGapReset( void );

// One line per core, for X.
void kickGapPrint( Stream* target );

#endif
