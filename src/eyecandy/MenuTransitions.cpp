// Menu frame transition engine. See MenuTransitions.h for the architecture
// overview and the Core 0 / Core 2 handoff contract.

#include "MenuTransitions.h"
#include "LEDs.h" // ledClass leds, hsvColor, HsvToRaw

MenuTransitionConfig menuTransitionConfig;

// Breadboard region managed by the engine: rows 0-59, 5 pixels per row.
#define MENU_TRANSITION_PIXELS 300

// Frame snapshots. prevFrame is what the transition blends *from*, targetFrame
// is the frame Core 0 just drew (what it blends *to*), accentFrame is the
// target recolored to the selected item's accent hue (same per-pixel
// brightness, accent hue/sat) for the accent transition types. Only touched by
// Core 0 while the drawing flag is up and by Core 2 while renderBusy is up —
// the beginDraw()/render() handshake below keeps those windows disjoint.
static uint32_t prevFrame[ MENU_TRANSITION_PIXELS ];
static uint32_t targetFrame[ MENU_TRANSITION_PIXELS ];
static uint32_t accentFrame[ MENU_TRANSITION_PIXELS ];

// Accent color for the next arm() (selected menu item's ring hue; 0 = none).
static volatile uint32_t mtAccentColor = 0;

// Cross-core handshake state.
static volatile bool mtDrawing = false;        // Core 0 is repainting the live buffer
static volatile unsigned long mtDrawStartMs = 0; // for the stuck-flag timeout
static volatile bool mtRenderBusy = false;     // Core 2 is mid-render (reading snapshots)
static volatile bool mtActive = false;         // transition clock is running
static volatile unsigned long mtStartMs = 0;
static volatile uint32_t mtSeed = 0x9E3779B9;

// If Core 0 sets the drawing flag and never arms (interrupted path), Core 2
// resumes showing after this long so the display can't wedge dark. Generous:
// a repaint bracket can contain an OLED mirror, and a flaky/disconnected OLED
// can stall I2C for a while — better to freeze the LEDs briefly than to show
// the half-painted buffer.
static const unsigned long MT_DRAW_TIMEOUT_MS = 300;

// Keep painting/showing the landed target frame for this long after the
// transition window closes. leds.show() DROPS frames while the previous DMA
// transfer is still streaming (a full strip takes ~9ms), so the single show
// after the final blend frame can silently vanish — leaving the last
// mid-transition frame on the physical LEDs even though the buffer holds the
// target. The linger guarantees several more show attempts; at least one
// lands after the DMA frees up.
static const unsigned long MT_LINGER_MS = 50;

// Transition frame pacing. Core 2's re-trigger used to re-render and re-show
// the blend as fast as its loop spins (sub-ms): every show samples the live
// buffer (each one a fresh tear window) and back-to-back shows crowd the
// WS2812 latch-low time. 60 FPS is smooth for a ~90ms blend while staying
// comfortably above the strip's ~9.6ms physical frame period (300 LEDs @
// 800kHz + latch) and far below the tight-loop rate.
static const unsigned long MT_FRAME_MS = 16;
static volatile unsigned long mtLastFrameMs = 0;

const char* menuTransitionTypeName( uint8_t type ) {
    switch ( type ) {
    case MENU_TRANSITION_OFF: return "Off";
    case MENU_TRANSITION_DITHER: return "Dither";
    case MENU_TRANSITION_FADE: return "Fade";
    case MENU_TRANSITION_SPARKLE: return "Sparkle";
    case MENU_TRANSITION_WIPE: return "Wipe";
    case MENU_TRANSITION_TINT: return "Tint";
    case MENU_TRANSITION_COLOR_DITHER: return "ClrDithr";
    case MENU_TRANSITION_COLOR_WIPE: return "ClrWipe";
    case MENU_TRANSITION_GLOW: return "Glow";
    case MENU_TRANSITION_RIPPLE: return "Ripple";
    default: return "?";
    }
}

// Brighten a packed color by num/den, clamped per channel.
static inline uint32_t mtBrighten( uint32_t c, int num, int den ) {
    int r = ( (int)( ( c >> 16 ) & 0xff ) * num ) / den;
    int g = ( (int)( ( c >> 8 ) & 0xff ) * num ) / den;
    int b = ( (int)( c & 0xff ) * num ) / den;
    if ( r > 255 ) r = 255;
    if ( g > 255 ) g = 255;
    if ( b > 255 ) b = 255;
    return ( (uint32_t)r << 16 ) | ( (uint32_t)g << 8 ) | (uint32_t)b;
}

// Effective blend window for the current type (OFF cuts instantly but still
// lingers so the landed frame is guaranteed to reach the physical LEDs).
static inline unsigned long mtEffDurationMs( void ) {
    if ( menuTransitionConfig.type == MENU_TRANSITION_OFF ) {
        return 0;
    }
    return menuTransitionConfig.durationMs;
}

