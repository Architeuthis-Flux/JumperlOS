// Menu frame transitions — masks the single-frame flash that can still slip
// through the encoder click/turn interlock (a step that commits, draws, and is
// then discarded by a late press) by cross-blending between menu frames instead
// of cutting hard. Also makes menu redraws atomic: Core 2 never shows the LED
// buffer while Core 0 is mid-repaint (b.clear()/b.print() are not atomic).
//
// Flow:
//   Core 0 (renderMenuLine):  menuTransitionBeginDraw() -> b.clear/b.print ->
//                             menuTransitionArm()  (snapshot live buffer)
//   Core 2 (core2stuff menu branch): menuTransitionRender() — paints the
//                             prev->target blend into the live buffer each
//                             frame; returns false while Core 0 is drawing so
//                             that frame's leds.show() is skipped.
//
// Only the 300 breadboard pixels (rows 0–59 x 5) are managed; rails / logo /
// header LEDs are owned by their existing paths.

#ifndef MENUTRANSITIONS_H
#define MENUTRANSITIONS_H

#include <Arduino.h>

enum MenuTransitionType : uint8_t {
    MENU_TRANSITION_OFF = 0, // hard cut (still gets the atomic-redraw fix)
    MENU_TRANSITION_DITHER,  // per-pixel random order flip from prev to target
    MENU_TRANSITION_FADE,    // per-channel linear crossfade
    MENU_TRANSITION_SPARKLE, // target frame + decaying random speckle overlay
    MENU_TRANSITION_WIPE,    // top-to-bottom row sweep (cheap comparison point)
    // Accent types: tinted with the selected menu item's ring color (set per
    // nav event by updateMenuLogoRing via menuTransitionSetAccent), so each
    // item announces itself in its own hue before settling to normal colors.
    MENU_TRANSITION_TINT,         // new frame pops in accent-colored, settles to true colors
    MENU_TRANSITION_COLOR_DITHER, // pixels dither in via the accent color, then settle
    MENU_TRANSITION_COLOR_WIPE,   // row sweep with an accent-colored leading band
    // Subtle overlay types: the new frame lands instantly and only a brief
    // brightness/hue flourish plays on top — no prev/target pixel mixing, so
    // an interrupted transition can never leave holes in the text.
    MENU_TRANSITION_GLOW,   // lit pixels flare accent-bright, settle to true colors
    MENU_TRANSITION_RIPPLE, // accent shimmer band sweeps down the landed frame
    MENU_TRANSITION_TYPE_COUNT,
};

// Live-tunable from the Menu FX debug TUI (Debugs.cpp). Volatile because the
// tuner mutates it on Core 0 while Core 2 renders with it.
struct MenuTransitionConfig {
    volatile uint8_t type = MENU_TRANSITION_GLOW;
    volatile uint16_t durationMs = 160;
    volatile uint32_t tintColor = 0x000000; // sparkle speckle color; 0 = random hues
    volatile uint8_t density = 128;         // sparkle speckle amount (0-255)
};
extern MenuTransitionConfig menuTransitionConfig;

// Human-readable name for a MenuTransitionType (for the tuner TUI / printouts).
const char* menuTransitionTypeName(uint8_t type);

// Set the accent color for the accent transition types (TINT / COLOR_DITHER /
// COLOR_WIPE). Call before menuTransitionArm(); the menu layer passes the
// selected item's logo-ring hue. 0 = no accent (falls back to a white flash).
void menuTransitionSetAccent(uint32_t color);

// Core 0: call immediately before repainting the menu LED buffer. Core 2 skips
// showing (and writing) the breadboard region until menuTransitionArm() lands.
void menuTransitionBeginDraw(void);

// Core 0: call once the repaint is complete. Snapshots the live buffer as the
// new target frame, folds the previous frame in as the blend source, and starts
// the transition clock.
void menuTransitionArm(void);

// Restart the last armed transition from the top (same prev/target frames, new
// random seed). Used by the tuner's replay key.
void menuTransitionReplay(void);

// Abort any in-flight transition WITHOUT painting. Call when a painter that
// doesn't use the beginDraw/arm bracket takes over the breadboard (value
// editors, node pickers, menu actions) — otherwise the active blend keeps
// stamping the old frame over their fresh paint, leaving stale/off pixels.
void menuTransitionCancel(void);

// True while a transition is in flight (or a Core 0 repaint is pending), i.e.
// Core 2 should keep scheduling menu frames.
bool menuTransitionActive(void);

// Frame-rate-paced version of the above for Core 2's transition re-trigger:
// true only when the next transition frame is due (MT_FRAME_MS since the last
// rendered frame). Without this pacing the re-trigger re-rendered and re-showed
// every Core 2 loop iteration — far faster than the WS2812 strip can latch
// frames, and each show is a fresh chance to tear against a Core 0 repaint.
bool menuTransitionFrameDue(void);

// Core 2: paint the current blend into the live LED buffer. Returns true if
// this frame should be shown, false if leds.show() should be skipped (Core 0 is
// mid-repaint). When no transition is active this is a no-op returning true.
bool menuTransitionRender(void);

// Last-moment show gate for Core 2. menuTransitionRender()'s verdict can go
// stale: Core 0 may START a repaint in the gap between the render and the
// actual leds.show() call, and showing then streams a half-painted buffer
// (misaligned/garbled text for one frame). Re-check this immediately before
// showing a menu frame.
bool menuTransitionCanShow(void);

#endif // MENUTRANSITIONS_H
