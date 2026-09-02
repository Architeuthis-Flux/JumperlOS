/**
 * @file Ser3Backchannel.cpp
 * @brief USBSer3 machine-readable backchannel implementation.
 *
 * See Ser3Backchannel.h for the full protocol / access-model documentation.
 *
 * This translation unit owns the entire USBSer3 backchannel: the machine query
 * handlers, the GPIO/LED/crossbar/OLED dumps, the command-list emitter, the
 * :bench timer, the :every scheduled-capture engine, and the dispatcher behind
 * SingleCharCommands::serviceUSBSer3(). All helpers are file-local; the only
 * externally visible definition is the serviceUSBSer3() member (declared in
 * SingleCharCommands.h, called from the Jerial service loop).
 *
 * The include set mirrors SingleCharCommands.cpp (where this code used to live)
 * so symbol visibility is identical.
 */

#include "Ser3Backchannel.h"
#include "SingleCharCommands.h"
#include "Apps.h"
#include "CH446Q.h"
#include "Commands.h"
#include "Debugs.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"
#include "Graphics.h"
#include "Highlighting.h"
#include "Jerial.h"
#include "JumperlOS.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "Menus.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "Python_Proper.h"
#include "States.h"
#include "FakeGpio.h"
#include "configManager.h"
#include "externVars.h"
#include "hardware/gpio.h"
#include "oled.h"
#include "JsonState.h"
#include "Undo.h"

// ============================================================================
// USBSer3 Backchannel - Machine-Parseable Commands
// ============================================================================

extern Adafruit_USBD_CDC USBSer3;

// ----------------------------------------------------------------------------
// Verb registry - single source of truth for the ':' verbs.
// ----------------------------------------------------------------------------
// Replaces the old flat verbs[] string array (advertised by :cmds) and the
// parallel usbSer3_verbTimings[] table. Each row is everything the backchannel
// knows about a verb: how to call it, what it returns, and the per-sample floor
// :every clamps to (mutable; :bench overwrites min_us with measured values).
// The :help YAML document is generated straight from this table, so the help is
// never out of sync with what :cmds advertises or what dispatch accepts.
struct Ser3Verb {
    const char* name;     // canonical verb, e.g. "gpio:s"
    const char* summary;  // one human line
    const char* args;     // "" or a short arg grammar, e.g. "[s|d|p|f|float]"
    const char* output;   // one-line shape of the reply
    const char* example;  // a copy-pasteable invocation, e.g. ":gpio:s"
    uint32_t    minUs;    // per-sample floor for :every (0 = not a sampled query)
};

// Order is the order :cmds and :help advertise. Sampled read-only queries carry
// a real minUs; meta/control verbs (cmds, help, bench, every, repeat, stop) use
// 0 because :every never schedules them.
static Ser3Verb usbSer3_verbs[] = {
    { "cmds",      "Allowed commands + verb list",                 "",               "json",                          ":cmds",            0 },
    { "cmds_all",  "Every registered command with access tag",     "",               "json",                          ":cmds_all",        0 },
    { "help",      "This help document",                           "[:verb|:char|:json]", "yaml",                     ":help",            0 },
    { "gpio",      "All four GPIO fields (state/dir/pull/func)",    "[s|d|p|f|float]","s{..}d{..}p{..}f{..}",          ":gpio",            500 },
    { "gpio:s",    "48-pin GPIO state snapshot",                   "[float]",        "s{<48 0/1[/f]>}",               ":gpio:s",          20 },
    { "gpio:d",    "48-pin GPIO direction",                        "",               "d{<48 i/o>}",                   ":gpio:d",          20 },
    { "gpio:p",    "48-pin GPIO pulls",                            "",               "p{<48 n/u/d/b>}",               ":gpio:p",          200 },
    { "gpio:f",    "48-pin GPIO function (FUNCSEL letter)",        "",               "f{<48 letters>}",               ":gpio:f",          200 },
    { "oled",      "One-shot OLED framebuffer dump",               "[quarter|full|b64|raw]", "encoded frame",         ":oled",            8000 },
    { "oled:stream:on",  "Stream framebuffer on every oled.show()","[:enc]",         "frames",                        ":oled:stream:on",  0 },
    { "oled:stream:off", "Stop streaming the framebuffer",         "",               "{oled_stream:off}",             ":oled:stream:off", 0 },
    { "leds",      "Breadboard LED snapshot",                      "",               "leds{<n>:<rgb hex>...}",        ":leds",            2000 },
    { "crossbar",  "Crossbar matrix snapshot",                     "",               "xbar{12x8:<hex>...}",           ":crossbar",        400 },
    { "xbar",      "Alias of crossbar",                            "",               "xbar{12x8:<hex>...}",           ":xbar",            400 },
    { "fs",        "Filesystem walk",                              "",               "f|path|size / d|path",          ":fs",              0 },
    { "json",      "JSON state (optionally one section)",          "[:section]",     "json",                          ":json:nets",       6000 },
    { "yaml",      "Full state YAML dump",                         "",               "---YAML_START---..---YAML_END---", ":yaml",         6000 },
    { "nets",      "Net list",                                     "",               "json",                          ":nets",            2000 },
    { "adc",       "ADC voltages + INA current",                   "",               "json",                          ":adc",             4000 },
    { "all",       "Full status (version+adc+current+gpio+nets+power)", "",          "json",                          ":all",             9000 },
    { "status",    "Alias of all",                                 "",               "json",                          ":status",          9000 },
    { "slot",      "Active slot query",                            "",               "slot info",                     ":slot",            30 },
    { "history",   "Undo/redo history list",                       "",               "history",                       ":history",         0 },
    { "ver",       "Firmware version",                             "",               "version",                       ":ver",             50 },
    { "bench",     "Time each read-only verb (compute-only)",      "[:verb]",        "bench{...}",                    ":bench",           0 },
    { "every",     "Scheduled capture of a verb",                  "<int>:<n>:<verb>","cap{...} caps{...}",           ":every:1ms:100:gpio:s", 0 },
    { "repeat",    "Repeat the last inquiry",                      "",               "(last reply)",                  ":repeat",          0 },
    { "stop",      "Cancel a running capture",                     "",               "{stop:1}",                      ":stop",            0 },
};
static const unsigned usbSer3_nVerbs = sizeof(usbSer3_verbs) / sizeof(usbSer3_verbs[0]);

// All backchannel output is parameterized on a Stream* so the same handlers
// can drive USBSer3 (normal) or a NullStream (the :bench compute-only pass).

static void usbSer3_printNormalized(Stream* out, const String& s) {
    for (unsigned int i = 0; i < s.length(); i++) {
        char ch = s[i];
        if (ch == '\n') {
            out->print("\r\n");
            if (i + 1 < s.length() && s[i + 1] == '\r') i++;
        } else if (ch == '\r') {
            out->print("\r\n");
            if (i + 1 < s.length() && s[i + 1] == '\n') i++;
        } else {
            out->write(ch);
        }
    }
}

