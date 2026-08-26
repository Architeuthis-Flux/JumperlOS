#ifndef CONFIG_H
#define CONFIG_H

//Jumperless config
#include "JumperlessDefines.h"

// ---------------------------------------------------------------------------
// Compile-time behaviour flags (not user-settable; -D on the build line)
// ---------------------------------------------------------------------------
//
// JL_PROJECT_RUN_HISTORY - how many run files a project keeps.
//
//   0 (DEFAULT)  ONE run file per project, /projects/<dir>/<dir>_run.yaml,
//                silently reused. A prompt appears only when a guided build
//                is MID-FLIGHT in it (resume / start fresh, which overwrites
//                from the template). Kevin's ruling: the numbered files
//                "make way too many files", and keeping a run is what
//                `slots` > `save to` is for.
//
//   1            The wave-2 numbered scheme: /projects/<dir>/<dir>_<N>.yaml,
//                N = max+1 per launch, a load-latest / start-new prompt,
//                `z ... run=<N>`, and the >=20-file pile-up hint. Kept
//                COMPILING - not deleted - so the history behaviour can come
//                back on a build flag.
//
// Both modes share the same create/validate/recover, resume, runSource and
// terminal-state machinery; only the NAME and the prompt policy differ.
// ProjectsApp.h carries the naming/namespace invariants for both.
#ifndef JL_PROJECT_RUN_HISTORY
#define JL_PROJECT_RUN_HISTORY 0
#endif

// ===========================================================================
// THE SINGLE-SITE CONFIG OPTION LIST
// ===========================================================================
//
// Adding a config option = adding ONE X() line to the right JL_CFG_<section>
// list below. That one line generates:
//   - the struct field (with its default) in `struct config`
//   - the parse / save / print / diff / help / TUI descriptor entry
//     (the second expansion lives in configManager.cpp)
//
// X(section, key, TYPE, default, min, max, step, enumTable, applyHook, flags,
//   "one-sentence description")
//
//   TYPE      BOOL / INT / VINT (volatile int) / FLOAT / HEX / FONT /
//             STR16 / STR33
//   min/max   numeric bounds for the TUI editor; 0,0 = unconstrained
//   step      TUI spinner increment (0 = 1 for ints / 0.01 for floats)
//   enumTable a StringIntEntry[] from configManager.h (names for values),
//             or nullptr. BOOLs implicitly use boolTable.
//   applyHook HOOK_* id (configManager.h) run on live changes, or HOOK_NONE
//   flags     JLC_* bits (configManager.h): JLC_CAL survives resets and
//             migration, JLC_BOOT_ONLY takes effect next boot, JLC_LEDS
//             refreshes the LEDs after a change, JLC_HIDDEN keeps it out of
//             the ~ printout and the TUI, JLC_SHOW_* cross-lists a debug
//             flag into another TUI category.
//
// Renamed/moved keys keep working through jlConfigAliases in configManager.cpp
// (old `[section] key` names remap on parse). Removed keys parse as no-ops.
// ===========================================================================

#if defined(OG_JUMPERLESS)
// OG has 1 LED per breadboard row; "wires" mode paints the 5-LEDs-per-row
// fill, which garbles the single-pixel strip. Default to lines so a freshly
// flashed OG renders correctly (the lines_wires parser also forces this).
#define JL_DEFAULT_LINES_WIRES 0
#else
#define JL_DEFAULT_LINES_WIRES 1
#endif

// --- [firmware] ------------------------------------------------------------
// Update tracking only - hidden from ~ and the TUI, written on every save.
#define JL_CFG_FIRMWARE(X) \
  X(firmware, last_version, STR16, "", 0, 0, 0, nullptr, HOOK_NONE, JLC_HIDDEN, \
    "Last firmware version that ran on this board (drives update detection).")

