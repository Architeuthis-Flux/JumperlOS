// SPDX-License-Identifier: MIT
//
// ScanProbe - see ScanProbe.h. Ported from the OG reference firmware's
// scanRows() / readFloatingOrState() / checkProbeButton()
// (Jumperless/JumperlessNano/src/Probing.cpp), driven from the board topology
// tables instead of the reference's literal chip and lane numbers, chunked
// into groups so a probe tick stays short, and bounded everywhere the
// reference spun forever.

#include "ScanProbe.h"

#include <Arduino.h>
#include <algorithm>
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

#include "JumperlessDefines.h"
#include "boards/board.h"
#include "CH446Q.h"      // sendXYraw
#include "Commands.h"    // waitCore2
#include "RouteSafety.h" // routingGeneration, markChipXYSuspect

namespace scanprobe {

// Tones. 25 kHz for the rows (the reference's value; the LM324 that buffers
// the OG's ADC inputs slews 3.3 V in ~7 us, inside the 10 us quarter period
// the samples sit at), 10 kHz for the button (the reference's value - the
// cable between the two probe lines is longer than any row).
static const uint32_t kRowToneHz    = 25000;
static const uint32_t kButtonToneHz = 10000;
// A crosspoint send that has not completed in this long marks its chip
// suspect and moves on: a sweep must never hang core 0 on a wedged handshake.
static const unsigned long kSendTimeoutUs = 5000;
static const int kMaxFound   = 8;
static const int kGroupCount = 13; // 8 breadboard chips, the corners, 2 halves x 2 header chips

static bool     s_active = false;
static int      s_group = 0;
static int      s_found[ kMaxFound ];
static int      s_foundCount = 0;
static uint32_t s_generation = 0;
static uint32_t s_sweepStartUs = 0;
static uint32_t s_lastSweepUs = 0;
static bool     s_sessionLed = false;

bool available( void ) { return board::currentBoard( ).caps.scanningProbe; }

// --- pins --------------------------------------------------------------------

// The tone is driven with the SDK's PWM calls, not analogWrite(): the Arduino
// layer's analogWriteFreq() is one global frequency for every PWM pin, so a
// 10 / 25 kHz tone would silently retune anyone else's analogWrite().
static void toneStart( uint32_t hz ) {
    const uint slice = pwm_gpio_to_slice_num( PROBE_PIN );
    const uint chan = pwm_gpio_to_channel( PROBE_PIN );
    const uint32_t wrap = 999; // 1000 counts per period
    float div = (float)clock_get_hz( clk_sys ) / ( (float)hz * (float)( wrap + 1 ) );
    if ( div < 1.0f ) div = 1.0f;
    if ( div > 255.0f ) div = 255.0f;
    pwm_set_enabled( slice, false );
    pwm_set_clkdiv( slice, div );
    pwm_set_wrap( slice, (uint16_t)wrap );
    pwm_set_chan_level( slice, chan, (uint16_t)( ( wrap + 1 ) / 2 ) ); // 50 % duty
    pwm_set_counter( slice, 0 );
    pwm_set_enabled( slice, true );
    gpio_set_drive_strength( PROBE_PIN, GPIO_DRIVE_STRENGTH_4MA );
    gpio_set_function( PROBE_PIN, GPIO_FUNC_PWM );
}

// SIO input, no pulls: the needle floats. The slice is stopped too - GPIO 18
// (the button line) is the same slice's other channel.
static void toneStop( void ) {
    gpio_set_function( PROBE_PIN, GPIO_FUNC_SIO );
    gpio_set_dir( PROBE_PIN, false );
    gpio_disable_pulls( PROBE_PIN );
    gpio_set_input_enabled( PROBE_PIN, true );
    pwm_set_enabled( pwm_gpio_to_slice_num( PROBE_PIN ), false );
}

static void asDigitalInput( uint pin, bool pullDown ) {
    gpio_set_function( pin, GPIO_FUNC_SIO );
    gpio_set_dir( pin, false );
    gpio_set_input_enabled( pin, true );
    gpio_set_pulls( pin, !pullDown, pullDown );
}

// Back to the ADC (adc_gpio_init: NULL function, pulls off, input buffer off)
// so readAdc() sees the buffers again.
static void adcPinRestore( uint pin ) { adc_gpio_init( pin ); }

static void parkButtonPin( void ) {
    gpio_set_function( BUTTON_PIN, GPIO_FUNC_SIO );
    gpio_set_input_enabled( BUTTON_PIN, true );
    if ( s_sessionLed ) {
        gpio_disable_pulls( BUTTON_PIN );
        gpio_set_drive_strength( BUTTON_PIN, GPIO_DRIVE_STRENGTH_4MA );
        gpio_put( BUTTON_PIN, false );
        gpio_set_dir( BUTTON_PIN, true );
    } else {
        gpio_set_dir( BUTTON_PIN, false );
        gpio_set_pulls( BUTTON_PIN, false, true );
    }
}

void parkPins( bool inSession ) {
    if ( !available( ) ) return;
    s_sessionLed = inSession;
    toneStop( );
    parkButtonPin( );
}

// --- the tone test -------------------------------------------------------------

// Wait for the tone's falling edge on the needle (gpio_get reads the pad
// whatever its function). Bounded: no tone means no wait.
static bool alignToFallingEdge( uint32_t periodUs ) {
    uint32_t t0 = time_us_32( );
    while ( !gpio_get( PROBE_PIN ) ) {
        if ( time_us_32( ) - t0 > 3 * periodUs ) return false;
    }
    while ( gpio_get( PROBE_PIN ) ) {
        if ( time_us_32( ) - t0 > 3 * periodUs ) return false;
    }
    return true;
}

// Four samples half a period apart, the first a quarter period after the
// falling edge: a line carrying the tone reads 0,1,0,1.
static const int kTonePattern = 0b0101;

static int samplePattern( uint pin, uint32_t periodUs ) {
    if ( !alignToFallingEdge( periodUs ) ) return -1;
    uint32_t q = periodUs / 4;
    busy_wait_us( q );
    int a = gpio_get( pin );
    busy_wait_us( 2 * q );
    int b = gpio_get( pin );
    busy_wait_us( 2 * q );
    int c = gpio_get( pin );
    busy_wait_us( 2 * q );
    int d = gpio_get( pin );
    return ( a << 3 ) | ( b << 2 ) | ( c << 1 ) | d;
}

// bothPulls: the direct button line has to follow the tone under a pull-down
// AND a pull-up (the kit LED between the lines couples it one way only). A
// row input sits behind the ADC buffer, whose output overrides any pull, so
// one pass is the whole test there.
static bool lineFollowsTone( uint pin, uint32_t hz, bool bothPulls ) {
    uint32_t periodUs = 1000000u / hz;
    gpio_set_pulls( pin, false, true );
    if ( samplePattern( pin, periodUs ) != kTonePattern ) return false;
    if ( bothPulls ) {
        gpio_set_pulls( pin, true, false );
        if ( samplePattern( pin, periodUs ) != kTonePattern ) return false;
    }
    return true;
}

TipLevel tipLevel( void ) {
    if ( !available( ) ) return TIP_FLOATING;
    gpio_set_function( PROBE_PIN, GPIO_FUNC_SIO );
    gpio_set_dir( PROBE_PIN, false );
    gpio_set_input_enabled( PROBE_PIN, true );
    gpio_set_pulls( PROBE_PIN, true, false );
    busy_wait_us( 200 );
    int up = gpio_get( PROBE_PIN );
    gpio_set_pulls( PROBE_PIN, false, true );
    busy_wait_us( 200 );
    int down = gpio_get( PROBE_PIN );
    gpio_disable_pulls( PROBE_PIN );
    if ( up && down ) return TIP_HIGH;
    if ( !up && !down ) return TIP_LOW;
    return TIP_FLOATING;
}

bool buttonPressed( void ) {
    if ( !available( ) ) return false;
    // The button line is an input BEFORE the tone starts: a pressed button
    // shorts it to the needle, and a driven line would fight the tone.
    asDigitalInput( BUTTON_PIN, true );
    toneStart( kButtonToneHz );
    bool pressed = lineFollowsTone( BUTTON_PIN, kButtonToneHz, true );
    toneStop( );
    parkButtonPin( );
    return pressed;
}

// --- topology lookups ------------------------------------------------------------

static int laneOf( int chip, int node ) {
    const board::BoardTopology& b = board::currentBoard( );
    for ( int x = 0; x < board::kXLanes; x++ ) {
        if ( b.xMap[ chip ][ x ] == node ) return x;
    }
    return -1;
}

static bool isBreadboardRow( int node ) { return node >= 1 && node <= 60; }

// --- the sweep ---------------------------------------------------------------------

static void crossbarReset( void ) {
    digitalWrite( RESETPIN, HIGH );
    busy_wait_us( 600 );
    digitalWrite( RESETPIN, LOW );
    busy_wait_us( 50 );
    // The shadow now lies about every chip. (A no-op on the OG build, whose
    // RouteSafety is stubbed - which is why the session forces a clean resend
    // at exit instead of relying on this.)
    for ( int c = 0; c < board::kChipCount; c++ ) markChipXYSuspect( c );
}

// Every crosspoint the sweep touches goes through here.
static void xy( int chip, int x, int y, int setOrClear ) {
    sendXYraw( chip, x, y, setOrClear, kSendTimeoutUs );
}

static void addFound( int node ) {
    if ( node <= 0 ) return;
    for ( int i = 0; i < s_foundCount; i++ ) {
        if ( s_found[ i ] == node ) return;
    }
    if ( s_foundCount < kMaxFound ) s_found[ s_foundCount++ ] = node;
}

static void finishSweep( void ) {
    toneStop( );
    adcPinRestore( ADC0_PIN );
    adcPinRestore( ADC1_PIN );
    s_active = false;
}

TipLevel sweepBegin( void ) {
    if ( !available( ) ) return TIP_FLOATING;
    if ( s_active ) finishSweep( );
    // No raw send may overlap a core-1 send (the OG's single-send token is
    // not locked). Nothing posts a send while a probe session owns core 0,
    // so idle now is idle for the whole sweep.
    waitCore2( );
    crossbarReset( );
    TipLevel tip = tipLevel( );
    if ( tip != TIP_FLOATING ) return tip; // a rail: report it, never drive it
    asDigitalInput( ADC0_PIN, true );
    asDigitalInput( ADC1_PIN, true );
    s_group = 0;
    s_foundCount = 0;
    s_generation = routingGeneration;
    s_sweepStartUs = time_us_32( );
    s_active = true;
    return TIP_FLOATING;
}

bool sweepActive( void ) { return s_active; }

void sweepAbort( void ) {
    if ( s_active ) finishSweep( );
}

// One group. Breadboard chip c: chip L bridges its ADC0 lane to L's Y[c] (that
// chip's Y0, the hub lane); the chip bridges its X0 to Y0 and to the row - X0
// is a spare lane inside the chip while the crossbar is empty. Corners: L's
// ADC0 lane and the corner-row lane both onto L's Y0. Header chips: the pin
// lane and the chip's own ADC lane (ADC0 on I, ADC1 on J) both onto Y0, read
// on that ADC's pin.
bool sweepStep( void ) {
    if ( !s_active ) return false;
    if ( routingGeneration != s_generation ) {
        // core 1 rewrote the crossbar under us (a connection just landed):
        // what we scanned since is unreliable - start over next tick.
        finishSweep( );
        return false;
    }
    const board::BoardTopology& b = board::currentBoard( );
    const int adcLaneL = laneOf( CHIP_L, ADC0 );
    toneStart( kRowToneHz );

    if ( s_group < 8 ) {
        int chip = s_group; // CHIP_A .. CHIP_H
        if ( adcLaneL >= 0 ) {
            xy( CHIP_L, adcLaneL, chip, 1 );
            for ( int y = 1; y < board::kYLanes; y++ ) {
                int node = b.yMap[ chip ][ y ];
                if ( !isBreadboardRow( node ) ) continue;
                xy( chip, 0, 0, 1 );
                xy( chip, 0, y, 1 );
                if ( lineFollowsTone( ADC0_PIN, kRowToneHz, false ) ) addFound( node );
                xy( chip, 0, 0, 0 );
                xy( chip, 0, y, 0 );
            }
            xy( CHIP_L, adcLaneL, chip, 0 );
        }
    } else if ( s_group == 8 ) {
        if ( adcLaneL >= 0 ) {
            xy( CHIP_L, adcLaneL, 0, 1 );
            for ( int x = 0; x < board::kXLanes; x++ ) {
                int node = b.xMap[ CHIP_L ][ x ];
                if ( !isBreadboardRow( node ) ) continue;
                xy( CHIP_L, x, 0, 1 );
                if ( lineFollowsTone( ADC0_PIN, kRowToneHz, false ) ) addFound( node );
                xy( CHIP_L, x, 0, 0 );
            }
            xy( CHIP_L, adcLaneL, 0, 0 );
        }
    } else {
        int half = s_group - 9; // 0,1 = chip I halves; 2,3 = chip J halves
        int chip = ( half < 2 ) ? CHIP_I : CHIP_J;
        int adcNode = ( chip == CHIP_I ) ? ADC0 : ADC1;
        uint pin = ( chip == CHIP_I ) ? ADC0_PIN : ADC1_PIN;
        int adcLane = laneOf( chip, adcNode );
        int x0 = ( half & 1 ) ? 6 : 0;
        if ( adcLane >= 0 ) {
            xy( chip, adcLane, 0, 1 );
            for ( int x = x0; x < x0 + 6 && x < board::kXLanes; x++ ) {
                int node = b.xMap[ chip ][ x ];
                if ( node <= 0 || node == adcNode ) continue;
                xy( chip, x, 0, 1 );
                if ( lineFollowsTone( pin, kRowToneHz, false ) ) addFound( node );
                xy( chip, x, 0, 0 );
            }
            xy( chip, adcLane, 0, 0 );
        }
    }

    toneStop( );
    s_group++;
    if ( s_group >= kGroupCount ) {
        s_lastSweepUs = time_us_32( ) - s_sweepStartUs;
        std::sort( s_found, s_found + s_foundCount ); // rows first, lowest first
        finishSweep( );
        return true;
    }
    return false;
}

int sweepResults( int* nodes, int maxNodes ) {
    int n = 0;
    for ( int i = 0; i < s_foundCount && n < maxNodes; i++ ) nodes[ n++ ] = s_found[ i ];
    return n;
}

uint32_t lastSweepUs( void ) { return s_lastSweepUs; }

// --- diagnostic ----------------------------------------------------------------------

void rawPatterns( int node, int* patternDown, int* patternUp ) {
    *patternDown = -1;
    *patternUp = -1;
    if ( !available( ) ) return;
    const board::BoardTopology& b = board::currentBoard( );
    if ( s_active ) finishSweep( );
    waitCore2( );
    crossbarReset( );

    // Route the node the way the sweep does and remember how to undo it.
    int sends[ 4 ][ 3 ];
    int nSends = 0;
    uint pin = ADC0_PIN;
    int adcLaneL = laneOf( CHIP_L, ADC0 );
    if ( isBreadboardRow( node ) ) {
        int cornerLane = laneOf( CHIP_L, node );
        if ( cornerLane >= 0 ) {
            sends[ nSends ][ 0 ] = CHIP_L; sends[ nSends ][ 1 ] = adcLaneL; sends[ nSends ][ 2 ] = 0; nSends++;
            sends[ nSends ][ 0 ] = CHIP_L; sends[ nSends ][ 1 ] = cornerLane; sends[ nSends ][ 2 ] = 0; nSends++;
        } else {
            for ( int chip = 0; chip < 8 && nSends == 0; chip++ ) {
                for ( int y = 1; y < board::kYLanes; y++ ) {
                    if ( b.yMap[ chip ][ y ] != node ) continue;
                    sends[ nSends ][ 0 ] = CHIP_L; sends[ nSends ][ 1 ] = adcLaneL; sends[ nSends ][ 2 ] = chip; nSends++;
                    sends[ nSends ][ 0 ] = chip; sends[ nSends ][ 1 ] = 0; sends[ nSends ][ 2 ] = 0; nSends++;
                    sends[ nSends ][ 0 ] = chip; sends[ nSends ][ 1 ] = 0; sends[ nSends ][ 2 ] = y; nSends++;
                    break;
                }
            }
        }
    } else {
        for ( int chip = CHIP_I; chip <= CHIP_J && nSends == 0; chip++ ) {
            int lane = laneOf( chip, node );
            if ( lane < 0 ) continue;
            int adcNode = ( chip == CHIP_I ) ? ADC0 : ADC1;
            pin = ( chip == CHIP_I ) ? ADC0_PIN : ADC1_PIN;
            sends[ nSends ][ 0 ] = chip; sends[ nSends ][ 1 ] = laneOf( chip, adcNode ); sends[ nSends ][ 2 ] = 0; nSends++;
            sends[ nSends ][ 0 ] = chip; sends[ nSends ][ 1 ] = lane; sends[ nSends ][ 2 ] = 0; nSends++;
        }
    }
    if ( nSends == 0 ) return;

    asDigitalInput( pin, true );
    toneStart( kRowToneHz );
    for ( int i = 0; i < nSends; i++ ) xy( sends[ i ][ 0 ], sends[ i ][ 1 ], sends[ i ][ 2 ], 1 );
    uint32_t periodUs = 1000000u / kRowToneHz;
    gpio_set_pulls( pin, false, true );
    *patternDown = samplePattern( pin, periodUs );
    gpio_set_pulls( pin, true, false );
    *patternUp = samplePattern( pin, periodUs );
    for ( int i = 0; i < nSends; i++ ) xy( sends[ i ][ 0 ], sends[ i ][ 1 ], sends[ i ][ 2 ], 0 );
    toneStop( );
    adcPinRestore( pin );
}

} // namespace scanprobe