static void usbSer3_sendAllStatus(Stream* out) {
    extern JumperlessState globalState;
    extern const char firmwareVersion[];

    SlotManager& mgr = SlotManager::getInstance();

    out->print("{\"version\":\"");
    out->print(firmwareVersion);
    // "slot" is -1 for a file context (never the internal -2), and
    // "slot_path" is ALWAYS emitted - numbered slots report their canonical
    // /slots/slotN.yaml. Numbered flows are byte-identical apart from the new
    // field; the app must tolerate slot: -1.
    out->print("\",\"slot\":");
    out->print(mgr.isPathContext() ? -1 : mgr.getActiveSlot());
    out->print(",\"slot_path\":\"");
    out->print(mgr.getActiveSlotPath());
    out->print("\",\r\n");

    out->print("\"adc\":{");
    for (int i = 0; i < 5; i++) {
        if (i > 0) out->print(',');
        out->printf("\"adc%d\":%.4f", i, readAdcVoltage(i, 8));
    }
    out->print("},\r\n");

#if defined(OG_JUMPERLESS)
    out->printf("\"current\":{\"ina0_mA\":%.3f,\"ina1_mA\":0.000},\r\n",
                INA0.getCurrent_mA()- currentReadingOffset0_mA);
#else
    out->printf("\"current\":{\"ina0_mA\":%.3f,\"ina1_mA\":%.3f},\r\n",
                INA0.getCurrent_mA()- currentReadingOffset0_mA, INA1.getCurrent_mA()- currentReadingOffset1_mA);
#endif

    const char* readingNames[] = {"low", "high", "float", "unknown"};
    out->print("\"gpio\":[");
    for (int i = 0; i < 10; i++) {
        if (i > 0) out->print(',');
        int reading = gpioReading[i];
        if (reading < 0 || reading > 3) reading = 3;
        out->printf("{\"net\":%d,\"reading\":\"%s\"}", gpioNet[i], readingNames[reading]);
    }
    out->print("]\r\n");   // (the sections below carry their own leading comma)

    String json = JsonState::getJumperlessStateJSON("nets");
    if (json.length() > 0) {
        out->print(",\"nets\":");
        usbSer3_printNormalized(out, json);
    }

    json = JsonState::getJumperlessStateJSON("power");
    if (json.length() > 0) {
        out->print(",\"power\":");
        usbSer3_printNormalized(out, json);
    }

    out->print("}\r\n");
}

static void usbSer3_sendYAML(Stream* out) {
    extern JumperlessState globalState;
    String yaml;
    if (globalState.toYAML(yaml, 0)) {
        out->print("---YAML_START---\r\n");
        usbSer3_printNormalized(out, yaml);
        out->print("---YAML_END---\r\n");
    } else {
        out->print("{\"error\":\"yaml_failed\"}\r\n");
    }
}

static void usbSer3_sendADC(Stream* out) {
    out->print("{\"adc\":{");
    for (int i = 0; i < 5; i++) {
        if (i > 0) out->print(',');
        out->printf("\"adc%d\":%.4f", i, readAdcVoltage(i, 8));
    }
#if defined(OG_JUMPERLESS)
    out->printf("},\"current\":{\"ina0_mA\":%.3f,\"ina1_mA\":0.000}}\r\n",
                INA0.getCurrent_mA()- currentReadingOffset0_mA);
#else
    out->printf("},\"current\":{\"ina0_mA\":%.3f,\"ina1_mA\":%.3f}}\r\n",
                INA0.getCurrent_mA()- currentReadingOffset0_mA, INA1.getCurrent_mA()- currentReadingOffset1_mA);
#endif
}

static void usbSer3_sendGpioJson(Stream* out) {
    const char* readingNames[] = {"low", "high", "float", "unknown"};
    out->print("{\"gpio\":[");
    for (int i = 0; i < 10; i++) {
        if (i > 0) out->print(',');
        int reading = gpioReading[i];
        if (reading < 0 || reading > 3) reading = 3;
        out->printf("{\"net\":%d,\"reading\":\"%s\"}", gpioNet[i], readingNames[reading]);
    }
    out->print("]}\r\n");
}

static void usbSer3_sendNets(Stream* out) {
    String json = JsonState::getJumperlessStateJSON("nets");
    if (json.length() > 0) {
        usbSer3_printNormalized(out, json);
        out->print("\r\n");
    } else {
        out->print("{\"nets\":[]}\r\n");
    }
}

// ----------------------------------------------------------------------------
// Raw GPIO register dump (machine-readable, pseudo logic analyzer)
// ----------------------------------------------------------------------------
// Bulk SIO register reads (not 48 per-pin SDK calls), composed into a single
// stack buffer and emitted with one write(). Covers all NUM_BANK0_GPIOS pins.

// Single-letter function code per RP2350 FUNCSEL (see gpio_function_t).
static char usbSer3_funcLetter(gpio_function_t f) {
    switch ((int)f) {
        case 0:  return 'h'; // HSTX
        case 1:  return 's'; // SPI
        case 2:  return 'u'; // UART
        case 3:  return 'i'; // I2C
        case 4:  return 'p'; // PWM
        case 5:  return 'g'; // SIO (general GPIO)
        case 6:  return '0'; // PIO0
        case 7:  return '1'; // PIO1
        case 8:  return '2'; // PIO2
        case 9:  return 'c'; // GPCK / XIP_CS1 / TRACE (pin-dependent)
        case 10: return 'b'; // USB
        case 11: return 'a'; // UART_AUX
        default: return '.'; // NULL / unknown
    }
}

// field: 's' state, 'd' direction, 'p' pulls, 'f' function.
// includeFloat only affects 's': reports cached floating ('f') for the 10
// logical GPIO pins (gpioReadFloating[]); never triggers an active float read.
static void usbSer3_sendRawGpioField(Stream* out, char field, bool includeFloat) {
    const int n = (int)NUM_BANK0_GPIOS;
    char buf[80];   // prefix + '{' + <=48 chars + "}\r\n"
    int len = 0;
    buf[len++] = field;
    buf[len++] = '{';

    if (field == 's') {
        uint64_t in = gpio_get_all64();
        for (int i = 0; i < n; i++) {
            char ch = ((in >> i) & 1ULL) ? '1' : '0';
            if (includeFloat && i < 10 && gpioReadFloating[i]) ch = 'f';
            buf[len++] = ch;
        }
    } else if (field == 'd') {
        uint32_t oe_lo = sio_hw->gpio_oe;
        uint32_t oe_hi = sio_hw->gpio_hi_oe;
        for (int i = 0; i < n; i++) {
            bool isOut = (i < 32) ? ((oe_lo >> i) & 1u) : ((oe_hi >> (i - 32)) & 1u);
            buf[len++] = isOut ? 'o' : 'i';
        }
    } else if (field == 'p') {
        for (int i = 0; i < n; i++) {
            bool up = gpio_is_pulled_up(i);
            bool dn = gpio_is_pulled_down(i);
            buf[len++] = (up && dn) ? 'b' : (up ? 'u' : (dn ? 'd' : 'n'));
        }
    } else if (field == 'f') {
        for (int i = 0; i < n; i++) {
            buf[len++] = usbSer3_funcLetter(gpio_get_function(i));
        }
    } else {
        return;
    }

    buf[len++] = '}';
    buf[len++] = '\r';
    buf[len++] = '\n';
    out->write((const uint8_t*)buf, len);
}

