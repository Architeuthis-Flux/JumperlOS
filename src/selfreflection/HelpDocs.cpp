// ============================================================================
// HelpDocs - the onboard help.
//
// The text lives in ONE place (the kTopics tables below) and is rendered two
// ways:
//   - plain:   'help <topic>' and '<key>?' print it to the terminal, so it
//              works from scripts, the Arduino <j> tags, and dumb terminals.
//   - TUI:     a bare 'help' opens the same two-pane browser the ` config
//              editor uses (Tui.h): topics on the left, wrapped text on the
//              right, arrow keys to move, plus a Commands screen generated
//              from the SingleCharCommands registry and a row that jumps
//              straight into the config editor.
//
// Line format (first char is the role):
//   "#Title"        section header        "!Title"   note header
//   ">cmd|desc"     command row           "~art"     plain-only (too wide for the pane)
//   ""              blank                 "text"     body
// ============================================================================
#include "HelpDocs.h"
#include "Graphics.h"
#include "configManager.h"
#include "SingleCharCommands.h"
#include "Tui.h"
#include <string.h>

#include "Jerial.h"

void configTuiRun(void);

// Color definitions for help text formatting
const int HELP_TITLE_COLOR = 51;      // Cyan
const int HELP_COMMAND_COLOR = 221;   // Yellow
const int HELP_DESC_COLOR = 207;      // Magenta
const int HELP_USAGE_COLOR = 69;      // Blue
const int HELP_NOTE_COLOR = 202;      // Orange/Red
const int HELP_NORMAL_COLOR = 38;     // Green

// ---------------------------------------------------------------------------
// Topic text
// ---------------------------------------------------------------------------
static const char* const kBasics[] = {
    "Everything the probe does can also be typed:",
    "",
    ">m   |Show the menu ('e' shows more of it each press)",
    ">+   |Add connections        + 1-5,10-gnd",
    ">-   |Remove connections     - 1-5",
    ">f   |Replace ALL connections with the ones listed",
    ">x   |Clear all connections",
    ">n   |Show the net list (what's connected to what)",
    ">^ & |Undo / redo",
    ">< |Next slot ('<3' jumps to slot 3)",
    ">p   |MicroPython REPL",
    ">/   |File manager (or /path/to/script.py to run it)",
    ">U   |Show up on your computer as a USB drive ('u' to stop)",
    "#Connection format:",
    "  1-5            breadboard rows",
    "  D2-A3          Nano header pins",
    "  gnd-30         rails: gnd, top_rail, bottom_rail (any case)",
    "  dac1-20        dac0, dac1, adc0-adc4, gpio_1-gpio_8,",
    "                 uart_tx, uart_rx, isense_plus, isense_minus",
    "  1-5,7-12,D2-A3 several at once",
    "!Notes:",
    "  'p' and '/' take over the terminal, Ctrl+Q gets you back out.",
    "  Everything you do saves into the active slot automatically.",
    "  Type '<key>?' (like 'v?') for help on any single key.",
    nullptr
};

static const char* const kProbe[] = {
    "~",
    "~                     Select        Measure                     ",
    "~                 .━━━━━━━━━.▁█▁▁▁▁▁.━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━.",
    "~           ▁▁.━━' Connect   ╭────────────────        ───────────────╮   \\",
    "~  ───╼━━━━{       Remove    │Connect                      Remove    │    ┃",
    "~           ▔▔`━━. Measure   ╰────────────────        ───────────────╯   /",
    "~                 `━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━'",
    "~",
    "#THE SWITCH:",
    "  Select   - normal probing (leave it here)",
    "  Measure  - voltmeter. Logo goes purple, touch a row and its",
    "             voltage shows on the OLED and terminal. Uses a",
    "             temporary ADC connection that's never saved.",
    "#CONNECT (Blue):",
    "  1. Click Connect, the logo turns blue",
    "  2. Tap a row - you're now 'holding' it",
    "  3. Tap another row - they're connected, a wire shows up",
    "  Click Connect while holding to drop the row, click it while",
    "  holding nothing to go back to idle.",
    "#REMOVE (Red):",
    "  1. Click Remove, the logo turns red",
    "  2. Tap a row (or swipe a few) to disconnect it",
    "  Only removes that node and its direct connections, not the",
    "  whole net. Tapping a rail pad does clear everything on it.",
    "#IDLE (rainbow logo):",
    "  Tap a row to highlight its net and see its voltage/current",
    "  Turn the wheel to walk the highlight along the rows",
    "  Connect - start Connect mode already holding that row",
    "  Connect on a GPIO output net - toggles it high/low",
    "  Remove twice - Remove mode with that node picked",
    "  Click the wheel on a rail/DAC net - adjust its voltage",
    "  Click the wheel on a GPIO net - that pin's options",
    "#UNDO / REDO:",
    "  Double-tap Remove to undo, double-tap Connect to redo.",
    "  Click > History to scrub through every change with the wheel.",
    "#SPECIAL FUNCTION PADS (by the logo):",
    "  DAC, GPIO, ADC - tap one while probing, a chooser shows up",
    "  on the LEDs, pick one, then tap the row it goes to.",
    "  The two little guys are UART Tx (top) / Rx (bottom), the two",
    "  buildings are current sense I+ / I- (they don't always",
    "  register a tap - use the wheel for those). Reassign them in",
    "  [logo_pads].",
    "#CONNECTING WITH JUST THE WHEEL:",
    "  Click > Connect > Add (or Remove), or turn the wheel while in",
    "  Connect mode. The cursor scrolls through rows 1-60, Nano",
    "  pins, rails, DACs, ADCs, GPIO 1-8, UART Tx/Rx, and I+/I-",
    "  (counterclockwise past row 1). Click to pick it, hold to exit.",
    "  The cursor hides itself after 5 seconds without a turn.",
    "!If taps land on the wrong row:",
    "  Fingers on the pads or the probe sense risers will throw off",
    "  the readings. Run Calibration > Probe Pads from the wheel menu.",
    nullptr
};

static const char* const kVoltage[] = {
    "Rails, DACs, ADCs, and current sense:",
    "",
    ">:   |Set DAC 1 voltage (:3.3). DAC 0 powers the probe",
    ">v   |Read the ADCs (v, v0-v4, vi, vi1, vl)",
    ">$   |Calibrate the DACs (Calibration > DACs Calib on the wheel)",
    "#Power sources:",
    "  Top rail, bottom rail, DAC 0, DAC 1 - all -8 V to +8 V",
    "  Wheel: Rails > Top/Bottom/Both, or Output > Voltage > DAC 0/1",
    "  Or tap a rail + pad in idle mode and click the wheel to adjust",
    "  The - rail pads are always GND. Voltages save with the slot.",
    "#Reading voltages:",
    "  Tap a connected row in idle mode - voltage and current on the",
    "  OLED/terminal",
    "  Probe switch on Measure - voltmeter, nothing gets saved",
    "  ADC pad (or Show > Voltage) - an ADC stays on that row, +-8 V",
    "  ADC 4 is the 5 V tolerant one through a divider",
    "#Current sense (I+ / I-):",
    "  The board already measures current on every net in the",
    "  background (marching ants on any wire with current). For a",
    "  real reading, put I+ and I- on two different nets: in Connect",
    "  mode turn the wheel counterclockwise past row 1 to Current -,",
    "  one more to Current +, click, tap the row. Or Show > Current.",
    "!I+ and I- are shorted together through a 2 ohm shunt.",
    "  Measure in series, like a multimeter on the current setting.",
    "  [measurement] net_currents = 0;  turns the background ants off",
    "  [measurement] current_flow = electron;  march the other way",
    "#PWM (from Python):",
    "  pwm(1, 1000, 0.5)           1 kHz on GPIO 1, 50% duty",
    "  pwm_set_frequency(1, 500)   pwm_set_duty_cycle(1, 0.75)",
    "  pwm_stop(1)",
    "  0.01 Hz to 62.5 MHz. Below 10 Hz it's a timer-driven slow PWM.",
    "  Also on the wheel: GPIO > Set Pins.",
    nullptr
};

