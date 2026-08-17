// SPDX-License-Identifier: MIT
#ifndef XBARLATENCY_H
#define XBARLATENCY_H

// ---------------------------------------------------------------------------
// Tap -> crossbar -> LEDs latency probe (section F of
// SCHEDULER_AND_HARDWARE_OFFLOAD.md; the gate for T2.2 / D2 and T2.3 / C2a).
//
// Five stamps along the path a connection takes from the user's finger to the
// board, and the last / max of each segment between them:
//
//   tap      probeMode() accepts a tap and commits it (core 0)
//   req      core 0 asks core 1 to send paths (sendAllPathsCore2 = +-1 / 3)
//   pickup   the send starts (top of sendPaths(), whichever core runs it)
//   sendDone sendPaths() done (crossbar matches the netlist)
//   show     the first LED frame shown after that (core 1, leds.show())
//
// Coalescing: a request that lands while one is outstanding keeps the FIRST
// request's stamp (that is the one that waited longest); a show is credited
// only if a send completed since the last show (an animation frame in between
// under-reports the send->show segment slightly - a probe, not a proof).
// Each stamp is one time_us_32() and a few compares. Core 0 writes tap/req,
// core 1 writes pickup/sendDone/show (or core 0, when it sends directly);
// 32-bit aligned words, so a torn read cannot happen. X prints it; X! resets.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <Arduino.h>

struct XbarLatSeg {
    uint32_t lastUs;
    uint32_t maxUs;
    uint32_t n;
};

struct XbarLatencyStats {
    uint32_t tTap, tReq, tPickup, tSendDone;
    volatile bool tapOutstanding;   // a tap was accepted, its request not yet seen
    volatile bool reqOutstanding;   // a request was made, its send not yet done
    volatile bool pickedUp;         // the outstanding request's send has started
    volatile bool showPending;      // a send finished, its LED show not yet seen
    XbarLatSeg tapReq, reqPickup, pickupSend, sendShow, reqShow;
};

extern XbarLatencyStats xbarLat;

void xbarLatTap( void );       // probeMode commit (core 0)
void xbarLatRequest( void );   // sendAllPathsCore2 written (core 0)
void xbarLatPickup( void );    // top of sendPaths()
void xbarLatSendDone( void );  // end of sendPaths()
void xbarLatShow( void );      // after leds.show() (core 1)
void xbarLatReset( void );     // X!
void xbarLatPrint( Stream* target );

#endif