// field == 0 -> all four lines; otherwise a single field.
static void usbSer3_sendRawGpio(Stream* out, char field, bool includeFloat) {
    if (field == 0) {
        usbSer3_sendRawGpioField(out, 's', includeFloat);
        usbSer3_sendRawGpioField(out, 'd', includeFloat);
        usbSer3_sendRawGpioField(out, 'p', includeFloat);
        usbSer3_sendRawGpioField(out, 'f', includeFloat);
    } else {
        usbSer3_sendRawGpioField(out, field, includeFloat);
    }
}

// ----------------------------------------------------------------------------
// Breadboard LED + crossbar snapshots (call-response only; never the loop1 path)
// ----------------------------------------------------------------------------
static const char USBSER3_HEX[] = "0123456789abcdef";

static void usbSer3_sendLeds(Stream* out) {
    // Briefly wait for any in-flight WS2812 DMA so the snapshot is coherent.
    uint32_t t0 = micros();
    while (bbleds.isDMABusy() && (micros() - t0) < 2000) { }

    static char b[16 + LED_COUNT * 6 + 8];
    int len = snprintf(b, sizeof(b), "leds{%d:", LED_COUNT);
    for (int i = 0; i < LED_COUNT; i++) {
        uint32_t c = leds.getPixelColor(i) & 0xFFFFFFu;
        b[len++] = USBSER3_HEX[(c >> 20) & 0xF];
        b[len++] = USBSER3_HEX[(c >> 16) & 0xF];
        b[len++] = USBSER3_HEX[(c >> 12) & 0xF];
        b[len++] = USBSER3_HEX[(c >> 8) & 0xF];
        b[len++] = USBSER3_HEX[(c >> 4) & 0xF];
        b[len++] = USBSER3_HEX[c & 0xF];
    }
    b[len++] = '}'; b[len++] = '\r'; b[len++] = '\n';
    out->write((const uint8_t*)b, len);
}

static void usbSer3_sendCrossbar(Stream* out) {
    chipXYBitfield snap[12];
    memcpy(snap, lastChipXY, sizeof(snap));   // single coherent snapshot
    static char b[16 + 12 * 8 * 4 + 8];
    int len = snprintf(b, sizeof(b), "xbar{12x8:");
    for (int chip = 0; chip < 12; chip++) {
        for (int y = 0; y < 8; y++) {
            uint16_t v = snap[chip].connected[y];
            b[len++] = USBSER3_HEX[(v >> 12) & 0xF];
            b[len++] = USBSER3_HEX[(v >> 8) & 0xF];
            b[len++] = USBSER3_HEX[(v >> 4) & 0xF];
            b[len++] = USBSER3_HEX[v & 0xF];
        }
    }
    b[len++] = '}'; b[len++] = '\r'; b[len++] = '\n';
    out->write((const uint8_t*)b, len);
}

// ----------------------------------------------------------------------------
// Command list (verbose :cmds + compact allowed-char string on rejection)
// ----------------------------------------------------------------------------
static const char* usbSer3_ser3AccessName(Ser3Access a) {
    switch (a) {
        case SER3_ALLOWED:        return "allowed";
        case SER3_INTERACTIVE:    return "interactive";
        case SER3_MODIFIES_STATE: return "status_only";
        case SER3_IRRELEVANT:     return "irrelevant";
        default:                  return "not_a_command";
    }
}

static void usbSer3_jsonStr(Stream* out, const char* s) {
    out->write('"');
    for (const char* p = s ? s : ""; *p; p++) {
        if (*p == '"' || *p == '\\') out->write('\\');
        out->write(*p);
    }
    out->write('"');
}

// ----------------------------------------------------------------------------
// YAML emitters (hand-rolled; no library on the firmware). The help system
// emits YAML because it reads cleanly in a raw terminal (indentation, short
// keys, block scalars for prose) yet round-trips through yaml.safe_load().
// ----------------------------------------------------------------------------

// Double-quoted flow scalar: always safe for any single-line string regardless
// of leading specials, colons, etc. Escapes \, ", and control chars.
static void usbSer3_yamlStr(Stream* out, const char* s) {
    out->write('"');
    for (const char* p = s ? s : ""; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { out->write('\\'); out->write(c); }
        else if (c == '\n')        { out->print("\\n"); }
        else if (c == '\r')        { /* drop bare CR */ }
        else if (c == '\t')        { out->print("\\t"); }
        else if ((unsigned char)c < 0x20) { out->printf("\\x%02x", (unsigned char)c); }
        else out->write(c);
    }
    out->write('"');
}

// Literal block scalar ('|') for multiline prose (preserves newlines for human
// reading). Caller has already printed the "key: " and a newline is added here
// after the '|'. Every content line is indented by `indent` spaces.
static void usbSer3_yamlBlock(Stream* out, const char* s, int indent) {
    out->print("|\r\n");
    auto pad = [&]() { for (int i = 0; i < indent; i++) out->write(' '); };
    pad();
    for (const char* p = s ? s : ""; *p; p++) {
        if (*p == '\r') continue;
        out->write(*p);
        if (*p == '\n') pad();
    }
    out->print("\r\n");
}

// Emit the command list as pretty-printed (one entry per line) JSON. Newlines
// are JSON whitespace, so this stays machine-parseable while being readable in
// a raw terminal. Always single-line per record so nothing wraps in a pane.
// `all` = false (":cmds") lists only backchannel-usable (SER3_ALLOWED) commands;
// `all` = true (":cmds_all") lists every registered command with its access.
static void usbSer3_sendCmds(Stream* out, bool all = false) {
    int count = singleCharCommands.getCommandCount();
    out->print("{\r\n  \"cmds\": [\r\n");
    bool first = true;
    for (int i = 0; i < count; i++) {
        const Command* cmd = singleCharCommands.getCommandByIndex(i);
        if (!cmd || cmd->trigger == 0) continue;
        if (!all && cmd->ser3Access != SER3_ALLOWED) continue;
        if (!first) out->print(",\r\n");
        first = false;
        char t = cmd->trigger;
        out->print("    {\"c\": ");
        if (t >= 32 && t < 127) {
            char tmp[2] = { t, 0 };
            usbSer3_jsonStr(out, tmp);
        } else {
            out->printf("%d", (int)t);   // control char as numeric code
        }
        out->printf(", \"ser3\": \"%s\", \"desc\": ", usbSer3_ser3AccessName(cmd->ser3Access));
        usbSer3_jsonStr(out, cmd->shortDesc ? cmd->shortDesc : "");
        out->print("}");
    }
    out->print("\r\n  ],\r\n  \"verbs\": [\r\n");
    const unsigned perLine = 6;
    for (unsigned i = 0; i < usbSer3_nVerbs; i++) {
        if (i % perLine == 0) out->print("    ");
        usbSer3_jsonStr(out, usbSer3_verbs[i].name);
        if (i + 1 < usbSer3_nVerbs) out->print(",");
        if ((i + 1) % perLine == 0 || i + 1 == usbSer3_nVerbs) out->print("\r\n");
        else out->print(" ");
    }
    out->print("  ]\r\n}\r\n");
}

