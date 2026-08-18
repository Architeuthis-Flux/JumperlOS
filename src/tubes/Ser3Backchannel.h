/**
 * @file Ser3Backchannel.h
 * @brief USBSer3 machine-readable backchannel protocol.
 *
 * USBSer3 is the third USB CDC interface (port 3) on the Jumperless V5. It is a
 * dedicated *machine* backchannel: a host application (JumperIDE, scripts, a
 * pseudo-logic-analyzer, ...) talks to the firmware here without competing with
 * the human-facing menu on the primary serial port. Everything emitted here is
 * intended to be both machine-parseable (JSON / compact framed lines) and
 * human-skimmable.
 *
 * The implementation lives in Ser3Backchannel.cpp. The only public entry point
 * is SingleCharCommands::serviceUSBSer3() (declared in SingleCharCommands.h and
 * called from the Jerial service loop); everything else is file-local.
 *
 * ===========================================================================
 *  ACCESS MODEL  (the safety gate)
 * ===========================================================================
 * Every registered single-char command carries a Ser3Access tag (see
 * SingleCharCommands.h). Only commands tagged SER3_ALLOWED run on this port;
 * everything else is rejected with a structured error. The intent is that the
 * backchannel can NEVER change system state or block:
 *
 *   SER3_ALLOWED        fast, non-blocking, read-only status query  -> RUN
 *   SER3_INTERACTIVE    enters a mode / waits for more input         -> BLOCK
 *   SER3_MODIFIES_STATE changes connections / config / hardware      -> BLOCK
 *   SER3_IRRELEVANT     terminal-UI only, meaningless to a machine   -> BLOCK
 *   SER3_NOT_A_COMMAND  unregistered trigger                         -> BLOCK
 *
 * IMPORTANT: registerCommand() defaults the access tag to SER3_ALLOWED, so a
 * command is only safe-by-omission if it is genuinely a read-only query. Any
 * command that mutates state (undo `^`, redo `&`, the PSRAM-debug toggle `%`,
 * the persistent board-LED stream toggle `R`, ...) MUST be tagged explicitly
 * SER3_MODIFIES_STATE in initializeCommands(). When in doubt, tag it.
 *
 * ===========================================================================
 *  WIRE PROTOCOL  (hybrid: instant single-char + ':' verbs)
 * ===========================================================================
 * The port is always raw / non-line-buffered, so each keystroke arrives as its
 * own byte. Two command forms coexist:
 *
 *  1. Single character  ── dispatched INSTANTLY, never waits for more bytes.
 *       - Special fast queries handled directly (no command-table lookup):
 *           A = full status (version+adc+current+gpio+nets+power, one JSON)
 *           V = ADC + INA current (JSON)
 *           G = logical GPIO state (JSON)
 *           N = net list (JSON)
 *           K = YAML state dump
 *       - Otherwise the trigger is looked up and gated by its Ser3Access.
 *       - Multi-char modifiers (c!, Y2, R!, ...) are NOT reachable as raw
 *         keystrokes here; use the ':' verbs instead (e.g. :crossbar, :yaml).
 *
 *  2. ':' verb line  ── the ONLY path that waits. After ':' the line is read
 *     until newline (or a 2s idle timeout that resets on each keystroke), with
 *     typed characters echoed back (and backspace handled) so a human can also
 *     drive the port by hand. A bare ':' that yields an empty line prints the
 *     :help document.
 *
 *  3. '.'  ── repeat the last inquiry (single-char or verb). Fast resample.
 *
 * ---------------------------------------------------------------------------
 *  HELP  (self-describing; generated from the verb registry + command table)
 * ---------------------------------------------------------------------------
 *   :help                 full YAML help document (framed, parseable + human)
 *   :help:<verb>          one verb's YAML mapping (e.g. :help:gpio:s)
 *   :help:<char>          single-char command help incl. long helpText
 *   :help:json            same content as :help, JSON shape
 *   :help <topic>         space-separated alias for the colon form
 * Help output is framed ---YAML_HELP_START--- / ---YAML_HELP_END--- so a host
 * peels the frame and yaml.safe_load()s the inner document; humans just read it.
 * The single ':' verb registry (usbSer3_verbs[] in Ser3Backchannel.cpp) is the
 * one source of truth feeding :cmds, :help, dispatch, and the :every min_us.
 *
 * ---------------------------------------------------------------------------
 *  VERBS  (all read-only; see usbSer3_dispatchVerb / :cmds "verbs" array)
 * ---------------------------------------------------------------------------
 *   :cmds                 allowed commands + verb list (pretty JSON)
 *   :cmds_all             every registered command with its access tag
 *   :gpio[:s|d|p|f]       raw 48-pin register dump (see GPIO DUMP below)
 *   :gpio:float           state field including cached floating ('f')
 *   :oled[:quarter|full|b64|raw]   one-shot framebuffer dump (default quarter)
 *   :oled:stream:on[:enc] push the framebuffer on every oled.show()
 *   :oled:stream:off      stop pushing
 *   :leds                 breadboard LED snapshot   leds{<n>:<rgb hex>...}
 *   :crossbar | :xbar     crossbar matrix snapshot  xbar{12x8:<hex>...}
 *   :fs                   filesystem walk (f|path|size / d|path lines)
 *   :json[:section]       JSON state (section = power|nets|gpio|overlays)
 *   :yaml :nets :adc :all|:status :slot :config :history :ver
 *   :bench[:verb]         time each read-only verb (compute-only, NullStream)
 *   :every:<int>:<n>:<verb>   scheduled capture (see SCHEDULED CAPTURE)
 *   :repeat (:r)          repeat last inquiry;  :stop  cancel a capture
 *
 * ---------------------------------------------------------------------------
 *  GPIO DUMP  (compact, one write(), pseudo logic-analyzer friendly)
 * ---------------------------------------------------------------------------
 * One framed line per field, one char per GPIO across all NUM_BANK0_GPIOS pins,
 * built from bulk SIO register reads (no per-pin SDK calls in the hot path):
 *   s{...}  state      '0'/'1'  (+ 'f' cached-floating only when float opt set)
 *   d{...}  direction  'i' in / 'o' out
 *   p{...}  pulls      'n' none / 'u' up / 'd' down / 'b' bus-keeper(both)
 *   f{...}  function   single letter per FUNCSEL (see usbSer3_funcLetter:
 *                      h HSTX, s SPI, u UART, i I2C, p PWM, g SIO, 0/1/2 PIO,
 *                      c GPCK/XIP/TRACE, b USB, a UART_AUX, . none)
 * Floating detection is OFF by default (never triggers an active float read).
 *
 * ---------------------------------------------------------------------------
 *  SCHEDULED CAPTURE  (:every)  &  BENCH  (:bench)
 * ---------------------------------------------------------------------------
 * :every:<interval>:<count>:<verb> samples a verb <count> times spaced by
 * <interval> (suffix us/ms/s; default ms; count<=0 = continuous). The interval
 * is clamped up to a per-verb minimum (usbSer3_verbs[].minUs, refined by
 * :bench). gpio:s uses a precise RAM-resident buffered path (__not_in_flash_func
 * + gpio_get_all() at busy-waited micros() spacing, dumped as caps{...}); every
 * other verb uses a best-effort streamed path. Capture stops on any inbound
 * byte, on :stop, on a flash op (pauseCore2), or a 60s safety cap.
 *
 * :bench times each read-only verb compute-only by routing its output to a
 * NullStream byte counter (so the measurement excludes USB/CDC cost) and seeds
 * the per-verb minimum intervals used by :every.
 *
 * ===========================================================================
 *  PERFORMANCE NOTES
 * ===========================================================================
 *  - All dumps build into a single stack buffer and emit with one write().
 *  - No heap allocation in the per-sample hot paths (the only malloc is the
 *    one-time :every gpio:s sample buffer, which falls back to streamed mode
 *    on failure).
 *  - OLED push-on-show is back-pressure aware (availableForWrite gating +
 *    FNV-1a change detection) and drops frames rather than stalling the UI.
 */

#ifndef SER3_BACKCHANNEL_H
#define SER3_BACKCHANNEL_H

// The backchannel's public surface is SingleCharCommands::serviceUSBSer3(),
// declared in SingleCharCommands.h and implemented in Ser3Backchannel.cpp.
// This header is documentation + include anchor for that translation unit;
// it intentionally exposes no additional symbols (all helpers are file-local).
#include "SingleCharCommands.h"

#endif // SER3_BACKCHANNEL_H
