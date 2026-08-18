// SPDX-License-Identifier: MIT
#include "XbarLatency.h"
#include "hardware/timer.h"
#include "pico/platform.h" // __not_in_flash_func: sendPaths() runs from RAM (T1.8)

XbarLatencyStats xbarLat = { };

static inline void seg( XbarLatSeg& s, uint32_t us ) {
    s.lastUs = us;
    if ( us > s.maxUs ) s.maxUs = us;
    s.n++;
}

void xbarLatTap( void ) {
    xbarLat.tTap = time_us_32( );
    xbarLat.tapOutstanding = true;
}

void xbarLatRequest( void ) {
    if ( xbarLat.reqOutstanding ) return; // keep the first request's stamp
    uint32_t now = time_us_32( );
    xbarLat.tReq = now;
    xbarLat.pickedUp = false;
    xbarLat.reqOutstanding = true;
    if ( xbarLat.tapOutstanding ) {
        xbarLat.tapOutstanding = false;
        seg( xbarLat.tapReq, now - xbarLat.tTap );
    }
}

void __not_in_flash_func( xbarLatPickup )( void ) {
    if ( !xbarLat.reqOutstanding || xbarLat.pickedUp ) return;
    uint32_t now = time_us_32( );
    xbarLat.tPickup = now;
    xbarLat.pickedUp = true;
    seg( xbarLat.reqPickup, now - xbarLat.tReq );
}

void __not_in_flash_func( xbarLatSendDone )( void ) {
    if ( !xbarLat.reqOutstanding ) return;
    uint32_t now = time_us_32( );
    if ( xbarLat.pickedUp ) {
        seg( xbarLat.pickupSend, now - xbarLat.tPickup );
    }
    xbarLat.tSendDone = now;
    xbarLat.showPending = true;
    xbarLat.reqOutstanding = false;
    xbarLat.pickedUp = false;
}

void __not_in_flash_func( xbarLatShow )( void ) {
    if ( !xbarLat.showPending ) return;
    uint32_t now = time_us_32( );
    xbarLat.showPending = false;
    seg( xbarLat.sendShow, now - xbarLat.tSendDone );
    seg( xbarLat.reqShow, now - xbarLat.tReq );
}

void xbarLatReset( void ) {
    XbarLatSeg* segs[] = { &xbarLat.tapReq, &xbarLat.reqPickup, &xbarLat.pickupSend,
                           &xbarLat.sendShow, &xbarLat.reqShow };
    for ( XbarLatSeg* s : segs ) {
        s->lastUs = 0;
        s->maxUs = 0;
        s->n = 0;
    }
}

static void one( Stream* t, const char* name, const XbarLatSeg& s ) {
    t->printf( "  %-14s last %8lu us  max %8lu us  n %lu\n\r", name,
               (unsigned long)s.lastUs, (unsigned long)s.maxUs, (unsigned long)s.n );
}

void xbarLatPrint( Stream* target ) {
    if ( target == nullptr ) return;
    target->println( "crossbar latency (tap -> request -> send -> LEDs; X! resets)" );
    one( target, "tap->req", xbarLat.tapReq );
    one( target, "req->pickup", xbarLat.reqPickup );
    one( target, "pickup->send", xbarLat.pickupSend );
    one( target, "send->show", xbarLat.sendShow );
    one( target, "req->show", xbarLat.reqShow );
}