// Build the compact list of single chars usable on the backchannel right now:
// the special handlers plus every SER3_ALLOWED registered command.
static void usbSer3_buildAllowedChars(char* outBuf, size_t n) {
    size_t len = 0;
    const char* specials = "AVGNK.:";   // special handlers + repeat + verb prefix
    for (const char* p = specials; *p && len + 1 < n; p++) outBuf[len++] = *p;
    int count = singleCharCommands.getCommandCount();
    for (int i = 0; i < count && len + 1 < n; i++) {
        const Command* cmd = singleCharCommands.getCommandByIndex(i);
        if (!cmd) continue;
        if (cmd->ser3Access == SER3_ALLOWED && cmd->trigger >= 33 && cmd->trigger < 127) {
            outBuf[len++] = cmd->trigger;
        }
    }
    outBuf[len] = 0;
}

// ----------------------------------------------------------------------------
// Self-describing help (:help / :help:<verb> / :help:<char>) - YAML primary
// ----------------------------------------------------------------------------
// Generated straight from usbSer3_verbs[] and the single-char command table, so
// the help can never drift from what dispatch actually accepts. Framed with
// ---YAML_HELP_START--- / ---YAML_HELP_END--- (matching usbSer3_sendYAML()), so
// a host peels the frame and yaml.safe_load()s the inner document; a human just
// reads it. Indentation is two-space; multi-line prose uses block scalars.

// Emit one registry row as a YAML mapping list-item (under "verbs:").
static void usbSer3_emitVerbYaml(Stream* out, const Ser3Verb& v, const char* indent) {
    out->printf("%s- name: ", indent);   usbSer3_yamlStr(out, v.name);    out->print("\r\n");
    out->printf("%s  summary: ", indent); usbSer3_yamlStr(out, v.summary); out->print("\r\n");
    out->printf("%s  args: ", indent);    usbSer3_yamlStr(out, v.args);    out->print("\r\n");
    out->printf("%s  output: ", indent);  usbSer3_yamlStr(out, v.output);  out->print("\r\n");
    out->printf("%s  example: ", indent); usbSer3_yamlStr(out, v.example); out->print("\r\n");
    out->printf("%s  min_us: %lu\r\n", indent, (unsigned long)v.minUs);
}

// Full help document.
static void usbSer3_sendHelp(Stream* out) {
    out->print("---YAML_HELP_START---\r\n");
    out->print("ser3_help: 1\r\n");

    out->print("protocol:\r\n");
    out->print("  single_char: ");
    usbSer3_yamlStr(out, "Instant dispatch; no line collection. Multi-char modifiers use ':' verbs.");
    out->print("\r\n  char_as_verb: ");
    usbSer3_yamlStr(out, "':n' == bare 'n' for any allowed single-char command. Useful when scripting with a consistent ':' prefix.");
    out->print("\r\n  colon_verbs: ");
    usbSer3_yamlStr(out, "Only ':' blocks; 2s idle timeout; chars echoed; Enter terminates.");
    out->print("\r\n  repeat: ");
    usbSer3_yamlStr(out, "'.' re-runs the last inquiry (single char or :verb).");
    out->print("\r\n  bare_colon: ");
    usbSer3_yamlStr(out, "An empty ':' line shows this help.");
    out->print("\r\n");

    out->print("fast_queries:\r\n");
    out->print("  - { key: A, summary: "); usbSer3_yamlStr(out, "Full status JSON (version+adc+current+gpio+nets+power)"); out->print(" }\r\n");
    out->print("  - { key: V, summary: "); usbSer3_yamlStr(out, "ADC voltages + INA current (JSON)"); out->print(" }\r\n");
    out->print("  - { key: G, summary: "); usbSer3_yamlStr(out, "Logical GPIO state (JSON)"); out->print(" }\r\n");
    out->print("  - { key: N, summary: "); usbSer3_yamlStr(out, "Net list (JSON)"); out->print(" }\r\n");
    out->print("  - { key: K, summary: "); usbSer3_yamlStr(out, "Full state YAML dump"); out->print(" }\r\n");

    out->print("access:\r\n");
    out->print("  allowed: ");     usbSer3_yamlStr(out, "Read-only; safe on the backchannel -> runs"); out->print("\r\n");
    out->print("  interactive: "); usbSer3_yamlStr(out, "Enters a mode / waits for input -> blocked"); out->print("\r\n");
    out->print("  status_only: "); usbSer3_yamlStr(out, "Mutates state -> blocked"); out->print("\r\n");
    out->print("  irrelevant: ");  usbSer3_yamlStr(out, "Terminal-UI only -> blocked"); out->print("\r\n");
    out->print("  detail: ");      usbSer3_yamlStr(out, "Per-command access tags: :cmds_all"); out->print("\r\n");

    out->print("encodings:\r\n");
    out->print("  oled: [quarter, full, b64, raw]\r\n");
    out->print("  gpio_state: "); usbSer3_yamlStr(out, "0=low 1=high f=cached-float"); out->print("\r\n");
    out->print("  gpio_dir: ");   usbSer3_yamlStr(out, "i=in o=out"); out->print("\r\n");
    out->print("  gpio_pull: ");  usbSer3_yamlStr(out, "n=none u=up d=down b=bus-keeper"); out->print("\r\n");
    out->print("  gpio_func: ");  usbSer3_yamlStr(out, "h HSTX, s SPI, u UART, i I2C, p PWM, g SIO, 0/1/2 PIO, c GPCK/XIP/TRACE, b USB, a UART_AUX, . none"); out->print("\r\n");

    out->print("verbs:\r\n");
    for (auto& v : usbSer3_verbs) usbSer3_emitVerbYaml(out, v, "  ");

    // Single-char commands that are also callable as ":char" verbs.
    out->print("single_char_cmds:\r\n");
    out->print("  note: ");
    usbSer3_yamlStr(out, "All 'allowed' cmds work as bare char OR as :char verb (e.g. 'n' == ':n'). Special queries A/V/G/N/K work as :A/:V/:G/:N/:K too.");
    out->print("\r\n  cmds:\r\n");
    // Special machine-query handlers first (not in the registered command table).
    struct { char key; const char* desc; } specials[] = {
        { 'A', "Full status JSON (version+adc+current+gpio+nets+power)" },
        { 'V', "ADC voltages + INA current (JSON)" },
        { 'G', "Logical GPIO state (JSON)" },
        { 'N', "Net list (JSON)" },
        { 'K', "Full state YAML dump" },
    };
    for (auto& s : specials) {
        char tmp[2] = { s.key, 0 };
        out->print("    - { key: "); usbSer3_yamlStr(out, tmp);
        out->print(", short: ");     usbSer3_yamlStr(out, s.desc);
        out->print(" }\r\n");
    }
    // SER3_ALLOWED registered commands (printable chars only).
    int cmdCount = singleCharCommands.getCommandCount();
    for (int i = 0; i < cmdCount; i++) {
        const Command* cmd = singleCharCommands.getCommandByIndex(i);
        if (!cmd || cmd->trigger < 33 || cmd->trigger >= 127) continue;
        if (cmd->ser3Access != SER3_ALLOWED) continue;
        char tmp[2] = { (char)cmd->trigger, 0 };
        out->print("    - { key: "); usbSer3_yamlStr(out, tmp);
        out->print(", short: ");     usbSer3_yamlStr(out, cmd->shortDesc ? cmd->shortDesc : "");
        out->print(" }\r\n");
    }

    out->print("---YAML_HELP_END---\r\n");
}