static const char* const kArduino[] = {
    "A Nano in the header:",
    "",
    ">A   |Connect the routable UART to D0/D1 ('A?' shows status)",
    ">a   |Disconnect it",
    ">r   |Reset the Arduino (rt = top reset pin, rb = bottom)",
    "#UART passthrough:",
    "  The Jumperless shows up as four serial ports. With the UART",
    "  connected, the second one is the Arduino - Serial Monitor",
    "  works, and the Arduino IDE can upload to it. The Jumperless",
    "  spots the upload and handles the reset timing. One USB cable.",
    "  Wheel: Output > UART, or the top/bottom guy pads while probing.",
    "#Commands FROM the Arduino:",
    "  Wrap a command in tags on its Serial and the Jumperless runs it:",
    "    Serial.println(\"<j>+ 1-2</j>\");",
    "    Serial.println(\"<p>print(adc_get(0))</p>\");   Python",
    "  Tags: <j>, <jumperless>, <jumperlessCommand>, <p>",
    "  They run quietly without echoing back to the Arduino.",
    "!Also:",
    "  Wokwi: 'W' pastes a diagram.json and wires it up (see 'W?').",
    "  The App at jumperless.org does terminal + Wokwi + flashing.",
    nullptr
};

static const char* const kPython[] = {
    "MicroPython with a 'jumperless' module already imported:",
    "",
    ">p   |The REPL (Ctrl+Q or 'quit' to leave)",
    ">>   |Run one line without entering the REPL:  > connect(1, 5)",
    ">/   |/path/to/script.py runs a file",
    "#Connections:",
    ">connect(1, 5)          |wire two nodes",
    ">disconnect(1, 5)       |unwire them",
    ">is_connected(1, 5)     |True / False",
    ">nodes_clear()          |same as 'x'",
    ">print_nets()           |same as 'n'",
    "#Read things:",
    ">adc_get(0)             |volts on ADC 0 (0-4)",
    ">gpio_get(1)            |HIGH / LOW / FLOATING on GPIO 1 (1-8)",
    ">get_current()          |mA through I+ / I-",
    ">probe_read()           |waits for a tap, returns the node",
    ">probe_button()         |CONNECT_BUTTON / REMOVE_BUTTON / none",
    "#Set things:",
    ">dac_set(1, 3.3)        |DAC 1 to 3.3 V (DAC 0 powers the probe)",
    ">dac_set(TOP_RAIL, 5)   |rails are DACs too",
    ">gpio_set(1, HIGH)      |drive GPIO 1",
    ">gpio_set_dir(1, INPUT) |or OUTPUT",
    ">pwm(1, 1000, 0.5)      |1 kHz, 50% on GPIO 1   pwm_stop(1)",
    "#OLED and printing:",
    ">print('hi')            |goes to the terminal",
    ">oled_print('hi')       |goes to the OLED    oled_clear()",
    ">run_app('I2C Scan')    |any app by its menu name",
    "#Constants (use them anywhere a node goes):",
    "  1-60                 breadboard rows (plain ints)",
    "  D0-D13  A0-A7        Nano header pins",
    "  TOP_RAIL  BOTTOM_RAIL  GND",
    "  DAC0  DAC1           ADC0-ADC4",
    "  GPIO_1-GPIO_8        UART_TX  UART_RX",
    "  ISENSE_PLUS  ISENSE_MINUS",
    "  HIGH  LOW  FLOATING  INPUT  OUTPUT",
    "  CONNECT_BUTTON  REMOVE_BUTTON  BUTTON_NONE",
    "  CURRENT_SLOT",
    "  Names work as strings too: connect('D2', 'GPIO_1')",
    "#Example:",
    "  dac_set(TOP_RAIL, 3.3)",
    "  connect(TOP_RAIL, 10)",
    "  connect(ADC0, 10)",
    "  print(adc_get(0))",
    "#More:",
    "  help()      lists everything in the module",
    "  Up/down history, tab completion, 'help scripts' for saving",
    nullptr
};

static const char* const kApps[] = {
    "Built-in apps:",
    "",
    "#Running them:",
    "  Click the wheel > Apps (or Calibration, Parts, GPIO), or from",
    "  Python:  run_app('I2C Scan')  - the name as it appears in the menu",
    "#Apps:",
    ">  I2C Scan       |Find I2C devices (also '@' from the terminal)",
    ">  Scan           |Measure the voltage on every node and print it",
    ">  uPython REPL   |Same as 'p'",
    ">  Custom App     |Yours, in Apps.cpp",
    ">  Bounce Startup |The startup animation",
    ">  Snake          |On a breadboard",
    ">  DMX Serial     |Drive DMX fixtures ('q' from the terminal)",
    ">  OLED Images    |Show images on the OLED",
    ">  JDI MIP display|JDI memory-in-pixel display driver",
    "#Calibration:",
    ">  Probe Pads     |Fix taps landing on the wrong row",
    ">  Switch Thresh  |Select/Measure switch detection",
    ">  DACs Calib     |Same as '$'",
    ">  Full Test      |Self test of everything",
    "  Probe Cable / Xbar Route / Tip Voltage / PSRAM Check",
    "#Parts (Click > Parts):",
    "  Place Part / Test Part / Remove Parts / Auto Scan",
    "  Tell it what chip is in the board and it wires and labels the pins.",
    "#GPIO (Click > GPIO):",
    "  Set Pins (direction, pulls, PWM) and BCD Counter",
    "#Guided projects:",
    "  /projects has wiring files that walk you through building a",
    "  circuit step by step, lighting each part's holes. Not a menu",
    "  row anymore - reach them with 'z' or from Files:",
    ">  z 555          |open (or start) the 555 blinker",
    ">  z 555 new      |start over from the wiring file",
    ">  z 555 noscript |just the wiring, skip its main.py",
    "  While it's running: wheel TURN browses steps, CLICK confirms,",
    "  HOLD (or q) quits. Probe CONNECT confirms, REMOVE un-commits.",
    "  Type 'z?' for the whole grammar.",
    nullptr
};

