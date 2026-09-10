// SPDX-License-Identifier: MIT
//
// ScanProbe - the probe of a board whose BoardCaps::scanningProbe is set (the
// OG Jumperless). The probe kit is a needle on PROBE_PIN and a button that
// shorts that line to BUTTON_PIN: no resistive pads, no switch, no LED chip.
//
// Where the needle is gets found the way the original OG firmware did it: the
// needle carries a square-wave tone, every breadboard row, corner row and
// header pin is routed in turn through the crossbar to an RP2040 input (the
// ADC0 / ADC1 pins, read digitally), and the one whose input follows the tone
// is the touch. The crossbar has to be empty for that, so a sweep resets the
// chips; between sweeps the board stays reset (the original OG probe-mode
// behaviour) and Probing::probeExitTail() puts the circuit back with a clean
// resend when the session ends.
//
// Every entry point is a no-op that returns "nothing" on a board without the
// capability, so callers gate once on scanprobe::available() and otherwise
// need no board macro.
#ifndef SCANPROBE_H
#define SCANPROBE_H

#include <stdint.h>

namespace scanprobe {

// BoardCaps::scanningProbe of the running board.
bool available( void );

// What the needle sits on while it is NOT driven: a rail or another hard
// level reads the same under a pull-up and a pull-down; a free row differs.
enum TipLevel { TIP_FLOATING = 0, TIP_HIGH = 1, TIP_LOW = 2 };
TipLevel tipLevel( void );

// The button shorts the needle line to BUTTON_PIN: a tone on the needle that
// shows up on the button line under BOTH pulls is a press (a pull-only
// coupling - the kit LED sits between the two lines - passes one pull, never
// both). ~0.5 ms. Leaves the needle floating and the button line parked.
bool buttonPressed( void );

// Idle: needle input, button line input + pull-down. In a session the button
// line is driven low instead, which lights the kit LED through the tone
// (needle -> LED -> button line), exactly as the original OG firmware did.
void parkPins( bool inSession );

// The sweep, chunked so one probeTick stays short (a group is one chip's seven
// rows, the four corner rows, or half a header chip; ~1-3 ms each):
//   sweepBegin()  - waits for core 1 to be idle, resets the crossbar, reads
//                   the tip. Starts the sweep only when the tip floats; a hard
//                   level is returned instead and nothing is driven into it.
//   sweepStep()   - scans the next group. Returns true when the sweep is done
//                   (results ready, pins restored). Aborts itself - returning
//                   false with sweepActive() false - if core 1 rewrote the
//                   crossbar underneath it.
//   sweepResults()- the nodes that followed the tone (0 = nothing touched).
//   sweepAbort()  - stop early: tone off, ADC pins back to the ADC.
TipLevel sweepBegin( void );
bool     sweepStep( void );
bool     sweepActive( void );
int      sweepResults( int* nodes, int maxNodes );
void     sweepAbort( void );

// Last completed sweep, microseconds (for the diagnostic and the latency ledger).
uint32_t lastSweepUs( void );

// Diagnostic (SingleCharCommands 's r'): route one node to its sense input,
// run the tone and return the raw 4-sample patterns seen under pull-down and
// pull-up (a touch reads 0b0101 / 0b0101). -1 in a slot = no tone edge seen.
// Resets the crossbar; the caller restores the circuit.
void rawPatterns( int node, int* patternDown, int* patternUp );

} // namespace scanprobe

#endif // SCANPROBE_H