// One verb as a short YAML mapping, or a YAML error for an unknown verb.
static bool usbSer3_sendVerbHelp(Stream* out, const String& name) {
    String n = name; n.toLowerCase();
    for (auto& v : usbSer3_verbs) {
        if (n == v.name) {
            out->print("---YAML_HELP_START---\r\n");
            out->print("verb:\r\n");
            usbSer3_emitVerbYaml(out, v, "  ");
            out->print("---YAML_HELP_END---\r\n");
            return true;
        }
    }
    return false;
}

// Single-char command help, exposing the long helpText from the command table.
static bool usbSer3_sendCmdHelp(Stream* out, char ch) {
    const Command* cmd = singleCharCommands.getCommand(ch);
    if (!cmd || cmd->trigger == 0) return false;
    out->print("---YAML_HELP_START---\r\n");
    out->print("cmd: ");  { char tmp[2] = { ch, 0 }; usbSer3_yamlStr(out, tmp); } out->print("\r\n");
    out->printf("ser3: %s\r\n", usbSer3_ser3AccessName(cmd->ser3Access));
    out->print("short: "); usbSer3_yamlStr(out, cmd->shortDesc ? cmd->shortDesc : ""); out->print("\r\n");
    if (cmd->helpText && cmd->helpText[0]) {
        out->print("help: ");
        usbSer3_yamlBlock(out, cmd->helpText, 2);
    }
    out->print("---YAML_HELP_END---\r\n");
    return true;
}

// JSON variant of the full help (optional :help:json) for clients that already
// ingest JSON everywhere. Same content as usbSer3_sendHelp, machine-only shape.
static void usbSer3_sendHelpJson(Stream* out) {
    out->print("{\"ser3_help\":1,\"verbs\":[");
    for (unsigned i = 0; i < usbSer3_nVerbs; i++) {
        const Ser3Verb& v = usbSer3_verbs[i];
        if (i) out->print(",");
        out->print("{\"name\":");    usbSer3_jsonStr(out, v.name);
        out->print(",\"summary\":"); usbSer3_jsonStr(out, v.summary);
        out->print(",\"args\":");    usbSer3_jsonStr(out, v.args);
        out->print(",\"output\":");  usbSer3_jsonStr(out, v.output);
        out->print(",\"example\":"); usbSer3_jsonStr(out, v.example);
        out->printf(",\"min_us\":%lu}", (unsigned long)v.minUs);
    }
    out->print("],\"single_char_cmds\":[");
    // Special machine-query handlers.
    struct { char key; const char* desc; } specials[] = {
        { 'A', "Full status JSON" },
        { 'V', "ADC voltages + INA current" },
        { 'G', "Logical GPIO state" },
        { 'N', "Net list" },
        { 'K', "Full state YAML dump" },
    };
    bool first = true;
    for (auto& s : specials) {
        if (!first) out->print(","); first = false;
        char tmp[2] = { s.key, 0 };
        out->print("{\"key\":"); usbSer3_jsonStr(out, tmp);
        out->print(",\"short\":"); usbSer3_jsonStr(out, s.desc);
        out->print(",\"ser3\":\"special\"}");
    }
    int cmdCount = singleCharCommands.getCommandCount();
    for (int i = 0; i < cmdCount; i++) {
        const Command* cmd = singleCharCommands.getCommandByIndex(i);
        if (!cmd || cmd->trigger < 33 || cmd->trigger >= 127) continue;
        if (cmd->ser3Access != SER3_ALLOWED) continue;
        char tmp[2] = { (char)cmd->trigger, 0 };
        out->print(",{\"key\":"); usbSer3_jsonStr(out, tmp);
        out->print(",\"short\":"); usbSer3_jsonStr(out, cmd->shortDesc ? cmd->shortDesc : "");
        out->print(",\"ser3\":\"allowed\"}");
    }
    out->print("]}\r\n");
}

// ----------------------------------------------------------------------------
// OLED encoding helpers
// ----------------------------------------------------------------------------
static uint8_t usbSer3_encFromName(const String& s) {
    if (s == "full") return OLED_ENC_FULL;
    if (s == "b64" || s == "base64") return OLED_ENC_B64;
    if (s == "raw") return OLED_ENC_RAW;
    return OLED_ENC_QUARTER;   // "", "quarter", anything else
}
static const char* usbSer3_encName(uint8_t e) {
    switch (e) {
        case OLED_ENC_FULL: return "full";
        case OLED_ENC_B64:  return "b64";
        case OLED_ENC_RAW:  return "raw";
        default:            return "quarter";
    }
}

// ----------------------------------------------------------------------------
// Per-verb minimum sample interval (microseconds) for the :every scheduler.
// Backed by usbSer3_verbs[].minUs (seeded with conservative defaults; :bench
// overwrites with measured values). Sampled queries carry a nonzero floor;
// meta/control verbs report 0, so they fall through to the 100us default below.
// ----------------------------------------------------------------------------
static uint32_t usbSer3_verbMinUs(const String& verb) {
    String v = verb; v.toLowerCase();
    for (auto& t : usbSer3_verbs) if (t.minUs && v == t.name) return t.minUs;
    int c = v.indexOf(':');           // fall back to head (e.g. json:nets -> json)
    if (c > 0) {
        String h = v.substring(0, c);
        for (auto& t : usbSer3_verbs) if (t.minUs && h == t.name) return t.minUs;
    }
    return 100;
}
static void usbSer3_setVerbMinUs(const String& verb, uint32_t us) {
    String v = verb; v.toLowerCase();
    for (auto& t : usbSer3_verbs) if (v == t.name) { t.minUs = us; return; }
}