// --- [hardware] ------------------------------------------------------------
// Board identity + memory layout. Preserved across `reset` like calibration.
#define JL_CFG_HARDWARE(X) \
  X(hardware, generation, INT, 5, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Jumperless hardware generation (5 = Jumperless V5).") \
  X(hardware, revision, INT, 5, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Board revision, set at the factory and mirrored in EEPROM.") \
  X(hardware, probe_revision, INT, 5, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Probe hardware revision.") \
  X(hardware, psram_installed, BOOL, false, 0, 0, 0, nullptr, HOOK_PSRAM, JLC_NONE, \
    "Whether the 8MB PSRAM chip is installed (enables the app arena and a bigger MicroPython heap).") \
  X(hardware, psram_app_size_kb, INT, 2048, 0, 8192, 256, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "KiB of PSRAM reserved for the app arena (file cache, undo log, scratch); the rest goes to MicroPython.")

// --- [probe] ---------------------------------------------------------------
// Everything about the probe: behavior knobs plus its calibration (flagged
// JLC_CAL so it survives resets and firmware-update migration).
//
// pad_max/pad_min: SELECT-feed pad decode endpoints. pad_max_measure /
// pad_max_measure_gpio: MEASURE-position endpoints per power feed - DAC0 is a
// stiff ~2-crosspoint feed, a routable GPIO is ~170-185 ohm through 4
// crosspoints and droops under the pad ladder. The ratiometric decode
// (endpoints x live ADC7 / 3.3V) cancels the drive VOLTAGE but not the source
// impedance, so each feed carries its own max. pad_min_measure is shared.
//
// droop_v0 / droop_ohms: GPIO-powered measure buffer droop-current model
// I = (V0 - ADC7) / R. droop_ohms == 0 means the droop calibration never ran
// (the model falls back to an empirical 30-ohm constant) - this is also the
// migration sentinel that triggers the switch-calibration prompt.
#define JL_CFG_PROBE(X) \
  X(probe, auto_connect, INT, 1, -1, 1, 1, autoConnectTable, HOOK_PROBE_AUTOCONNECT, JLC_NONE, \
    "Power the probe's measure buffer automatically: on, off until reboot, or off persistently.") \
  X(probe, power_source, INT, 0, 0, 1, 1, probePowerSourceTable, HOOK_PROBE_POWER_SOURCE, JLC_NONE, \
    "Which feed the probe buffer tries first: DAC0 (current-sensed, 2 crosspoints) or a routable GPIO (keeps DAC0 free).") \
  X(probe, use_pio_button, BOOL, true, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Sample the probe button with a PIO state machine (~75x faster than CPU bit-banging; auto-falls back).") \
  X(probe, led_on_button_pin, BOOL, true, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Drive the probe's WS2812 LED on the button pin (GPIO 9) so a flaky GPIO2-side cable contact stops mattering.") \
  X(probe, led_refresh_us, INT, 0, 0, 1000000, 50, nullptr, HOOK_LED_REFRESH_US, JLC_NONE, \
    "Minimum microseconds between idle probe LED re-sends; 0 = legacy constant re-send (scope experiment knob).") \
  X(probe, pad_max, INT, 4055, 0, 4095, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "Highest raw probe ADC reading in SELECT position (top pad decode endpoint).") \
  X(probe, pad_min, INT, 10, 0, 4095, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "Lowest raw probe ADC reading in SELECT position (bottom pad decode endpoint).") \
  X(probe, pad_max_measure, INT, 4055, 0, 4200, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "MEASURE-position top pad endpoint with the DAC0 power feed.") \
  X(probe, pad_max_measure_gpio, INT, 4055, 0, 4200, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "MEASURE-position top pad endpoint with a routable-GPIO power feed (droops more than DAC0).") \
  X(probe, pad_min_measure, INT, 10, 0, 4095, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "MEASURE-position bottom pad endpoint (shared by both power feeds).") \
  X(probe, switch_threshold_high, FLOAT, 1.2f, 0.0f, 10.0f, 0.05f, nullptr, HOOK_NONE, JLC_CAL, \
    "Buffer current (mA) above which the switch reads as SELECT (hysteresis top).") \
  X(probe, switch_threshold_low, FLOAT, 0.90f, 0.0f, 10.0f, 0.05f, nullptr, HOOK_NONE, JLC_CAL, \
    "Buffer current (mA) below which the switch reads as MEASURE (hysteresis bottom).") \
  X(probe, switch_select_max_ma, FLOAT, 0.0f, 0.0f, 50.0f, 0.25f, nullptr, HOOK_NONE, JLC_CAL, \
    "Upper bound of the SELECT current signature under a DAC0 feed; above it the buffer is loaded, not in SELECT. 0 = disabled.") \
  X(probe, switch_blink_hold_pct, INT, 50, 1, 99, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "GPIO-feed switch detector: ADC7 held percentage during the feed blink at or above this classifies SELECT.") \
  X(probe, measure_voltage, FLOAT, 3.30f, 3.0f, 3.6f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "MEASURE-mode tip drive voltage; servoed by the self test so the tip sits at 3.30V - nothing else should move it.") \
  X(probe, current_zero, FLOAT, 2.0f, 0.0f, 10.0f, 0.05f, nullptr, HOOK_NONE, JLC_CAL, \
    "INA current reading (mA) with the probe unloaded, subtracted from switch-position measurements.") \
  X(probe, min_valid_reading, INT, 85, 0, 4095, 5, nullptr, HOOK_NONE, JLC_CAL, \
    "Raw probe ADC readings below this count as no touch (pad decode gate).") \
  X(probe, droop_v0, FLOAT, 3.35f, 3.0f, 3.6f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "GPIO-powered buffer: unloaded ADC7 tip voltage that maps to zero droop current.") \
  X(probe, droop_ohms, FLOAT, 0.0f, 0.0f, 500.0f, 0.5f, nullptr, HOOK_NONE, JLC_CAL, \
    "GPIO feed source resistance for the droop current model; 0 = never calibrated (empirical 30-ohm fallback).") \
  X(probe, pad_ohms, FLOAT, 5.0f, 0.0f, 100.0f, 0.5f, nullptr, HOOK_NONE, JLC_CAL, \
    "The GPIO pad's own share of the droop resistance at the 12mA drive the feed claims.")

// --- [clickwheel] ----------------------------------------------------------
// Rotary encoder behavior + the menu frame-transition eye candy (live-tunable
// from the Menu FX tuner, persisted here).
#define JL_CFG_CLICKWHEEL(X) \
  X(clickwheel, encoder_pio, INT, -1, -1, 2, 1, encoderPioTable, HOOK_ENCODER_PIO, JLC_BOOT_ONLY, \
    "Which PIO block the encoder's quadrature sampler tries first; auto = PIO2 then PIO1, keeping PIO0 for user programs.") \
  X(clickwheel, rail_click_adjust, INT, 1, 0, 2, 1, railClickAdjustTable, HOOK_NONE, JLC_NONE, \
    "Click the wheel on a highlighted rail/DAC to adjust its voltage: off, only with an OLED, or always.") \
  X(clickwheel, fx_type, INT, 8, 0, 9, 1, menuFxTypeTable, HOOK_MENU_FX, JLC_NONE, \
    "Menu frame transition style on the breadboard LEDs (glow, sparkle, dither, wipe...).") \
  X(clickwheel, fx_duration_ms, INT, 160, 0, 1000, 15, nullptr, HOOK_MENU_FX, JLC_NONE, \
    "How long a menu frame transition runs, in milliseconds.") \
  X(clickwheel, fx_tint, HEX, 0x000000, 0, 0xFFFFFF, 0, nullptr, HOOK_MENU_FX, JLC_NONE, \
    "Sparkle speckle color as 0xRRGGBB; 0 = random hues.") \
  X(clickwheel, fx_density, INT, 128, 0, 255, 16, nullptr, HOOK_MENU_FX, JLC_NONE, \
    "Sparkle speckle amount (0-255).")

// --- [measurement] ---------------------------------------------------------
// The net voltage/current scan and how its results are shown.
#define JL_CFG_MEASUREMENT(X) \
  X(measurement, net_currents, INT, 1, 0, 1, 1, nullptr, HOOK_NONE, JLC_LEDS, \
    "Scan node voltages in the background and show per-connection current as marching ants on the LEDs (V5 only).") \
  X(measurement, current_flow, INT, 0, 0, 1, 1, currentFlowTable, HOOK_NONE, JLC_LEDS, \
    "Direction the current ants march: conventional (+ to -) or electron (- to +); reported values stay conventional.") \
  X(measurement, show_probe_current, INT, 0, 0, 2, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Print the probe's measured current while probing (0 = off).") \
  X(measurement, crosspoint_resistance, FLOAT, 40.0f, 0.0f, 200.0f, 0.5f, nullptr, HOOK_NONE, JLC_CAL, \
    "On-resistance of one CH446Q crosspoint (ohms), used to turn node voltage deltas into per-connection currents.")

// --- [terminal] ------------------------------------------------------------
// How the serial terminal behaves.
#define JL_CFG_TERMINAL(X) \
  X(terminal, colors, BOOL, true, 0, 0, 0, nullptr, HOOK_TERM_COLORS, JLC_NONE, \
    "ANSI colors in terminal output; turn off for dumb terminals or logs.") \
  X(terminal, line_buffering, BOOL, true, 0, 0, 0, nullptr, HOOK_LINE_BUFFERING, JLC_NONE, \
    "Buffer typed serial input into lines with editing/history instead of acting on every keystroke.")

// --- [undo] ----------------------------------------------------------------
// The connection-history undo log (clickwheel scrub / u command).
#define JL_CFG_UNDO(X) \
  X(undo, persist, BOOL, true, 0, 0, 0, nullptr, HOOK_UNDO, JLC_NONE, \
    "Save undo history to /undo_history.txt so it survives a power cycle.") \
  X(undo, max_saved_actions, INT, 256, 1, 256, 16, nullptr, HOOK_UNDO, JLC_NONE, \
    "How many undo transactions the persisted history keeps (RAM ring is unaffected).")

// --- [dacs] ----------------------------------------------------------------
// DAC output limits. Voltage values live in activeState.power, not here.
#define JL_CFG_DACS(X) \
  X(dacs, limit_max, FLOAT, 8.00f, 0.0f, 10.0f, 0.25f, nullptr, HOOK_NONE, JLC_NONE, \
    "Highest voltage the DACs and rails can be set to.") \
  X(dacs, limit_min, FLOAT, -8.00f, -10.0f, 0.0f, 0.25f, nullptr, HOOK_NONE, JLC_NONE, \
    "Lowest voltage the DACs and rails can be set to.")

// --- [debug] ---------------------------------------------------------------
// Debug print gates. Single source of truth for the flags that other TUI
// categories cross-list (JLC_SHOW_*).
#define JL_CFG_DEBUG(X) \
  X(debug, file_parsing, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG, \
    "Print node-file parsing steps.") \
  X(debug, net_manager, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG, \
    "Print net manager decisions (which nodes join which nets).") \
  X(debug, nets_to_chips, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG, \
    "Print net-to-crossbar-chip routing results.") \
  X(debug, nets_to_chips_alt, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG, \
    "Print the alternate (verbose path-search) view of chip routing.") \
  X(debug, probing, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG | JLC_SHOW_PROBE, \
    "Print probe pad decoding and touch events.") \
  X(debug, arduino, INT, 0, 0, 3, 1, nullptr, HOOK_NONE, JLC_DEBUG, \
    "Arduino/UART passthrough debug verbosity (0 = off).") \
  X(debug, show_node_errors, BOOL, true, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG, \
    "Warn when a node file references a node that doesn't exist.") \
  X(debug, probe_switch_stats, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG | JLC_SHOW_PROBE, \
    "Print one line per switch-position evaluation: sensing source, current, thresholds, verdict.") \
  X(debug, probe_switch_agree, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG | JLC_SHOW_PROBE, \
    "Use the experimental agreement classifier for the probe switch (tip sense must agree with the feed detector).") \
  X(debug, net_voltage_scan, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_DEBUG | JLC_SHOW_MEASURE, \
    "Print net voltage scan stats once a second: node voltages and per-path currents.") \
  X(debug, net_scan_pair_taps, INT, 1, 0, 1, 1, nullptr, HOOK_NONE, JLC_DEBUG | JLC_SHOW_MEASURE, \
    "Tap both ends of a routed path at once on two ADCs (1, default) or sequentially (0).")

// --- [routing] -------------------------------------------------------------
#define JL_CFG_ROUTING(X) \
  X(routing, stack_paths, INT, 2, 0, 7, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "How many parallel crosspoint paths to stack per connection (lowers resistance).") \
  X(routing, stack_rails, INT, 3, 0, 7, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "How many parallel paths to stack for rail connections.") \
  X(routing, stack_dacs, INT, 0, 0, 7, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "How many extra parallel paths to stack for DAC connections.")

// --- [slots] ---------------------------------------------------------------
// Which context the board comes up in. Deliberately NOT where the active
// slot itself is remembered - that lives in /slots/last_active.txt, because
// config saves are full-file rewrites behind a diff gate and writing config
// on every slot switch would churn flash and the diff cache.
#define JL_CFG_SLOTS(X) \
  X(slots, boot_mode, INT, 1, 0, 1, 1, bootModeTable, HOOK_NONE, JLC_NONE, \
    "Boot into the last-active slot (default) or always into boot_slot.") \
  X(slots, boot_slot, INT, 0, 0, 7, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Slot number used when boot_mode is fixed_slot.")

// --- [calibration] ---------------------------------------------------------
// DAC / rail / ADC transfer curves, written by the DAC calibration ($ / the
// Calib DACs app). Probe calibration lives in [probe]. All JLC_CAL.
#define JL_CFG_CALIBRATION(X) \
  X(calibration, top_rail_zero, INT, 1650, 0, 4095, 1, nullptr, HOOK_NONE, JLC_CAL, \
    "Top rail DAC code that outputs 0V.") \
  X(calibration, top_rail_spread, FLOAT, 21.5f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "Top rail full-scale voltage spread.") \
  X(calibration, bottom_rail_zero, INT, 1650, 0, 4095, 1, nullptr, HOOK_NONE, JLC_CAL, \
    "Bottom rail DAC code that outputs 0V.") \
  X(calibration, bottom_rail_spread, FLOAT, 21.5f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "Bottom rail full-scale voltage spread.") \
  X(calibration, dac_0_zero, INT, 1650, 0, 4095, 1, nullptr, HOOK_NONE, JLC_CAL, \
    "DAC 0 code that outputs 0V.") \
  X(calibration, dac_0_spread, FLOAT, 21.5f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "DAC 0 full-scale voltage spread.") \
  X(calibration, dac_1_zero, INT, 1650, 0, 4095, 1, nullptr, HOOK_NONE, JLC_CAL, \
    "DAC 1 code that outputs 0V.") \
  X(calibration, dac_1_spread, FLOAT, 21.5f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "DAC 1 full-scale voltage spread.") \
  X(calibration, adc_0_zero, FLOAT, 9.0f, 0.0f, 20.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 0 zero offset.") \
  X(calibration, adc_0_spread, FLOAT, 18.28f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 0 full-scale spread.") \
  X(calibration, adc_1_zero, FLOAT, 9.0f, 0.0f, 20.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 1 zero offset.") \
  X(calibration, adc_1_spread, FLOAT, 18.28f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 1 full-scale spread.") \
  X(calibration, adc_2_zero, FLOAT, 9.0f, 0.0f, 20.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 2 zero offset.") \
  X(calibration, adc_2_spread, FLOAT, 18.28f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 2 full-scale spread.") \
  X(calibration, adc_3_zero, FLOAT, 9.0f, 0.0f, 20.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 3 zero offset.") \
  X(calibration, adc_3_spread, FLOAT, 18.28f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 3 full-scale spread.") \
  X(calibration, adc_4_zero, FLOAT, 0.0f, 0.0f, 20.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 4 (5V-tolerant divider) zero offset.") \
  X(calibration, adc_4_spread, FLOAT, 5.0f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 4 full-scale spread.") \
  X(calibration, adc_7_zero, FLOAT, 9.0f, 0.0f, 20.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 7 (probe tip) zero offset.") \
  X(calibration, adc_7_spread, FLOAT, 18.28f, 0.0f, 40.0f, 0.01f, nullptr, HOOK_NONE, JLC_CAL, \
    "ADC 7 (probe tip) full-scale spread.")

// --- [logo_pads] -----------------------------------------------------------
// Each probe pad carries TWO bindings: the NODE it stands for when tapped in
// connect/clear mode (any special node except the rails - those have their
// own pads - or "choose" for the on-board picker), and the ACTION an idle
// tap runs when the probe is parked in SELECT (gpio toggle/high/low, DAC
// nudges).
#define JL_CFG_LOGO_PADS(X) \
  X(logo_pads, top_guy, INT, 0, 0, 0, 0, padNodeTable, HOOK_NONE, JLC_LEDS, \
    "Node the top logo pad stands for in connect/clear mode (or 'choose' for the on-board picker).") \
  X(logo_pads, bottom_guy, INT, 1, 0, 0, 0, padNodeTable, HOOK_NONE, JLC_LEDS, \
    "Node the bottom logo pad stands for in connect/clear mode.") \
  X(logo_pads, building_pad_top, INT, 25, 0, 0, 0, padNodeTable, HOOK_NONE, JLC_LEDS, \
    "Node the top building pad stands for in connect/clear mode (default current sense +).") \
  X(logo_pads, building_pad_bottom, INT, 26, 0, 0, 0, padNodeTable, HOOK_NONE, JLC_LEDS, \
    "Node the bottom building pad stands for in connect/clear mode (default current sense -).") \
  X(logo_pads, top_guy_idle, INT, 0, 0, 0, 0, padActionTable, HOOK_NONE, JLC_NONE, \
    "Action an idle tap on the top logo pad runs (probe parked, switch in SELECT).") \
  X(logo_pads, bottom_guy_idle, INT, 0, 0, 0, 0, padActionTable, HOOK_NONE, JLC_NONE, \
    "Action an idle tap on the bottom logo pad runs.") \
  X(logo_pads, building_pad_top_idle, INT, 0, 0, 0, 0, padActionTable, HOOK_NONE, JLC_NONE, \
    "Action an idle tap on the top building pad runs.") \
  X(logo_pads, building_pad_bottom_idle, INT, 0, 0, 0, 0, padActionTable, HOOK_NONE, JLC_NONE, \
    "Action an idle tap on the bottom building pad runs.")

// --- [display] -------------------------------------------------------------
// Breadboard LED rendering.
#define JL_CFG_DISPLAY(X) \
  X(display, lines_wires, VINT, JL_DEFAULT_LINES_WIRES, 0, 1, 1, linesWiresTable, HOOK_LINES_WIRES, JLC_LEDS, \
    "Draw nets as full 5-LED wires or single-LED lines (OG hardware forces lines).") \
  X(display, menu_brightness, INT, -10, -100, 100, 5, nullptr, HOOK_NONE, JLC_LEDS, \
    "Brightness offset for the LED menu overlay.") \
  X(display, led_brightness, INT, 10, 0, 255, 5, nullptr, HOOK_NONE, JLC_LEDS, \
    "Base brightness for net LEDs.") \
  X(display, rail_brightness, INT, 55, 0, 255, 5, nullptr, HOOK_NONE, JLC_LEDS, \
    "Brightness for the power rail LEDs.") \
  X(display, special_net_brightness, INT, 20, 0, 255, 5, nullptr, HOOK_NONE, JLC_LEDS, \
    "Brightness for special nets (GND, rails, DACs) drawn on the breadboard.") \
  X(display, net_color_mode, INT, 0, 0, 5, 1, netColorModeTable, HOOK_NONE, JLC_LEDS, \
    "How net colors are assigned: rainbow order, shuffled, or set from serial.")

// --- [serial_1] ------------------------------------------------------------
#define JL_CFG_SERIAL_1(X) \
  X(serial_1, function, INT, 1, 0, 6, 1, uartFunctionTable, HOOK_SERIAL_FUNCTION, JLC_NONE, \
    "What UART 1 does: passthrough to the Arduino header, main control, MicroPython, OLED, or LEDs.") \
  X(serial_1, baud_rate, INT, 115200, 0, 0, 0, nullptr, HOOK_SERIAL_FUNCTION, JLC_NONE, \
    "UART 1 baud rate.") \
  X(serial_1, print_passthrough, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Echo UART 1 passthrough traffic to the main terminal.") \
  X(serial_1, connect_on_boot, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Route UART 1 to the Arduino header rows at boot.") \
  X(serial_1, lock_connection, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Keep the UART 1 routing locked so net edits can't disconnect it.") \
  X(serial_1, autoconnect_flashing, INT, 1, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Automatically route UART 1 when an Arduino flash is detected.") \
  X(serial_1, async_passthrough, BOOL, true, 0, 0, 0, nullptr, HOOK_NONE, JLC_NONE, \
    "Run UART 1 passthrough on the async engine (recommended).") \
  X(serial_1, tag_parsing, INT, 1, 0, 2, 1, tagParsingTable, HOOK_NONE, JLC_NONE, \
    "Parse [tag] commands arriving over UART 1: off, parse + pass through, or parse + strip.") \
  X(serial_1, flash_reset_type, INT, 1, 0, 3, 1, flashTypeTable, HOOK_NONE, JLC_NONE, \
    "Reset dance used when flashing the connected target (AVR, ESP32, RP2040, or none).")

// --- [serial_2] ------------------------------------------------------------
#define JL_CFG_SERIAL_2(X) \
  X(serial_2, function, INT, 3, 0, 6, 1, uartFunctionTable, HOOK_SERIAL_FUNCTION, JLC_NONE, \
    "What UART 2 does (default MicroPython REPL).") \
  X(serial_2, baud_rate, INT, 115200, 0, 0, 0, nullptr, HOOK_SERIAL_FUNCTION, JLC_NONE, \
    "UART 2 baud rate.") \
  X(serial_2, print_passthrough, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Echo UART 2 passthrough traffic to the main terminal.") \
  X(serial_2, connect_on_boot, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Route UART 2 to its rows at boot.") \
  X(serial_2, lock_connection, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Keep the UART 2 routing locked so net edits can't disconnect it.")

// --- [top_oled] ------------------------------------------------------------
#define JL_CFG_TOP_OLED(X) \
  X(top_oled, enabled, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Whether an OLED is attached and should be driven.") \
  X(top_oled, i2c_address, HEX, 0x3C, 0, 0x7F, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "The OLED's I2C address (usually 0x3C).") \
  X(top_oled, width, INT, 128, 0, 256, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "OLED width in pixels.") \
  X(top_oled, height, INT, 32, 0, 128, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "OLED height in pixels.") \
  X(top_oled, rotation, INT, 0, 0, 3, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Display rotation in 90-degree steps.") \
  X(top_oled, connection_type, INT, 0, 0, 3, 1, connectionTypeTable, HOOK_OLED_CONNECTION, JLC_NONE, \
    "How the OLED is wired: crossbar GPIO 7/8, hardwired RP6/RP7, internal I2C0, or custom pins.") \
  X(top_oled, sda_pin, INT, 26, 0, 29, 1, nullptr, HOOK_OLED_PIN, JLC_NONE, \
    "SDA hardware pin (validated against the I2C pin map before applying).") \
  X(top_oled, scl_pin, INT, 27, 0, 29, 1, nullptr, HOOK_OLED_PIN, JLC_NONE, \
    "SCL hardware pin (validated against the I2C pin map before applying).") \
  X(top_oled, gpio_sda, INT, RP_GPIO_7, 0, 0, 0, nullptr, HOOK_NONE, JLC_NONE, \
    "Node define for the SDA routable GPIO (crossbar connection types).") \
  X(top_oled, gpio_scl, INT, RP_GPIO_8, 0, 0, 0, nullptr, HOOK_NONE, JLC_NONE, \
    "Node define for the SCL routable GPIO (crossbar connection types).") \
  X(top_oled, sda_row, INT, NANO_D2, 0, 0, 0, nullptr, HOOK_NONE, JLC_NONE, \
    "Breadboard row the OLED's SDA plugs into (-1 for hardwired types).") \
  X(top_oled, scl_row, INT, NANO_D3, 0, 0, 0, nullptr, HOOK_NONE, JLC_NONE, \
    "Breadboard row the OLED's SCL plugs into (-1 for hardwired types).") \
  X(top_oled, connect_on_boot, INT, 1, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Connect and initialize the OLED at boot.") \
  X(top_oled, lock_connection, INT, 0, 0, 1, 1, nullptr, HOOK_NONE, JLC_NONE, \
    "Keep the OLED's crossbar routing locked so net edits can't disconnect it.") \
  X(top_oled, show_in_terminal, INT, 0, 0, 5, 1, oledMirrorTable, HOOK_SERIAL_FUNCTION, JLC_NONE, \
    "Mirror the OLED into a terminal: off, port_1 (main), port_3, port_5, port_7, or uart.") \
  X(top_oled, font, FONT, 0, 0, 10, 1, nullptr, HOOK_OLED_FONT, JLC_NONE, \
    "OLED font family (by name, from the built-in font list).") \
  X(top_oled, startup_message, STR33, "", 0, 0, 0, nullptr, HOOK_NONE, JLC_NONE, \
    "Text or images/*.bin path shown on the OLED at boot.")

// --- [usb_cdc] -------------------------------------------------------------
#define JL_CFG_USB_CDC(X) \
  X(usb_cdc, ignore_dtr, BOOL, false, 0, 0, 0, nullptr, HOOK_USB_CDC_DTR, JLC_NONE, \
    "Ignore the DTR line so hosts that never assert it (some industrial software) can still talk.")

// --- [usb_audio] -----------------------------------------------------------
// USB Audio Class microphone. The USB spec gives no way to add an interface
// without re-enumerating (~2s CDC port drop), so enabled=true restores the
// mic at BOOT, before the host enumerates - set it once and every power-on
// comes up with the audio device already present.
#define JL_CFG_USB_AUDIO(X) \
  X(usb_audio, enabled, BOOL, false, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Advertise the USB microphone from boot (changing it live re-enumerates and drops CDC ports for ~2s).") \
  X(usb_audio, left, INT, 0, 0, 7, 1, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "ADC channel streamed to the left audio channel.") \
  X(usb_audio, right, INT, 1, 0, 7, 1, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "ADC channel streamed to the right audio channel.") \
  X(usb_audio, rate, INT, 16000, 8000, 48000, 1000, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Sample rate in Hz.") \
  X(usb_audio, full_scale, FLOAT, 8.0f, 0.0f, 10.0f, 0.5f, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "Volts mapped to full-scale PCM.") \
  X(usb_audio, dc_block, BOOL, true, 0, 0, 0, nullptr, HOOK_NONE, JLC_BOOT_ONLY, \
    "High-pass the stream to remove DC offset.")

// Every section list, in file/print order. [config] (the firmware_version
// stamp) is virtual - printed from the running firmware, parsed only for the
// version check - so it is not in this list.
#define JL_CONFIG_ALL_OPTIONS(X) \
  JL_CFG_FIRMWARE(X) \
  JL_CFG_HARDWARE(X) \
  JL_CFG_PROBE(X) \
  JL_CFG_CLICKWHEEL(X) \
  JL_CFG_MEASUREMENT(X) \
  JL_CFG_TERMINAL(X) \
  JL_CFG_UNDO(X) \
  JL_CFG_DACS(X) \
  JL_CFG_DEBUG(X) \
  JL_CFG_ROUTING(X) \
  JL_CFG_SLOTS(X) \
  JL_CFG_CALIBRATION(X) \
  JL_CFG_LOGO_PADS(X) \
  JL_CFG_DISPLAY(X) \
  JL_CFG_SERIAL_1(X) \
  JL_CFG_SERIAL_2(X) \
  JL_CFG_TOP_OLED(X) \
  JL_CFG_USB_CDC(X) \
  JL_CFG_USB_AUDIO(X)

// ---------------------------------------------------------------------------
// Struct generation. One field per X() line, initialized to its default.
// ---------------------------------------------------------------------------
typedef char jl_conf_str16[16];
typedef char jl_conf_str33[33];

#define JL_CT_BOOL   bool
#define JL_CT_INT    int
#define JL_CT_VINT   volatile int
#define JL_CT_FLOAT  float
#define JL_CT_HEX    int
#define JL_CT_FONT   int
#define JL_CT_STR16  jl_conf_str16
#define JL_CT_STR33  jl_conf_str33

#define JL_XFIELD(sect, key, type, def, minv, maxv, step, table, hook, flags, desc) \
    JL_CT_##type key = def;

extern struct config jumperlessConfig;

struct config {
    struct firmware    { JL_CFG_FIRMWARE(JL_XFIELD) }    firmware;
    struct hardware    { JL_CFG_HARDWARE(JL_XFIELD) }    hardware;
    struct probe       { JL_CFG_PROBE(JL_XFIELD) }       probe;
    struct clickwheel  { JL_CFG_CLICKWHEEL(JL_XFIELD) }  clickwheel;
    struct measurement { JL_CFG_MEASUREMENT(JL_XFIELD) } measurement;
    struct terminal    { JL_CFG_TERMINAL(JL_XFIELD) }    terminal;
    struct undo        { JL_CFG_UNDO(JL_XFIELD) }        undo;
    struct dacs        { JL_CFG_DACS(JL_XFIELD) }        dacs;
    struct debug       { JL_CFG_DEBUG(JL_XFIELD) }       debug;
    struct routing     { JL_CFG_ROUTING(JL_XFIELD) }     routing;
    struct slots       { JL_CFG_SLOTS(JL_XFIELD) }       slots;
    struct calibration { JL_CFG_CALIBRATION(JL_XFIELD) } calibration;
    struct logo_pads   { JL_CFG_LOGO_PADS(JL_XFIELD) }   logo_pads;
    struct display     { JL_CFG_DISPLAY(JL_XFIELD) }     display;
    struct serial_1    { JL_CFG_SERIAL_1(JL_XFIELD) }    serial_1;
    struct serial_2    { JL_CFG_SERIAL_2(JL_XFIELD) }    serial_2;
    struct top_oled    { JL_CFG_TOP_OLED(JL_XFIELD) }    top_oled;
    struct usb_cdc     { JL_CFG_USB_CDC(JL_XFIELD) }     usb_cdc;
    struct usb_audio   { JL_CFG_USB_AUDIO(JL_XFIELD) }   usb_audio;
};

#endif // CONFIG_H