// Small stateless hash — gives each pixel a stable pseudorandom threshold per
// transition (seeded at arm time) so dither pixels flip once and stay flipped.
static inline uint32_t mtHash( uint32_t x ) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// xorshift32 stream for the per-frame sparkle speckles.
static inline uint32_t mtRand( uint32_t& s ) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Per-channel lerp, t in 0..256 (256 = full target).
static inline uint32_t mtLerpColor( uint32_t a, uint32_t b, int t ) {
    int ar = ( a >> 16 ) & 0xff, ag = ( a >> 8 ) & 0xff, ab = a & 0xff;
    int br = ( b >> 16 ) & 0xff, bg = ( b >> 8 ) & 0xff, bb = b & 0xff;
    int r = ar + ( ( ( br - ar ) * t ) >> 8 );
    int g = ag + ( ( ( bg - ag ) * t ) >> 8 );
    int bl = ab + ( ( ( bb - ab ) * t ) >> 8 );
    return ( (uint32_t)r << 16 ) | ( (uint32_t)g << 8 ) | (uint32_t)bl;
}

// Progress through the current transition, 0..256. >=256 means done.
static int mtProgress256( void ) {
    unsigned long dur = mtEffDurationMs( );
    if ( dur == 0 ) {
        return 256;
    }
    unsigned long elapsed = millis( ) - mtStartMs;
    if ( elapsed >= dur ) {
        return 256;
    }
    return (int)( ( elapsed * 256UL ) / dur );
}

void menuTransitionSetAccent( uint32_t color ) {
    mtAccentColor = color;
}

// Rebuild accentFrame from the (just-snapshotted) targetFrame: each lit pixel
// keeps its own brightness but takes the accent hue, so the accent flash reads
// at the same intensity as the menu text regardless of brightness settings.
// With no accent set the frame desaturates to white.
static void mtBuildAccentFrame( void ) {
    uint32_t accent = mtAccentColor;
    hsvColor accentHsv = RgbToHsv( accent ? accent : (uint32_t)0xFFFFFF );
    for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
        uint32_t t = targetFrame[ i ];
        if ( t == 0 ) {
            accentFrame[ i ] = 0;
            continue;
        }
        hsvColor hv = RgbToHsv( t );
        hv.h = accentHsv.h;
        hv.s = accent ? 240 : 0;
        accentFrame[ i ] = HsvToRaw( hv );
    }
}

void menuTransitionBeginDraw( void ) {
    mtDrawing = true;
    mtDrawStartMs = millis( );
    __dmb( );
    // Wait out an in-flight Core 2 render so it can't paint stale blend pixels
    // over the repaint we're about to do. Renders take well under a millisecond;
    // the timeout is just a belt-and-braces bound.
    unsigned long waitStart = micros( );
    while ( mtRenderBusy && ( micros( ) - waitStart ) < 3000 ) {
        tight_loop_contents( );
    }

    // If a transition is still in flight, the live buffer holds a TRANSIENT
    // blend/flare frame right now. The painter about to run may only repaint
    // part of the breadboard (selectSubmenuOption's option rows do
    // b.clear(1) — bottom half only), and arm() snapshots the WHOLE buffer
    // as the next target — so any region the painter skips would bake the
    // transient pixels into the new target frame. Mid-glow this compounds:
    // every arm() captures the previous flare (~1.7x bright) and the next
    // glow flares on top of it — scroll an option list quickly and the
    // pinned header climbs to blinding white. Restore the landed target
    // first so un-repainted regions snapshot at their true steady colors.
    if ( mtActive ) {
        for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
            leds.setPixelColor( (uint16_t)i, targetFrame[ i ] );
        }
    }
}

void menuTransitionArm( void ) {
    // Fold whatever was on screen into prevFrame so back-to-back nav events
    // chain smoothly: mid-transition we capture the current blend (linear
    // approximation — close enough for dither/sparkle), otherwise the old
    // target IS the displayed frame.
    if ( mtActive ) {
        int t = mtProgress256( );
        if ( t >= 256 ) {
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                prevFrame[ i ] = targetFrame[ i ];
            }
        } else {
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                prevFrame[ i ] = mtLerpColor( prevFrame[ i ], targetFrame[ i ], t );
            }
        }
    } else {
        for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
            prevFrame[ i ] = targetFrame[ i ];
        }
    }

    // Snapshot the frame Core 0 just drew.
    for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
        targetFrame[ i ] = leds.getPixelColor( (uint16_t)i );
    }
    mtBuildAccentFrame( );

    mtSeed = mtHash( mtSeed ^ micros( ) );
    mtStartMs = millis( );
    // Always activate — even OFF runs the linger window so the landed frame
    // can't be lost to a DMA-busy dropped show.
    mtActive = true;
    __dmb( ); // snapshots + clock visible before Core 2 resumes showing
    mtDrawing = false;
}