// ----------------------------------------------------------------------------
// Leaf verb dispatch (single read-only query). No :every / :repeat / :bench
// here (those are handled by the caller to avoid recursion). `out` lets bench
// route output to a NullStream.
// ----------------------------------------------------------------------------
static void usbSer3_dispatchVerb(Stream* out, const String& verb) {
    int sp = verb.indexOf(' ');
    int c1 = verb.indexOf(':');
    String head, rest;
    if (sp > 0 && (c1 < 0 || sp < c1)) {   // space-separated head, e.g. "help gpio:s"
        head = verb.substring(0, sp);
        rest = verb.substring(sp + 1);
        rest.trim();
    } else {
        head = (c1 < 0) ? verb : verb.substring(0, c1);
        rest = (c1 < 0) ? String("") : verb.substring(c1 + 1);
    }
    head.toLowerCase();

    if (head == "cmds_all" || head == "cmdsall") {
        usbSer3_sendCmds(out, true);
    } else if (head == "cmds") {
        usbSer3_sendCmds(out, false);
    } else if (head == "help") {
        String topic = rest; topic.trim();
        String tl = topic; tl.toLowerCase();
        if (topic.length() == 0) {
            usbSer3_sendHelp(out);
        } else if (tl == "json") {
            usbSer3_sendHelpJson(out);
        } else if (tl == "all") {
            usbSer3_sendCmds(out, true);   // :help:all == :cmds_all
        } else if (usbSer3_sendVerbHelp(out, topic)) {
            // matched a registry verb
        } else if (topic.length() == 1 && usbSer3_sendCmdHelp(out, topic[0])) {
            // matched a single registered command char
        } else {
            out->print("---YAML_HELP_START---\r\n");
            out->print("error: unknown_help_topic\r\ntopic: ");
            usbSer3_yamlStr(out, topic.c_str());
            out->print("\r\nhint: ");
            usbSer3_yamlStr(out, ":help lists every verb");
            out->print("\r\n---YAML_HELP_END---\r\n");
        }
    } else if (head == "gpio") {
        char field = 0;
        bool inclFloat = false;
        String rl = rest; rl.toLowerCase();
        if (rl.startsWith("float")) {
            field = 's'; inclFloat = true;
        } else if (rl.length() > 0) {
            char f = rl[0];
            if (f == 's' || f == 'd' || f == 'p' || f == 'f') field = f;
            if (rl.indexOf("float") >= 0) inclFloat = true;
        }
        usbSer3_sendRawGpio(out, field, inclFloat);
    } else if (head == "oled") {
        String rl = rest; rl.toLowerCase();
        if (rl.startsWith("stream")) {
            String s2 = rl.substring(6);
            if (s2.startsWith(":")) s2 = s2.substring(1);
            if (s2.startsWith("off")) {
                oledSer3Stream = false;
                out->print("{\"oled_stream\":\"off\"}\r\n");
            } else {
                int cc = s2.indexOf(':');
                String enc = (cc < 0) ? String("") : s2.substring(cc + 1);
                oledSer3Enc = usbSer3_encFromName(enc);
                oledSer3Target = &USBSer3;
                oledSer3Stream = true;
                out->printf("{\"oled_stream\":\"on\",\"enc\":\"%s\"}\r\n",
                            usbSer3_encName(oledSer3Enc));
            }
        } else {
            oled.dumpFrameBufferEncoded(out, usbSer3_encFromName(rl));
        }
    } else if (head == "leds") {
        usbSer3_sendLeds(out);
    } else if (head == "crossbar" || head == "xbar") {
        usbSer3_sendCrossbar(out);
    } else if (head == "fs") {
        runFilesystemWalk(out);
    } else if (head == "json") {
        String line = (rest.length() > 0) ? (String("J ") + rest) : String("J");
        Jerial.setCurrentResponseTarget(out);
        cmd_showJsonState('J', line);
        Jerial.clearCurrentResponseTarget();
    } else if (head == "yaml") {
        usbSer3_sendYAML(out);
    } else if (head == "nets") {
        usbSer3_sendNets(out);
    } else if (head == "adc") {
        usbSer3_sendADC(out);
    } else if (head == "all" || head == "status") {
        usbSer3_sendAllStatus(out);
    } else if (head == "slot") {
        Jerial.setCurrentResponseTarget(out);
        cmd_queryActiveSlot('Q', "Q");
        Jerial.clearCurrentResponseTarget();
    } else if (head == "history") {
        Jerial.setCurrentResponseTarget(out);
        cmd_historyList('(', "(");
        Jerial.clearCurrentResponseTarget();
    } else if (head == "histstatus") {
        Jerial.setCurrentResponseTarget(out);
        cmd_historyStatus(',', ",");
        Jerial.clearCurrentResponseTarget();
    } else if (head == "ver" || head == "version") {
        Jerial.setCurrentResponseTarget(out);
        cmd_showVersion('?', "?");
        Jerial.clearCurrentResponseTarget();
    } else if (head == "gpiojson") {
        usbSer3_sendGpioJson(out);
    } else {
        // Single-char passthrough: ":n" == bare 'n', ":J" == bare 'J', etc.
        // Special machine-query chars (A/V/G/N/K) route to `out` directly.
        // Registered SER3_ALLOWED commands execute through the normal gate.
        if (head.length() == 1) {
            char ch = head[0];
            switch (ch) {
                case 'A': usbSer3_sendAllStatus(out); return;
                case 'V': usbSer3_sendADC(out);       return;
                case 'G': usbSer3_sendGpioJson(out);  return;
                case 'N': usbSer3_sendNets(out);       return;
                case 'K': usbSer3_sendYAML(out);       return;
                default: break;
            }
            const Command* cmd = singleCharCommands.getCommand(ch);
            if (cmd && cmd->ser3Access == SER3_ALLOWED) {
                char cmdStr[2] = { ch, 0 };
                Jerial.setCurrentResponseTarget(out);
                // A backchannel command is complete as sent: its handler must
                // not wait on port 1's stream for arguments (getCommandArgs).
                bool wasLine = g_commandInputIsLine;
                g_commandInputIsLine = true;
                singleCharCommands.executeCommand(ch, String(cmdStr));
                g_commandInputIsLine = wasLine;
                Jerial.clearCurrentResponseTarget();
                return;
            }
        }
        out->print("{\"error\":\"unknown_verb\",\"verb\":");
        usbSer3_jsonStr(out, verb.c_str());
        out->print("}\r\n");
    }
}

// ----------------------------------------------------------------------------
// :bench - time each read-only verb (compute-only via NullStream byte counter)
// ----------------------------------------------------------------------------
class NullStream : public Stream {
public:
    size_t bytes = 0;
    int available() { return 0; }
    int read() { return -1; }
    int peek() { return -1; }
    void flush() { }
    size_t write(uint8_t) { bytes++; return 1; }
    size_t write(const uint8_t* /*b*/, size_t size) { bytes += size; return size; }
    int availableForWrite() { return 32767; }
    using Print::write;
};

static const char* usbSer3_benchVerbs[] = {
    "gpio:s", "gpio:d", "gpio:p", "gpio:f", "gpio", "leds", "crossbar",
    "adc", "nets", "slot", "ver", "json", "all", "yaml"
};

static void usbSer3_runBench(const String& which) {
    const int N = 16;
    USBSer3.print("bench_start\r\n");
    for (unsigned vi = 0; vi < sizeof(usbSer3_benchVerbs) / sizeof(usbSer3_benchVerbs[0]); vi++) {
        String verb = usbSer3_benchVerbs[vi];
        if (which.length() > 0 && which != verb) continue;

        NullStream ns;
        usbSer3_dispatchVerb(&ns, verb);   // warmup (prime XIP cache, etc.)

        uint32_t cmin = 0xFFFFFFFFu, cmax = 0;
        uint64_t csum = 0;
        size_t bytes = 0;
        for (int i = 0; i < N; i++) {
            ns.bytes = 0;
            uint32_t t = micros();
            usbSer3_dispatchVerb(&ns, verb);
            uint32_t dt = micros() - t;
            if (dt < cmin) cmin = dt;
            if (dt > cmax) cmax = dt;
            csum += dt;
            bytes = ns.bytes;
        }
        uint32_t cavg = (uint32_t)(csum / N);
        usbSer3_setVerbMinUs(verb, cavg > 0 ? cavg : 1);
        USBSer3.printf("bench{verb:%s,min_us:%lu,avg_us:%lu,max_us:%lu,bytes:%u}\r\n",
                       verb.c_str(), (unsigned long)cmin, (unsigned long)cavg,
                       (unsigned long)cmax, (unsigned)bytes);
        USBSer3.flush();
    }
    USBSer3.print("bench_end\r\n");
    USBSer3.flush();
}