static const char* const kDisplay[] = {
    "OLED and LEDs:",
    "",
    ">.   |Connect / disconnect the OLED",
    ">O   |Cycle how it's wired: GPIO 7/8 -> RP6/RP7 -> internal I2C0",
    ">k   |Mirror the OLED into the terminal",
    ">t   |Type text onto the OLED (ESC to exit)",
    ">F   |Cycle fonts",
    ">=   |Dump the OLED frame buffer",
    ">l   |LED brightness and test patterns",
    ">R   |Show the board LEDs in the terminal (R! one-shot)",
    ">'   |Replay the startup animation",
    "#OLED setup:",
    "  128x32 or 128x64 SSD1306. Rev 7 boards: plugs straight into",
    "  the pin headers. Rev 5 (Crowd Supply / Mouser): goes on the",
    "  SBC adapter. Type '.' or Click > OLED > Connect. It copies",
    "  everything the breadboard LEDs say into text.",
    "#OLED config ('~' prints it, '`' edits it):",
    "  `[top_oled] connect_on_boot = 1;",
    "  `[top_oled] lock_connection = 1;    net edits can't disconnect it",
    "  `[top_oled] connection_type = 2;    0 GPIO 7/8, 1 RP6/7, 2 I2C0",
    "  `[top_oled] height = 64;",
    "  `[top_oled] startup_message = Hello;",
    "  `[top_oled] show_in_terminal = port_1;",
    "  Or Click > OLED for font, size, pins, startup message.",
    "#LEDs (Click > Display Options):",
    "  Colors (Rainbow / Shuffle), Jumpers (Wires / Lines)",
    "  Bright > Menu / Special / Rails / Wires, each 1-8",
    "  Wires are colored per net and saved with the slot.",
    "  Voltages: green (0 V) -> red (5 V) -> pink (8 V), blue negative.",
    "  GPIO shows high/low, ants march where current flows.",
    nullptr
};

static const char* const kSlots[] = {
    "8 saved circuits, 0-7:",
    "",
    "><   |Next slot (wraps 7 -> 0). '<3' jumps to slot 3",
    ">o   |Load a slot by number",
    ">Q   |Print which slot is active",
    ">y   |Reload the active slot from its file",
    "#What a slot holds:",
    "  Connections, rail and DAC voltages, GPIO setup, net colors,",
    "  history. Everything you do saves into the active slot",
    "  automatically. On boot it loads the last one you had active.",
    "#From the wheel:",
    "  Slots > Load       switch to one",
    "  Slots > Save to    copy the current circuit into another slot",
    "  Slots > Clear      empty one",
    "#From Python:",
    "  switch_slot(2)   nodes_save()   nodes_save(3)   CURRENT_SLOT",
    "  nodes_discard()   nodes_has_changes()",
    "!Files:",
    "  /slots/slot0.yaml ... slot7.yaml - plain YAML, edit them by",
    "  hand if you want ('/' or 'U'). To always boot into one slot:",
    "  `[slots] boot_mode = fixed_slot;  and  `[slots] boot_slot = 2;",
    nullptr
};

static const char* const kHistory[] = {
    "Every change to the circuit is recorded:",
    "",
    ">^   |Undo",
    ">&   |Redo",
    ">,   |Undo log status",
    ">(   |Print the whole undo log",
    "#From the probe:",
    "  Double-tap Remove to undo, double-tap Connect to redo.",
    "  The logo flashes yellow and the OLED says what got reverted.",
    "#From the wheel:",
    "  Click > History, then turn: clockwise steps back in time,",
    "  counterclockwise forward, two detents a step. The board",
    "  updates live. Click (or Connect) keeps where you stopped,",
    "  hold (or Remove) goes back.",
    "#From Python:",
    "  undo()   redo()   history_size()   history_position()",
    "  history_jump(n)",
    "!Notes:",
    "  History is saved with the slot, so it survives a power cycle.",
    "  Undo only ever touches the live circuit, not files.",
    nullptr
};

static const char* const kScripts[] = {
    "Python scripts on the board:",
    "",
    "#Running:",
    "  /python_scripts/examples/gpio_basics.py   from the main terminal",
    "  Click > Files, pick one                   from the wheel",
    "  exec(open('/python_scripts/examples/gpio_basics.py').read())",
    "                                            in the REPL",
    "  A script leaves its connections on the board; 'x' clears them.",
    "#Saving from the REPL:",
    "  save              save the last thing you ran (auto-numbered)",
    "  save blink        save it with a name",
    "  history           list saved scripts",
    "  load 3 / load blink    put one back in the editor, enter runs it",
    "  del blink         delete one",
    "  new / edit        open the eKilo editor",
    "#Examples in /python_scripts/examples:",
    "  dac_basics  adc_basics  gpio_basics  node_connections",
    "  uart_basics  machine_pin_basics  pin_irq_basics",
    "  pin_irq_freq_counter  voltage_monitor  oscilloscope",
    "  usb_audio_mic  stylophone  oled_stats_page  oled_layout_editor",
    "  fake_gpio  file_io_basics ...",
    "  README.md in there explains each one.",
    "!Editing:",
    "  'U' makes the board a USB drive so you can use your own editor.",
    "  JumperIDE (ide.jumperless.org) talks to it over serial.",
    nullptr
};

static const char* const kDebug[] = {
    "Seeing what the board is actually doing:",
    "",
    ">n   |Net list (what's connected to what)",
    ">b   |Bridge array and the paths routed for each one",
    ">c   |Crossbar view, compact ('c!' toggles live updating)",
    ">C   |Crossbar view, full color",
    ">J   |Whole state as JSON (J nets / J power / J gpio / J overlays)",
    ">Y   |Whole state as YAML (Y0 plain, Y1/Y2 colored)",
    ">D   |Status & diagnostics menu (arrow keys)",
    ">d   |Debug print flags menu",
    ">g   |GPIO pin states",
    ">H   |Live FakeGPIO / TDM voltage view",
    ">i!  |Net current scan report ('i?' RouteSafety check, 'i@' infra)",
    ">X   |Resource allocation",
    ">)   |PSRAM, file cache, undo log status",
    ">?   |Firmware version",
    ">Z   |USB mass storage debug menu",
    "#When something's wrong:",
    "  1. 'n' - is the connection even in the net list?",
    "  2. 'b' - did it get a path through the chips?",
    "  3. 'c' - are the crosspoints actually set?",
    "  4. Fingers on the probe pads / risers throw off the readings",
    "  5. 'y' reloads the slot, 'x' clears everything",
    "  6. Ask in the Discord, dumb questions help me write these docs",
    "!Crashes:",
    "  There's a crash log on the filesystem; 'D' and ')' show the",
    "  state that matters. Include the '?' version when reporting.",
    nullptr
};