void menuTransitionReplay( void ) {
    mtSeed = mtHash( mtSeed ^ micros( ) );
    mtStartMs = millis( );
    mtActive = true;
    __dmb( );
}

void menuTransitionCancel( void ) {
    mtActive = false;
    mtDrawing = false;
    __dmb( );
    // Let an in-flight Core 2 render drain so its last blend frame can't land
    // after the caller starts painting.
    unsigned long waitStart = micros( );
    while ( mtRenderBusy && ( micros( ) - waitStart ) < 3000 ) {
        tight_loop_contents( );
    }
}

bool menuTransitionActive( void ) {
    if ( mtDrawing ) {
        return true;
    }
    return mtActive;
}

bool menuTransitionFrameDue( void ) {
    if ( mtDrawing ) {
        // Keep Core 2 scheduling menu frames so the post-arm frame lands
        // promptly (render() skips the show itself while the bracket is open).
        return true;
    }
    if ( !mtActive ) {
        return false;
    }
    return ( millis( ) - mtLastFrameMs ) >= MT_FRAME_MS;
}

bool menuTransitionCanShow( void ) {
    if ( mtDrawing && ( millis( ) - mtDrawStartMs ) < MT_DRAW_TIMEOUT_MS ) {
        return false; // Core 0 is mid-repaint — don't stream the buffer
    }
    return true;
}