// ----------------------------------------------------------------------------
// :every - scheduled capture
// ----------------------------------------------------------------------------
static uint32_t usbSer3_parseInterval(String s) {
    s.trim(); s.toLowerCase();
    uint32_t mult = 1000;            // default unit: ms
    if (s.endsWith("us")) { mult = 1;        s = s.substring(0, s.length() - 2); }
    else if (s.endsWith("ms")) { mult = 1000; s = s.substring(0, s.length() - 2); }
    else if (s.endsWith("s")) { mult = 1000000; s = s.substring(0, s.length() - 1); }
    double val = s.toFloat();
    if (val <= 0) val = 1;
    uint32_t us = (uint32_t)(val * (double)mult);
    return us < 1 ? 1 : us;
}

// A buffered capture owns core 0 for its whole duration and cannot poll USBSer3
// from inside the timed wait (tud_cdc_n_available lives in flash and takes the
// USB mutex, which is exactly the jitter this path exists to avoid), so cap the
// total wall time it can hold the board.
static const uint32_t USBSER3_CAPTURE_MAX_US = 10000000UL;   // 10 s
// Below this slot width a poll between samples is a measurable fraction of the
// interval, so only slow captures - the ones worth aborting - are abortable.
static const uint32_t USBSER3_CAPTURE_POLL_MIN_US = 1000;

// Precise buffered GPIO state capture: one gpio_get_all() per sample into a
// preallocated buffer at exact micros() spacing, no serial in the loop. RAM
// resident so it neither stalls on XIP misses nor fights Core 1 for the cache.
// Returns the number of samples actually written.
static uint32_t __not_in_flash_func(usbSer3_captureGpioLoop)(uint32_t* buf, uint32_t n, uint32_t intervalUs) {
    bool pollable = (intervalUs >= USBSER3_CAPTURE_POLL_MIN_US);
    uint32_t start = micros();
    uint32_t i = 0;
    for (; i < n; i++) {
        uint32_t due = start + i * intervalUs;
        while ((int32_t)(due - micros()) > 0) { /* busy wait for precise timing */ }
        buf[i] = gpio_get_all();
        if (pollable && USBSer3.available() > 0) { i++; break; }  // host sent a byte -> stop
    }
    return i;
}

// Returns true if it handled the capture; false (malloc failure) lets the
// caller fall back to streamed mode.
static bool usbSer3_captureGpioBuffered(uint32_t intervalUs, long count, const String& verb) {
    long n = count;
    if (n <= 0) n = 1000;            // continuous unsupported in buffered mode
    if (n > 8192) n = 8192;
    if (intervalUs > USBSER3_CAPTURE_MAX_US) intervalUs = USBSER3_CAPTURE_MAX_US;
    if ((uint32_t)n > USBSER3_CAPTURE_MAX_US / intervalUs) {
        n = (long)(USBSER3_CAPTURE_MAX_US / intervalUs);
        if (n < 1) n = 1;
    }
    uint32_t* buf = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!buf) { return false; }      // caller falls back to streamed

    USBSer3.printf("cap{verb:gpio:s,mode:buffered,eff_us:%lu,count:%ld}\r\n",
                   (unsigned long)intervalUs, n);
    USBSer3.flush();

    n = (long)usbSer3_captureGpioLoop(buf, (uint32_t)n, intervalUs);

    // Dump as 8 hex chars (32-bit GPIO0-31 word) per sample, chunked.
    USBSer3.print("caps{");
    char chunk[8 * 64];
    int cl = 0;
    for (long i = 0; i < n; i++) {
        uint32_t v = buf[i];
        for (int b = 7; b >= 0; b--) chunk[cl++] = USBSER3_HEX[(v >> (b * 4)) & 0xF];
        if (cl >= (int)sizeof(chunk)) { USBSer3.write((const uint8_t*)chunk, cl); cl = 0; }
    }
    if (cl) USBSer3.write((const uint8_t*)chunk, cl);
    USBSer3.print("}\r\n");
    free(buf);
    USBSer3.printf("cap_end{count:%ld}\r\n", n);
    USBSer3.flush();
    return true;
}

// Best-effort streamed capture: re-run the verb each slot, busy-wait to the
// next due time. Bails on flash ops or any inbound byte (host stop).
static void usbSer3_captureStreamed(uint32_t intervalUs, long count, const String& verb) {
    USBSer3.printf("cap{verb:%s,mode:streamed,eff_us:%lu,count:%ld}\r\n",
                   verb.c_str(), (unsigned long)intervalUs, count);
    uint32_t start = micros();
    long late = 0, i = 0;
    bool continuous = (count <= 0);
    while (continuous || i < count) {
        usbSer3_dispatchVerb(&USBSer3, verb);
        i++;
        if (USBSer3.available() > 0) break;   // host sent a byte -> stop
        if (core1FramesHeld()) break;         // flash op -> bail
        if (continuous && (micros() - start) > 60000000UL) break;  // safety cap
        uint32_t nextDue = start + (uint32_t)i * intervalUs;
        if ((int32_t)(micros() - nextDue) >= 0) {
            late++;                             // already overran this slot
        } else {
            while ((int32_t)(nextDue - micros()) > 0) {
                if (USBSer3.available() > 0) break;
            }
        }
    }
    USBSer3.printf("cap_end{taken_us:%lu,sent:%ld,late:%ld}\r\n",
                   (unsigned long)(micros() - start), i, late);
    USBSer3.flush();
}

// args = "<interval>:<count>:<verb...>"
static void usbSer3_runEvery(const String& args) {
    int p1 = args.indexOf(':');
    int p2 = (p1 < 0) ? -1 : args.indexOf(':', p1 + 1);
    if (p1 < 0 || p2 < 0) {
        USBSer3.print("{\"error\":\"every_usage\",\"fmt\":\":every:<interval>:<count>:<verb>\"}\r\n");
        USBSer3.flush();
        return;
    }
    uint32_t intervalUs = usbSer3_parseInterval(args.substring(0, p1));
    long count = args.substring(p1 + 1, p2).toInt();
    String verb = args.substring(p2 + 1);
    verb.trim();

    uint32_t minUs = usbSer3_verbMinUs(verb);
    if (intervalUs < minUs) intervalUs = minUs;

    // GPIO state sampling uses the precise buffered path; everything else
    // (including the multi-line "gpio" all-fields verb) is best-effort streamed.
    String vlow = verb; vlow.toLowerCase();
    if (vlow == "gpio:s") {
        if (usbSer3_captureGpioBuffered(intervalUs, count, verb)) return;
        // malloc failed: fall through to streamed as a backstop.
    }
    usbSer3_captureStreamed(intervalUs, count, verb);
}

// ----------------------------------------------------------------------------
// Single-char fast path + dispatch
// ----------------------------------------------------------------------------
static bool handleUSBSer3Special(char c) {
    bool handled = true;
    switch (c) {
        case 'A': usbSer3_sendAllStatus(&USBSer3); break;
        case 'V': usbSer3_sendADC(&USBSer3);       break;
        case 'G': usbSer3_sendGpioJson(&USBSer3);  break;
        case 'N': usbSer3_sendNets(&USBSer3);      break;
        case 'K': usbSer3_sendYAML(&USBSer3);      break;
        default:  handled = false;                  break;
    }
    if (handled) USBSer3.flush();
    return handled;
}