static const char* const kAdvanced[] = {
    "Getting state in and out, plus the stuff that's mostly for me:",
    "",
    "#State import / export:",
    ">J / L |Dump / load state as JSON (a section: J power, L overlays)",
    ">Y / S |Dump / load state as YAML (paste, end with an empty line)",
    ">W     |Paste a Wokwi diagram.json (W [slot], W [file])",
    ">z     |Run a /projects wiring headless ('z?')",
    ">Q     |Print the active slot (for scripts driving the board)",
    "#Terminal:",
    ">G     |Toggle terminal colors",
    ">B     |Toggle line buffering (for raw terminals)",
    ">E     |Stop printing the menu after every command",
    ">e     |Show more of the menu (levels 0-3)",
    "#Hardware:",
    ">M     |USB audio mic from two ADCs (M01, Ms saves it, M? status)",
    ">P     |PSRAM test",
    ">*     |Raw crossbar switching speed test",
    ">w     |Wavegen test",
    ">|     |Clear the GPIO eratta workaround",
    ">q     |DMX serial app",
    ">_     |Micros per byte on the UART passthrough",
    ">%     |PSRAM / file cache / undo trace prints",
    ">#     |Print text from the menu system",
    ">T j   |LED test patterns (switch position, overlay)",
    "#From Python:",
    "  send_raw('A', 1, 2, 1)   set/clear one crosspoint (chip, x, y, set)",
    "  pause_core2(True)        hold core 1 (LEDs / scanning) still",
    "  history_snapshot()  history_jump(n)",
    "  place_part(...)  part_identify(...)  load_project('555')",
    "!Automation:",
    "  The 4th serial port is a read-only telemetry backchannel",
    "  (:help, :gpio, :leds ...). 'J' and 'Y' dump state for scripts.",
    "  There's an MCP server and an agent skill on jumperless.org",
    "  under Automation.",
    nullptr
};

static const char* const kGlossary[] = {
    "What I mean when I say:",
    "",
    ">node      |Anything you can connect: row, Nano pin, rail, DAC, ADC, GPIO",
    ">row       |One breadboard column of 5 holes (I know), numbered 1-60",
    ">net       |Everything connected together, shares one color on the LEDs",
    ">bridge    |A connection between exactly two nodes (what + and - take)",
    ">path      |The crosspoints a bridge got routed through",
    ">rail      |The + strips top and bottom (the - strips are always GND)",
    ">slot      |A saved circuit, 0-7, /slots/slotN.yaml",
    ">context   |Whatever file is the live circuit: a slot, a project run,",
    ">          |or slotPython",
    ">chip      |One of the 12 CH446Q crossbar switches, A-L",
    ">holding   |You tapped one row in Connect mode, the next tap connects",
    ">idle      |The probe isn't in a mode (rainbow logo)",
    ">click/hold|Short / long press of the wheel. Click = yes, hold = back",
    "#Routable nodes:",
    "  Rows 1-60, D0-D13, A0-A7, AREF, TOP_RAIL, BOTTOM_RAIL, GND,",
    "  DAC0, DAC1, ADC0-ADC4, GPIO_1-GPIO_8, UART_TX, UART_RX,",
    "  ISENSE_PLUS, ISENSE_MINUS, BUFFER_IN, BUFFER_OUT.",
    "  There is no routable fixed 3.3 V or 5 V - use a rail or a DAC.",
    nullptr
};

// Rows that aren't text topics.
static const char* const kCommandsBlurb[] = {
    "Every key the main terminal understands, with what it does.",
    "Same text as '<key>?'.",
    nullptr
};
static const char* const kConfigBlurb[] = {
    "Opens the interactive config editor (the same as typing ` alone).",
    "'~' prints the whole config, '`[section] key = value;' sets one.",
    nullptr
};
static const char* const kExitBlurb[] = { "Back to the menu (left arrow and q work too).", nullptr };

enum TopicAction : uint8_t { TOPIC_TEXT, TOPIC_COMMANDS, TOPIC_CONFIG, TOPIC_EXIT };

struct HelpTopic {
    const char* name;
    const char* blurb;               // one-liner for the general help list
    const char* const* lines;
    TopicAction action;
    const char* url;                 // the matching page on docs.jumperless.org
};

#define DOCS "https://docs.jumperless.org/"
static const HelpTopic kTopics[] = {
    { "basics",   "Connecting things from the terminal",                    kBasics,   TOPIC_TEXT, DOCS "01-getting-started/#from-the-terminal" },
    { "probe",    "The probe, the click wheel, and idle mode",              kProbe,    TOPIC_TEXT, DOCS "01-getting-started/#the-probe" },
    { "voltage",  "Rails, DACs, ADCs, current sense, PWM",                  kVoltage,  TOPIC_TEXT, DOCS "01-getting-started/#power-rails-and-dacs" },
    { "arduino",  "Nano header UART, flashing, commands from an Arduino",   kArduino,  TOPIC_TEXT, DOCS "05-arduino/" },
    { "python",   "MicroPython REPL and the jumperless module",             kPython,   TOPIC_TEXT, DOCS "09.5-micropythonAPIreference/" },
    { "apps",     "Built-in apps and guided projects",                      kApps,     TOPIC_TEXT, DOCS "08.5-examples/" },
    { "display",  "OLED and LED settings",                                  kDisplay,  TOPIC_TEXT, DOCS "04-oled/" },
    { "slots",    "Saving and switching between circuits",                  kSlots,    TOPIC_TEXT, DOCS "01-getting-started/#slots" },
    { "history",  "Undo / redo",                                            kHistory,  TOPIC_TEXT, DOCS "01-getting-started/#undo-redo" },
    { "scripts",  "Running and saving Python scripts",                      kScripts,  TOPIC_TEXT, DOCS "08-micropython/" },
    { "debug",    "Seeing what the board is actually doing",                kDebug,    TOPIC_TEXT, DOCS "07-debugging/" },
    { "advanced", "State import/export and the weird stuff",                kAdvanced, TOPIC_TEXT, DOCS "07.5-automation/" },
    { "glossary", "What I mean by net, node, bridge, slot...",              kGlossary, TOPIC_TEXT, DOCS "99-glossary/" },
    { "commands", "Every terminal key, from the command registry",          kCommandsBlurb, TOPIC_COMMANDS, DOCS "01-getting-started/#from-the-terminal" },
    { "config",   "The config editor and persistent settings",              kConfigBlurb,   TOPIC_CONFIG,   DOCS "06-config/" },
    { "exit",     "",                                                       kExitBlurb,     TOPIC_EXIT,     DOCS },
};
static const int kTopicCount = (int)(sizeof(kTopics) / sizeof(kTopics[0]));