bool menuTransitionRender( void ) {
    // Core 0 is mid-repaint: skip this frame's show so a half-drawn buffer
    // never reaches the LEDs. Timeout guards against an interrupted repaint.
    if ( mtDrawing ) {
        if ( millis( ) - mtDrawStartMs < MT_DRAW_TIMEOUT_MS ) {
            return false;
        }
        mtDrawing = false; // stuck flag — resume showing
    }

    if ( !mtActive ) {
        return true; // steady state: live buffer already holds the frame
    }

    mtRenderBusy = true;
    __dmb( );
    // Re-check: beginDraw() may have raced us between the check above and the
    // busy flag going up. Bail without touching the buffer.
    if ( mtDrawing ) {
        mtRenderBusy = false;
        return false;
    }

    int t = mtProgress256( );
    uint8_t type = menuTransitionConfig.type;
    uint32_t seed = mtSeed;

    if ( t >= 256 ) {
        // Landed: paint the target verbatim. Stay active through the linger
        // window so this frame keeps getting show attempts — leds.show()
        // drops frames while the strip DMA is busy, and without the linger
        // the final frame's one show can vanish, stranding the last blend
        // frame on the physical LEDs.
        for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
            leds.setPixelColor( (uint16_t)i, targetFrame[ i ] );
        }
        if ( millis( ) - mtStartMs >= mtEffDurationMs( ) + MT_LINGER_MS ) {
            mtActive = false;
        }
    } else {
        switch ( type ) {
        case MENU_TRANSITION_FADE:
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                leds.setPixelColor( (uint16_t)i, mtLerpColor( prevFrame[ i ], targetFrame[ i ], t ) );
            }
            break;

        case MENU_TRANSITION_DITHER: {
            // Each pixel gets a stable random threshold; it flips from prev to
            // target the moment progress crosses it — random scatter that
            // resolves into the new frame.
            uint32_t thr = (uint32_t)t << 8; // 0..65536
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                uint32_t h = mtHash( seed + (uint32_t)i ) & 0xFFFF;
                leds.setPixelColor( (uint16_t)i, ( h < thr ) ? targetFrame[ i ] : prevFrame[ i ] );
            }
            break;
        }

        case MENU_TRANSITION_SPARKLE: {
            // Target frame immediately, plus a random speckle overlay whose
            // count decays to zero across the window ("random dithering that
            // quickly fades"). Speckles re-randomize every frame.
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                leds.setPixelColor( (uint16_t)i, targetFrame[ i ] );
            }
            int maxSpeckles = ( menuTransitionConfig.density * 48 ) / 255; // 0..48 pixels
            int speckles = ( maxSpeckles * ( 256 - t ) ) >> 8;
            uint32_t frameSeed = mtHash( seed ^ millis( ) );
            uint32_t tint = menuTransitionConfig.tintColor;
            for ( int s = 0; s < speckles; s++ ) {
                uint32_t r = mtRand( frameSeed );
                uint16_t px = (uint16_t)( r % MENU_TRANSITION_PIXELS );
                uint32_t c = tint;
                if ( c == 0 ) {
                    hsvColor hsv = { (uint8_t)( r >> 16 ), 220, 60 };
                    c = HsvToRaw( hsv );
                }
                // Fade the speckle brightness out with progress too.
                leds.setPixelColor( px, mtLerpColor( c, targetFrame[ px ], t ) );
            }
            break;
        }

        case MENU_TRANSITION_WIPE: {
            int rowEdge = ( 60 * t ) >> 8; // rows fully on target
            for ( int row = 0; row < 60; row++ ) {
                const uint32_t* src = ( row <= rowEdge ) ? targetFrame : prevFrame;
                for ( int j = 0; j < 5; j++ ) {
                    int i = row * 5 + j;
                    leds.setPixelColor( (uint16_t)i, src[ i ] );
                }
            }
            break;
        }

        case MENU_TRANSITION_TINT: {
            // The new frame pops in immediately, but recolored to the item's
            // accent hue; the first quarter crossfades prev -> accent so the
            // cut isn't harsh, the rest settles accent -> true colors. Each
            // item announces itself in its own color.
            if ( t < 64 ) {
                int u = t << 2; // 0..256 across the first quarter
                for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                    leds.setPixelColor( (uint16_t)i, mtLerpColor( prevFrame[ i ], accentFrame[ i ], u ) );
                }
            } else {
                int u = ( ( t - 64 ) * 256 ) / 192; // 0..256 across the rest
                for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                    leds.setPixelColor( (uint16_t)i, mtLerpColor( accentFrame[ i ], targetFrame[ i ], u ) );
                }
            }
            break;
        }

        case MENU_TRANSITION_COLOR_DITHER: {
            // Like DITHER, but each pixel arrives in the accent color the
            // moment it flips, then settles to its true color over the rest
            // of its window — a colored shimmer that resolves into the item.
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                int h = (int)( mtHash( seed + (uint32_t)i ) & 0xFF ); // flip point, 0..255
                uint32_t px;
                if ( t <= h ) {
                    px = prevFrame[ i ];
                } else {
                    int u = ( ( t - h ) * 256 ) / ( 256 - h ); // 0..256 since flip
                    px = mtLerpColor( accentFrame[ i ], targetFrame[ i ], u );
                }
                leds.setPixelColor( (uint16_t)i, px );
            }
            break;
        }

        case MENU_TRANSITION_COLOR_WIPE: {
            // Row sweep with an accent-colored leading band trailing the edge.
            int rowEdge = ( 60 * t ) >> 8;
            const int band = 5;
            for ( int row = 0; row < 60; row++ ) {
                const uint32_t* src;
                if ( row > rowEdge ) {
                    src = prevFrame;
                } else if ( row > rowEdge - band ) {
                    src = accentFrame;
                } else {
                    src = targetFrame;
                }
                for ( int j = 0; j < 5; j++ ) {
                    int i = row * 5 + j;
                    leds.setPixelColor( (uint16_t)i, src[ i ] );
                }
            }
            break;
        }

        case MENU_TRANSITION_GLOW: {
            // New frame lands on the first frame; its lit pixels start flared
            // toward a brightened accent and ease back to true colors — a
            // quick colorful pulse that announces the item without ever
            // mixing in the previous frame.
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                uint32_t tc = targetFrame[ i ];
                if ( tc == 0 ) {
                    leds.setPixelColor( (uint16_t)i, 0 );
                    continue;
                }
                uint32_t flare = mtBrighten( accentFrame[ i ], 8, 3 );
                leds.setPixelColor( (uint16_t)i, mtLerpColor( flare, tc, t ) );
            }
            break;
        }

        case MENU_TRANSITION_RIPPLE: {
            // The frame is fully landed everywhere; a narrow accent shimmer
            // band sweeps top-to-bottom across it once. Rows it hasn't
            // reached (and rows it has passed) show the true frame.
            int waveRow = ( 68 * t ) >> 8; // sweeps past row 59 so the band exits cleanly
            for ( int row = 0; row < 60; row++ ) {
                int d = row - waveRow;
                if ( d < 0 ) d = -d;
                int boost = ( d >= 4 ) ? 0 : ( 256 - d * 64 ); // 0..256 within the band
                for ( int j = 0; j < 5; j++ ) {
                    int i = row * 5 + j;
                    uint32_t tc = targetFrame[ i ];
                    if ( tc == 0 || boost == 0 ) {
                        leds.setPixelColor( (uint16_t)i, tc );
                    } else {
                        uint32_t shimmer = mtBrighten( accentFrame[ i ], 3, 2 );
                        leds.setPixelColor( (uint16_t)i, mtLerpColor( tc, shimmer, boost ) );
                    }
                }
            }
            break;
        }

        default: // OFF — land on target; linger handled in the t>=256 path
            for ( int i = 0; i < MENU_TRANSITION_PIXELS; i++ ) {
                leds.setPixelColor( (uint16_t)i, targetFrame[ i ] );
            }
            break;
        }
    }

    mtLastFrameMs = millis( ); // pace menuTransitionFrameDue() from real renders
    __dmb( );
    mtRenderBusy = false;
    return true;
}