// Read a newline-terminated verb line from USBSer3 (after the ':' prefix).
// Echoes typed characters (and handles backspace) so a human can enter verbs
// interactively. The 2s idle timeout resets on each keystroke, so there's
// plenty of time to type; a machine sending a complete ":verb\n" is unaffected
// beyond seeing its own input echoed back ahead of the response.
static String usbSer3_readLine(unsigned int timeoutMs = 2000) {
    String s;
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (USBSer3.available() > 0) {
            char ch = (char)USBSer3.read();
            start = millis();   // reset idle timeout on activity
            if (ch == '\n' || ch == '\r') {
                USBSer3.print("\r\n");   // echo the newline
                return s;
            }
            if (ch == 0x08 || ch == 0x7F) {   // backspace / delete
                if (s.length() > 0) {
                    s.remove(s.length() - 1);
                    USBSer3.print("\b \b");   // erase the char on the terminal
                }
                continue;
            }
            if (ch >= 32 && ch < 127) {
                s += ch;
                USBSer3.write(ch);   // echo printable char
            }
        }
        delay(1);
    }
    return s;
}

// Re-run a stored inquiry (single char or ":verb") for the repeat command.
static void usbSer3_runInquiry(const String& inq) {
    if (inq.length() == 0) return;
    if (inq[0] == ':') {
        String line = inq.substring(1);
        String low = line; low.toLowerCase();
        if (low.startsWith("every:")) { usbSer3_runEvery(line.substring(6)); return; }
        if (low.startsWith("bench")) {
            usbSer3_runBench(line.length() > 6 ? line.substring(6) : String(""));
            return;
        }
        Jerial.setCurrentResponseTarget(&USBSer3);
        usbSer3_dispatchVerb(&USBSer3, line);
        Jerial.clearCurrentResponseTarget();
        USBSer3.flush();
        return;
    }
    char c = inq[0];
    if (handleUSBSer3Special(c)) return;
    Jerial.setCurrentResponseTarget(&USBSer3);
    char cmdStr[2] = { c, 0 };
    {
        // complete as sent: the handler must not block on port 1 for arguments
        bool wasLine = g_commandInputIsLine;
        g_commandInputIsLine = true;
        singleCharCommands.executeCommand(c, String(cmdStr));
        g_commandInputIsLine = wasLine;
    }
    Jerial.clearCurrentResponseTarget();
    USBSer3.flush();
}

void SingleCharCommands::serviceUSBSer3() {
    static String lastInquiry;

    while (USBSer3.available() > 0) {
        char c = (char)USBSer3.read();

        // Swallow ANSI/VT escape sequences (arrow keys, Home/End, function
        // keys, ...) as a unit so they don't get misparsed as separate
        // commands (e.g. an up-arrow is ESC '[' 'A', where '[' would hit the
        // blocked-command path and 'A' would fire the full-status query).
        // Form: ESC '[' ... <final 0x40-0x7E>, or ESC 'O' <final>. A lone ESC
        // is simply dropped.
        if (c == 0x1B) {
            unsigned long t = millis();
            while (USBSer3.available() == 0 && millis() - t < 20) { delay(1); }
            if (USBSer3.available() > 0) {
                char b = (char)USBSer3.read();
                if (b == '[' || b == 'O') {
                    t = millis();
                    while (millis() - t < 20) {
                        if (USBSer3.available() > 0) {
                            char f = (char)USBSer3.read();
                            t = millis();
                            if (f >= 0x40 && f <= 0x7E) break;   // CSI final byte
                        } else {
                            delay(1);
                        }
                    }
                }
                // ESC followed by a non-CSI byte: both already consumed/dropped.
            }
            continue;
        }

        if (c == '\r' || c == '\n') continue;   // stray line endings
        if (c < 32 || c >= 127) continue;

        // ':' begins a newline-terminated verb command. This is the ONLY path
        // that blocks waiting for more bytes; single-char commands dispatch
        // instantly below. A bare ':' that times out with no verb shows help.
        if (c == ':') {
            USBSer3.write(':');   // echo the prefix so humans see the full verb
            String line = usbSer3_readLine();
            line.trim();
            if (line.length() == 0) {
                Jerial.setCurrentResponseTarget(&USBSer3);
                usbSer3_sendHelp(&USBSer3);   // bare ':' -> comprehensive help
                Jerial.clearCurrentResponseTarget();
                USBSer3.flush();
                continue;
            }
            String low = line; low.toLowerCase();

            if (low == "repeat" || low == "r") {
                if (lastInquiry.length()) usbSer3_runInquiry(lastInquiry);
                continue;
            }
            if (low == "stop") {
                USBSer3.print("{\"stop\":1}\r\n");
                USBSer3.flush();
                continue;
            }
            if (low.startsWith("every:")) {
                lastInquiry = String(":") + line;
                usbSer3_runEvery(line.substring(6));
                continue;
            }
            if (low.startsWith("bench")) {
                lastInquiry = String(":") + line;
                usbSer3_runBench(line.length() > 6 ? line.substring(6) : String(""));
                continue;
            }

            lastInquiry = String(":") + line;
            Jerial.setCurrentResponseTarget(&USBSer3);
            usbSer3_dispatchVerb(&USBSer3, line);
            Jerial.clearCurrentResponseTarget();
            USBSer3.flush();
            continue;
        }

        // '.' repeats the last inquiry (fast resample).
        if (c == '.') {
            if (lastInquiry.length()) usbSer3_runInquiry(lastInquiry);
            continue;
        }

        // Single-char machine queries (A/V/G/N/K) stay instant (no line collect).
        if (handleUSBSer3Special(c)) {
            lastInquiry = String(c);
            continue;
        }

        // Registered single-char commands dispatch instantly with no waiting.
        // Multi-char modifiers ("c!", "Y2", ...) are not collected here; use the
        // ':' verbs for those (e.g. :crossbar, :yaml). This keeps every keystroke
        // a zero-latency fire-and-forget on the always-non-line-buffered port.
        Ser3Access access = getBackchannelAccess(c);
        if (access != SER3_ALLOWED) {
            const char* reason;
            switch (access) {
                case SER3_INTERACTIVE:    reason = "interactive";   break;
                case SER3_MODIFIES_STATE: reason = "status_only";   break;
                case SER3_IRRELEVANT:     reason = "irrelevant";    break;
                case SER3_NOT_A_COMMAND:  reason = "not_a_command"; break;
                default:                  reason = "blocked";       break;
            }
            char allowed[192];
            usbSer3_buildAllowedChars(allowed, sizeof(allowed));
            char cmdStr2[2] = { c, 0 };
            USBSer3.print("{\"error\":\"blocked\",\"cmd\":");
            usbSer3_jsonStr(&USBSer3, cmdStr2);
            USBSer3.printf(",\"reason\":\"%s\",\"ok\":", reason);
            usbSer3_jsonStr(&USBSer3, allowed);
            USBSer3.print(",\"hint\":\":cmds\"}\r\n");
            USBSer3.flush();
            continue;
        }

        char cmdStr[2] = { c, 0 };
        lastInquiry = String(c);
        Jerial.setCurrentResponseTarget(&USBSer3);
        {
            bool wasLine = g_commandInputIsLine;   // same rule as the ':char' path
            g_commandInputIsLine = true;
            executeCommand(c, String(cmdStr));
            g_commandInputIsLine = wasLine;
        }
        Jerial.clearCurrentResponseTarget();
        USBSer3.flush();
    }
}