// Examples for the commands whose one-line registry text can't hold them.
static const char* const kExtraConnect[] = {
    "Examples:",
    "  + 1-5              rows 1 and 5",
    "  + D2-A3            Nano header pins",
    "  + gnd-30,top_rail-12   rails (case doesn't matter)",
    "  + dac1-20,gpio_1-21,adc0-22   DACs, GPIO, ADCs",
    "  f 1-5,7-12         'f' replaces everything, '+' adds, '-' removes",
    "Anything that would obviously cause problems (TOP_RAIL-GND) is ignored.",
    nullptr
};
static const char* const kExtraV[] = {
    "Usage:",
    "  v      all 5 ADCs plus the probe tip",
    "  v0-v4  one ADC",
    "  vi     INA0 current, bus voltage, power (the I+/I- shunt)",
    "  vi1    INA1 (the DAC-side sensor)",
    "  vl     toggle live readings on the LEDs",
    nullptr
};
static const char* const kExtraColon[] = {
    "Example: :3.3 sets DAC 1 to 3.3 V (-8 to +8).",
    "DAC 0 powers the probe by default, so DAC 1 is the one to use.",
    "Rails and DACs can also be set from the wheel: Rails / Output > Voltage.",
    nullptr
};
static const char* const kExtraAt[] = {
    "Examples:",
    "  @        prompts for the SDA and SCL rows",
    "  @5,10    SDA on row 5, SCL on row 10",
    "  @5       tries SDA/SCL on 5&6, 6&5, 5&4, 4&5",
    nullptr
};
static const char* const kExtraLt[] = {
    "'<' goes to the next slot (wraps 7 -> 0), '<3' jumps to slot 3.",
    nullptr
};
static const char* const kExtraP[] = {
    "Inside the REPL, besides Python:",
    "  helpl          REPL help       history   list saved scripts",
    "  save [name]    save the last thing you ran",
    "  load <n|name>  put a saved script in the editor",
    "  del <name>     delete one",
    "  new / edit     open the eKilo editor   files   file manager",
    "  context        toggle global/python connection context",
    "  quit (or Ctrl+Q)   back to the main terminal",
    nullptr
};
static const char* const kExtraGt[] = {
    "Examples:",
    "  > connect(1, 5)",
    "  > print(adc_get(0))",
    "  > run_app('I2C Scan')",
    nullptr
};
static const char* const kExtraSlash[] = {
    "Examples:",
    "  /                                open the file manager",
    "  /python_scripts/examples/gpio_basics.py   run that script",
    "Ctrl+Q leaves the file manager. A script leaves its connections",
    "on the board; 'x' clears them.",
    nullptr
};
static const char* const kExtraD[] = {
    "The menu uses letters: x all off, z all on, f file parsing,",
    "n net manager, c chip connections, h alt paths, e LEDs,",
    "s probe current, u/U serial passthrough... Enter leaves it.",
    nullptr
};
static const char* const kExtraE[] = {
    "Each press shows one more level of the menu (0-3).",
    nullptr
};

static const char* const* extraFor(char c) {
    switch (c) {
        case 'f': case '+': case '-': return kExtraConnect;
        case 'v': return kExtraV;
        case ':': return kExtraColon;
        case '@': return kExtraAt;
        case '<': return kExtraLt;
        case 'p': return kExtraP;
        case '>': return kExtraGt;
        case '/': return kExtraSlash;
        case 'd': return kExtraD;
        case 'e': return kExtraE;
        default:  return nullptr;
    }
}

static const HelpTopic* findTopic(const char* name) {
    for (int i = 0; i < kTopicCount; i++)
        if (strcmp(kTopics[i].name, name) == 0) return &kTopics[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Plain rendering (help <topic>, <key>?)
// ---------------------------------------------------------------------------
static void printLines(const char* const* lines) {
    for (int i = 0; lines[i]; i++) {
        const char* s = lines[i];
        switch (s[0]) {
            case '#':
                changeTerminalColor(HELP_USAGE_COLOR, true);
                Jerial.print("\n "); Jerial.println(s + 1);
                break;
            case '!':
                changeTerminalColor(HELP_NOTE_COLOR, true);
                Jerial.print("\n "); Jerial.println(s + 1);
                break;
            case '>': {
                const char* bar = strchr(s + 1, '|');
                changeTerminalColor(HELP_COMMAND_COLOR, false);
                if (bar) { Jerial.write((const uint8_t*)(s + 1), bar - (s + 1)); }
                else Jerial.print(s + 1);
                changeTerminalColor(HELP_DESC_COLOR, false);
                if (bar && bar[1]) { Jerial.print("- "); Jerial.println(bar + 1); }
                else Jerial.println();
                break;
            }
            case '~':
                changeTerminalColor(HELP_COMMAND_COLOR, true);
                Jerial.println(s + 1);
                break;
            default:
                changeTerminalColor(HELP_DESC_COLOR, true);
                Jerial.println(s);
                break;
        }
    }
}

static void printHeader(const char* text) {
    changeTerminalColor(HELP_TITLE_COLOR, true);
    Jerial.print("\n╭───────────────────────────────────────────────────────────────────────────╮\n");
    int totalWidth = 74;
    int textLen = strlen(text);
    int leftPadding = (totalWidth - textLen) / 2;
    int rightPadding = totalWidth - textLen - leftPadding;
    Jerial.print("│");
    for (int i = 0; i < leftPadding; i++) Jerial.print(" ");
    Jerial.print(text);
    for (int i = 0; i < rightPadding; i++) Jerial.print(" ");
    Jerial.print("│\n");
    Jerial.print("╰───────────────────────────────────────────────────────────────────────────╯\n");
}

bool isHelpRequest(const char* input) {
    if (!input) return false;
    if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) return true;
    if (strncmp(input, "help ", 5) == 0 && strlen(input) > 5) return true;
    int len = strlen(input);
    if (len == 2 && input[1] == '?') return true;
    return false;
}

bool handleHelpRequest(const char* input) {
    if (!isHelpRequest(input)) return false;
    if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) { showGeneralHelp(); return true; }
    if (strncmp(input, "help ", 5) == 0) { showCategoryHelp(input + 5); return true; }
    if (strlen(input) == 2 && input[1] == '?') { showCommandHelp(input[0]); return true; }
    return false;
}

// The non-interactive index. 'help' alone opens the TUI; this is what
// 'help list' prints and what the TUI's unknown-topic fallback uses.
static void printTopicIndex() {
    printHeader("JUMPERLESS HELP");
    changeTerminalColor(HELP_DESC_COLOR, true);
    Jerial.println("Type 'help' alone for the interactive browser, 'help <topic>' to print one,");
    Jerial.println("or any key followed by ? (like 'f?' or 'v?'). Full docs: " DOCS "\n");
    for (int i = 0; i < kTopicCount; i++) {
        if (kTopics[i].action == TOPIC_EXIT) continue;
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(" ");
        Jerial.print(kTopics[i].name);
        for (int p = strlen(kTopics[i].name); p < 10; p++) Jerial.print(' ');
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.print("- ");
        Jerial.println(kTopics[i].blurb);
    }
    changeTerminalColor(HELP_USAGE_COLOR, true);
    Jerial.println("\n QUICK START:");
    Jerial.println("  1. Click Connect on the probe (logo turns blue), tap two rows");
    Jerial.println("  2. Or type '+ 1-5' to connect rows 1 and 5 from here");
    Jerial.println("  3. Type 'n' to see what's connected");
    Jerial.println("  4. Double-tap Remove (or type '^') to undo");
    Jerial.println("  5. Type 'p' for the MicroPython REPL, Ctrl+Q to get back out");
    changeTerminalColor(HELP_NORMAL_COLOR, true);
    Jerial.println();
}

static int helpTuiRun();

int showGeneralHelp() {
    return helpTuiRun();
}

void showCategoryHelp(const char* category) {
    if (strcmp(category, "list") == 0) { printTopicIndex(); return; }

    const HelpTopic* t = findTopic(category);
    if (!t || t->action == TOPIC_EXIT) {
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.print("\nUnknown topic: ");
        Jerial.println(category);
        printTopicIndex();
        return;
    }
    if (t->action == TOPIC_CONFIG) { printConfigHelp(); return; }

    char title[32];
    snprintf(title, sizeof(title), "%s HELP", t->name);
    printHeader(title);
    changeTerminalColor(HELP_NORMAL_COLOR, false);
    Jerial.print("docs: ");
    Jerial.println(t->url);
    Jerial.println();
    if (t->action == TOPIC_COMMANDS) {
        singleCharCommands.printAllHelp(-1);
    } else {
        printLines(t->lines);
    }
    changeTerminalColor(HELP_NORMAL_COLOR, true);
    Jerial.println();
}

void showCommandHelp(char command) {
    const Command* cmd = singleCharCommands.getCommand(command);
    if (cmd == nullptr) {
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.print("\nNo command '");
        Jerial.print(command);
        Jerial.println("'");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Type 'm' for the menu ('e' shows more of it) or 'help'.");
    } else {
        singleCharCommands.printCommandHelp(command);
        const char* const* extra = extraFor(command);
        if (extra) printLines(extra);
    }
    changeTerminalColor(HELP_NORMAL_COLOR, true);
    Jerial.println();
}

// ===========================================================================
// Interactive browser (bare 'help')
// ===========================================================================
namespace {

using namespace Tui;

struct Layout {
    int rows, cols;
    int listTop, listBottom;
    int leftC1, leftC2, rightC1, rightC2;
    int innerLeftW, innerRightW;
};

// leftCols: how wide the left pane is. Topic names are short, so the topic
// screen gives the text most of the terminal; the commands screen needs room
// for "f  load node file" and splits like the config editor does.
static Layout computeLayout(const Session& s, int leftCols) {
    Layout L;
    L.rows = s.rows();
    L.cols = s.cols();
    if (leftCols > L.cols - 30) leftCols = L.cols - 30;
    L.leftC1 = 1;
    L.leftC2 = leftCols;
    L.rightC1 = leftCols + 1;
    L.rightC2 = L.cols;
    L.listTop = 3;
    L.listBottom = L.rows - 2;
    L.innerLeftW = L.leftC2 - L.leftC1 - 3;
    L.innerRightW = L.rightC2 - L.rightC1 - 3;
    return L;
}

static void drawChrome(const Session& s, const Layout& L, const char* title, const Theme& th) {
    Stream* out = s.out();
    clearScreen(out);
    box(L.listTop - 1, L.leftC1, L.listBottom + 1, L.leftC2, BORDER_ROUNDED, roleColor(th.border, 0), out);
    box(L.listTop - 1, L.rightC1, L.listBottom + 1, L.rightC2, BORDER_ROUNDED, roleColor(th.border, 3), out);
    at(L.listTop - 1, L.rightC1 + 2, out);
    fg(roleColor(th.title, 1), out);
    bold(true, out);
    out->print(" ");
    printClipped(title, L.rightC2 - L.rightC1 - 6, out);
    out->print(" ");
    bold(false, out);
    at(1, 2, out);
    fg(roleColor(th.title, 2), out);
    bold(true, out);
    out->print("Jumperless Help");
    bold(false, out);
    resetColor(out);
}

static void drawFooter(const Session& s, const Layout& L, const char* hint, const Theme& th) {
    Stream* out = s.out();
    at(L.rows, 2, out);
    eraseToEnd(out);
    fg(th.hint, out);
    printClipped(hint, L.cols - 3, out);
    resetColor(out);
}

static void drawListRow(const Session& s, const Layout& L, const char* text, int row,
                        int rainbowIdx, bool selected, bool boldText, const Theme& th) {
    Stream* out = s.out();
    at(row, L.leftC1 + 1, out);
    for (int c = 0; c < L.leftC2 - L.leftC1 - 1; c++) out->print(' ');
    at(row, L.leftC1 + 2, out);
    if (selected) inverse(true, out);
    fg(selected ? th.selected : roleColor(th.category, rainbowIdx), out);
    if (boldText) bold(true, out);
    printClipped(text, L.innerLeftW, out);
    if (selected) {
        for (int c = strlen(text); c < L.innerLeftW; c++) out->print(' ');
        inverse(false, out);
    }
    if (boldText) bold(false, out);
    resetColor(out);
}

// ---------------------------------------------------------------------------
// Text pane: wrapped rendering of a line array, scrolled by SOURCE line.
// Each source line takes as many rows as its wrap needs; '~' lines are
// skipped (they're the wide ASCII art, plain-print only).
// ---------------------------------------------------------------------------
static int leadingSpaces(const char* s) {
    int n = 0;
    while (s[n] == ' ') n++;
    return n;
}

// Rows one source line occupies at width w (0 for skipped lines).
static int lineRows(const char* s, int w) {
    if (s[0] == '~') return 0;
    if (s[0] == '\0') return 1;
    const char* body = s;
    if (s[0] == '#' || s[0] == '!') body = s + 1;
    else if (s[0] == '>') { const char* bar = strchr(s + 1, '|'); body = bar ? bar + 1 : ""; }
    int indent = 0;
    if (s[0] == '>') {
        const char* bar = strchr(s + 1, '|');
        indent = bar ? (int)(bar - (s + 1)) + 1 : 0;
    } else {
        indent = leadingSpaces(body);
    }
    int ww = w - indent;
    if (ww < 8) ww = 8;
    char buf[8];
    int n = 0;
    while (wrapLine(body, ww, n, buf, sizeof(buf))) n++;
    return n < 1 ? 1 : n;
}

// Total rows from source line `from` to the end.
static int rowsFrom(const char* const* lines, int from, int w) {
    int total = 0;
    for (int i = from; lines[i]; i++) total += lineRows(lines[i], w);
    return total;
}

static int lineCount(const char* const* lines) {
    int n = 0;
    while (lines[n]) n++;
    return n;
}

// Largest scroll offset that still fills the pane (0 if it all fits).
static int maxScroll(const char* const* lines, int w, int paneRows) {
    int n = lineCount(lines);
    if (rowsFrom(lines, 0, w) <= paneRows) return 0;
    int off = n - 1;
    while (off > 0 && rowsFrom(lines, off, w) < paneRows) off--;
    return off;
}

// Draw one source line at row r; returns the next free row.
static int drawTextLine(Stream* out, const Layout& L, const char* s, int r, const Theme& th) {
    int w = L.innerRightW;
    int col = L.rightC1 + 2;
    if (s[0] == '~') return r;
    if (s[0] == '\0') return r + 1;

    const char* body = s;
    int indent = 0;
    int color = th.desc;
    bool isBold = false;
    char prefix[32] = "";

    if (s[0] == '#') { body = s + 1; color = roleColor(th.category, 5); isBold = true; }
    else if (s[0] == '!') { body = s + 1; color = 214; isBold = true; }
    else if (s[0] == '>') {
        const char* bar = strchr(s + 1, '|');
        int cmdLen = bar ? (int)(bar - (s + 1)) : (int)strlen(s + 1);
        if (cmdLen > (int)sizeof(prefix) - 1) cmdLen = sizeof(prefix) - 1;
        memcpy(prefix, s + 1, cmdLen);
        prefix[cmdLen] = '\0';
        body = bar ? bar + 1 : "";
        indent = cmdLen + 1;
    } else {
        indent = leadingSpaces(body);
    }

    int ww = w - indent;
    if (ww < 8) ww = 8;
    char lineBuf[128];
    int li = 0;
    bool any = false;
    for (; r <= L.listBottom && wrapLine(body, ww, li, lineBuf, sizeof(lineBuf)); li++, r++) {
        any = true;
        at(r, col, out);
        if (li == 0 && prefix[0]) {
            fg(roleColor(th.key, 3), out);
            printClipped(prefix, indent - 1, out);
            out->print(' ');
        } else {
            for (int i = 0; i < indent; i++) out->print(' ');
        }
        fg(color, out);
        if (isBold) bold(true, out);
        printClipped(lineBuf, ww, out);
        if (isBold) bold(false, out);
        pump();
    }
    if (!any && r <= L.listBottom) {
        // Command row with an empty description (e.g. a bare list of names).
        at(r, col, out);
        fg(roleColor(th.key, 3), out);
        printClipped(prefix, w, out);
        r++;
    }
    resetColor(out);
    return r;
}

static void clearPane(const Session& s, const Layout& L) {
    Stream* out = s.out();
    for (int row = L.listTop; row <= L.listBottom; row++) {
        at(row, L.rightC1 + 1, out);
        for (int c = 0; c < L.innerRightW + 2; c++) out->print(' ');
        pump();
    }
}

// Draw `lines` starting at source line `scroll`. An optional header line is
// drawn first with a blank row under it: bold for a title, dim for a link.
static void drawTextPane(const Session& s, const Layout& L, const char* header, bool headerIsLink,
                         const char* const* lines, int scroll, const Theme& th) {
    Stream* out = s.out();
    clearPane(s, L);
    int r = L.listTop;
    if (header) {
        at(r, L.rightC1 + 2, out);
        if (headerIsLink) {
            fg(th.hint, out);
            out->print("docs: ");
            fg(th.value, out);
            printClipped(header, L.innerRightW - 6, out);
        } else {
            fg(roleColor(th.category, 5), out);
            bold(true, out);
            printClipped(header, L.innerRightW, out);
            bold(false, out);
        }
        resetColor(out);
        r += 2;
    }
    for (int i = scroll; lines[i] && r <= L.listBottom; i++) {
        r = drawTextLine(out, L, lines[i], r, th);
    }
    out->flush();
}

// Scroll indicator in the right pane's bottom border.
static void drawScrollHint(const Session& s, const Layout& L, int scroll, int maxOff, const Theme& th) {
    Stream* out = s.out();
    const int W = 18;
    at(L.listBottom + 1, L.rightC2 - W - 2, out);
    fg(roleColor(th.border, 3), out);
    if (maxOff <= 0) {
        for (int i = 0; i < W; i++) out->print("─");
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), " %d/%d %s ", scroll, maxOff, scroll < maxOff ? "more v" : "end");
        int len = strlen(buf);
        fg(th.hint, out);
        printClipped(buf, W, out);
        fg(roleColor(th.border, 3), out);
        for (int i = len; i < W; i++) out->print("─");
    }
    resetColor(out);
}

// ---------------------------------------------------------------------------
// Commands screen: every registered key on the left, its help on the right.
// Enter on a command asks "run it?"; a second Enter answers yes and the
// trigger char is returned so the caller can execute it once the TUI is
// gone. Returns 0 when the user just backs out.
// ---------------------------------------------------------------------------
static int runCommandsScreen(Session& s, const Theme& th) {
    int n = singleCharCommands.getCommandCount();
    // Visible commands only: the raw control bytes the app sends (SO/SI/DC3)
    // are registered too and would render as blanks.
    const Command* list[128];
    int count = 0;
    for (int i = 0; i < n && count < 128; i++) {
        const Command* c = singleCharCommands.getCommandByIndex(i);
        if (c && c->trigger > ' ' && c->trigger < 127) list[count++] = c;
    }

    Layout L = computeLayout(s, (s.cols() * 44) / 100);
    ListView lv;
    lv.count = count + 1;   // row 0 = "< Back"
    lv.visible = L.listBottom - L.listTop + 1;
    lv.clamp();

    bool full = true;
    int lastSelected = -1;

    for (;;) {
        if (full) {
            drawChrome(s, L, "Commands", th);
            drawFooter(s, L, "up/down move   enter run   type a key to jump to it   left back   q exit", th);
        }
        if (full || lv.selected != lastSelected) {
            for (int i = lv.top; i < lv.top + lv.visible && i < lv.count; i++) {
                int row = L.listTop + (i - lv.top);
                if (i == 0) {
                    drawListRow(s, L, "< Back", row, 0, lv.selected == 0, true, th);
                } else {
                    const Command* c = list[i - 1];
                    char rowBuf[64];
                    snprintf(rowBuf, sizeof(rowBuf), "%c  %s", c->trigger, c->shortDesc ? c->shortDesc : "");
                    drawListRow(s, L, rowBuf, row, i, i == lv.selected, false, th);
                }
                pump();
            }
            if (lv.selected == 0) {
                static const char* const back[] = { "Return to the topic list.", nullptr };
                drawTextPane(s, L, nullptr, false, back, 0, th);
            } else {
                const Command* c = list[lv.selected - 1];
                // Assemble: helpText (may hold embedded newlines - split them),
                // then the extra examples if there are any.
                const char* linesBuf[64];
                static char textCopy[2048];   // z's help is ~1.5 KB
                int ln = 0;
                if (c->helpText && c->helpText[0]) {
                    strncpy(textCopy, c->helpText, sizeof(textCopy) - 1);
                    textCopy[sizeof(textCopy) - 1] = '\0';
                    char* p = textCopy;
                    while (p && *p && ln < 60) {
                        char* nl = strpbrk(p, "\r\n");
                        if (nl) { *nl = '\0'; }
                        linesBuf[ln++] = p;
                        if (!nl) break;
                        p = nl + 1;
                        while (*p == '\r' || *p == '\n') p++;
                    }
                }
                const char* const* extra = extraFor(c->trigger);
                if (extra) {
                    if (ln) linesBuf[ln++] = "";
                    for (int i = 0; extra[i] && ln < 63; i++) linesBuf[ln++] = extra[i];
                }
                linesBuf[ln] = nullptr;
                char head[80];
                snprintf(head, sizeof(head), "%c   %s", c->trigger, c->shortDesc ? c->shortDesc : "");
                drawTextPane(s, L, head, false, linesBuf, 0, th);
            }
            lastSelected = lv.selected;
            full = false;
            s.out()->flush();
        }

        Key k = readKey(s.in());
        if (k == KEY_NONE) { Tui::idle(); continue; }
        switch (k) {
            case KEY_UP:   { int t = lv.top; lv.move(-1); if (lv.top != t) full = true; break; }
            case KEY_DOWN: { int t = lv.top; lv.move(+1); if (lv.top != t) full = true; break; }
            case KEY_PGUP: lv.page(-1); full = true; break;
            case KEY_PGDN: lv.page(+1); full = true; break;
            case KEY_HOME: lv.home(); full = true; break;
            case KEY_END:  lv.end(); full = true; break;
            case KEY_LEFT:
            case KEY_ESC:  return 0;
            case KEY_ENTER:
            case KEY_RIGHT: {
                if (lv.selected == 0) return 0;
                const Command* c = list[lv.selected - 1];
                char prompt[96];
                snprintf(prompt, sizeof(prompt), "run '%c' (%s)?   enter = yes   any other key = no",
                         c->trigger, c->shortDesc ? c->shortDesc : "");
                drawFooter(s, L, prompt, th);
                s.out()->flush();
                Key k2;
                do { k2 = readKey(s.in()); if (k2 == KEY_NONE) Tui::idle(); } while (k2 == KEY_NONE);
                if (k2 == KEY_ENTER) return c->trigger;
                full = true;   // repaint the normal footer
                break;
            }
            case KEY_CHAR: {
                char ch = (char)lastChar();
                if (ch == 'q') return 0;
                if (ch == 'j') { int t = lv.top; lv.move(+1); if (lv.top != t) full = true; break; }
                if (ch == 'k') { int t = lv.top; lv.move(-1); if (lv.top != t) full = true; break; }
                // Type a key to jump to it.
                for (int i = 0; i < count; i++) {
                    if (list[i]->trigger == ch) {
                        int t = lv.top;
                        lv.selected = i + 1;
                        lv.clamp();
                        if (lv.top != t) full = true;
                        break;
                    }
                }
                break;
            }
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Topic screen: topics on the left, the text on the right. Enter/right
// focuses the text so up/down scroll it; left comes back to the list.
// ---------------------------------------------------------------------------
} // namespace

static int helpTuiRun() {
    Tui::Session s;
    s.begin();
    Tui::Theme th;

    Tui::ListView lv;
    lv.count = kTopicCount;

    bool full = true;
    bool reading = false;   // right pane has focus
    int scroll = 0;
    int lastSelected = -1;
    int lastScroll = -1;
    bool lastReading = false;

    for (;;) {
        Layout L = computeLayout(s, 14);
        lv.visible = L.listBottom - L.listTop + 1;
        lv.clamp();
        const HelpTopic& t = kTopics[lv.selected];
        int paneRows = L.listBottom - L.listTop + 1 - 2;   // -2: the docs link + blank row
        int maxOff = (t.action == TOPIC_TEXT) ? maxScroll(t.lines, L.innerRightW, paneRows) : 0;
        if (scroll > maxOff) scroll = maxOff;

        if (full) {
            drawChrome(s, L, t.name, th);
        }
        if (full || lv.selected != lastSelected || reading != lastReading) {
            drawFooter(s, L, reading
                ? "up/down scroll   pgup/pgdn page   left back to topics   q exit"
                : (maxOff > 0 ? "up/down move   enter read/scroll   left/q exit"
                              : "up/down move   enter open   left/q exit"), th);
            for (int i = lv.top; i < lv.top + lv.visible && i < lv.count; i++) {
                bool isAction = kTopics[i].action != TOPIC_TEXT;
                drawListRow(s, L, kTopics[i].name, L.listTop + (i - lv.top), i,
                            i == lv.selected && !reading, isAction, th);
                pump();
            }
        }
        if (full || lv.selected != lastSelected || scroll != lastScroll) {
            if (lv.selected != lastSelected) {
                // Retitle the right pane for the new topic.
                Tui::at(L.listTop - 1, L.rightC1 + 1, s.out());
                Tui::fg(Tui::roleColor(th.border, 3), s.out());
                for (int c = L.rightC1 + 1; c < L.rightC2; c++) s.out()->print("─");
                Tui::at(L.listTop - 1, L.rightC1 + 2, s.out());
                Tui::fg(Tui::roleColor(th.title, 1), s.out());
                Tui::bold(true, s.out());
                s.out()->print(" "); s.out()->print(t.name); s.out()->print(" ");
                Tui::bold(false, s.out());
                Tui::resetColor(s.out());
            }
            drawTextPane(s, L, t.url, true, t.lines, scroll, th);
            drawScrollHint(s, L, scroll, maxOff, th);
            lastSelected = lv.selected;
            lastScroll = scroll;
            lastReading = reading;
            full = false;
            s.out()->flush();
        }
        lastReading = reading;

        Tui::Key k = Tui::readKey(s.in());
        if (k == Tui::KEY_NONE) { Tui::idle(); continue; }

        if (reading) {
            switch (k) {
                case Tui::KEY_UP:   if (scroll > 0) scroll--; break;
                case Tui::KEY_DOWN: if (scroll < maxOff) scroll++; break;
                case Tui::KEY_PGUP: scroll -= paneRows - 2; if (scroll < 0) scroll = 0; break;
                case Tui::KEY_PGDN: scroll += paneRows - 2; if (scroll > maxOff) scroll = maxOff; break;
                case Tui::KEY_HOME: scroll = 0; break;
                case Tui::KEY_END:  scroll = maxOff; break;
                case Tui::KEY_LEFT:
                case Tui::KEY_ESC:
                case Tui::KEY_ENTER: reading = false; break;
                case Tui::KEY_CHAR:
                    if (Tui::lastChar() == 'q') { s.end(); return 0; }
                    if (Tui::lastChar() == 'j' && scroll < maxOff) scroll++;
                    if (Tui::lastChar() == 'k' && scroll > 0) scroll--;
                    break;
                default: break;
            }
            continue;
        }

        switch (k) {
            case Tui::KEY_UP:   { int tp = lv.top; lv.move(-1); scroll = 0; if (lv.top != tp) full = true; break; }
            case Tui::KEY_DOWN: { int tp = lv.top; lv.move(+1); scroll = 0; if (lv.top != tp) full = true; break; }
            case Tui::KEY_HOME: lv.home(); scroll = 0; full = true; break;
            case Tui::KEY_END:  lv.end(); scroll = 0; full = true; break;
            case Tui::KEY_PGDN: if (maxOff > 0) { reading = true; scroll += paneRows - 2; if (scroll > maxOff) scroll = maxOff; } break;
            case Tui::KEY_ENTER:
            case Tui::KEY_RIGHT:
                switch (t.action) {
                    case TOPIC_TEXT:
                        if (maxOff > 0) reading = true;
                        break;
                    case TOPIC_COMMANDS: {
                        int run = runCommandsScreen(s, th);
                        if (run) { s.end(); return run; }
                        full = true; lastSelected = -1;
                        break;
                    }
                    case TOPIC_CONFIG:
                        // The config editor runs its own session; hand the
                        // terminal over and take it back after.
                        s.end();
                        configTuiRun();
                        s.out()->print("\x1b[2J\x1b[H");
                        Tui::drainInput(s.in());
                        s.begin();
                        full = true; lastSelected = -1;
                        break;
                    case TOPIC_EXIT:
                        s.end();
                        return 0;
                }
                break;
            case Tui::KEY_CHAR:
                if (Tui::lastChar() == 'q') { s.end(); return 0; }
                if (Tui::lastChar() == 'j') { int tp = lv.top; lv.move(+1); scroll = 0; if (lv.top != tp) full = true; }
                if (Tui::lastChar() == 'k') { int tp = lv.top; lv.move(-1); scroll = 0; if (lv.top != tp) full = true; }
                break;
            case Tui::KEY_LEFT:
            case Tui::KEY_ESC:
                s.end();
                return 0;
            default:
                break;
        }
    }
}
