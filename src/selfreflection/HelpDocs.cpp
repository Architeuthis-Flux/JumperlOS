#include "HelpDocs.h"
#include "Graphics.h"
#include "configManager.h"
#include "SingleCharCommands.h"
#include <string.h>

#include "Jerial.h"

// Color definitions for help text formatting
const int HELP_TITLE_COLOR = 51;      // Cyan
const int HELP_COMMAND_COLOR = 221;   // Yellow
const int HELP_DESC_COLOR = 207;      // Magenta
const int HELP_USAGE_COLOR = 69;      // Blue
const int HELP_NOTE_COLOR = 202;      // Orange/Red
const int HELP_NORMAL_COLOR = 38;     // Green

bool isHelpRequest(const char* input) {
    if (!input) return false;
    
    // Check for general help commands
    if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) {
        return true;
    }
    
    // Check for category help (help <category>)
    if (strncmp(input, "help ", 5) == 0 && strlen(input) > 5) {
        return true;
    }
    
    // Check for command-specific help (command followed by ?)
    int len = strlen(input);
    if (len == 2 && input[1] == '?') {
        return true;
    }
    
    return false;
}

bool handleHelpRequest(const char* input) {
    if (!isHelpRequest(input)) {
        return false;
    }
    
    if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) {
        showGeneralHelp();
        return true;
    }
    
    // Check for "help <category>" format
    if (strncmp(input, "help ", 5) == 0) {
        const char* category = input + 5; // Skip "help "
        showCategoryHelp(category);
        return true;
    }
    
    if (strlen(input) == 2 && input[1] == '?') {
        showCommandHelp(input[0]);
        return true;
    }
    
    return false;
}

void showGeneralHelp() {
    changeTerminalColor(HELP_TITLE_COLOR, true);
    Jerial.println("\n╭───────────────────────────────────────────────────────────────────────────╮");
    Jerial.println("│                          JUMPERLESS HELP SYSTEM                           │");
    Jerial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    
    changeTerminalColor(HELP_DESC_COLOR, true);
    Jerial.println("Type any command followed by ? for detailed help (like 'f?' or 'n?')");
    Jerial.println("Type 'help <category>' for section-specific help\n");
    changeTerminalColor(HELP_NOTE_COLOR, true);
    Jerial.println("  This help system is partially AI generated, so it may contain bullshit");
    Jerial.println("  When I make absolutely sure everything is accurate, I'll remove this message\n\r");
    
    // // ASCII art probe
    // changeTerminalColor(HELP_COMMAND_COLOR, true);
    // Jerial.println("                    ╭─────────────╮");
    // Jerial.println("                    │  THE PROBE  │  ← Your magic wand!");
    // Jerial.println("                    ╰─────────────╯");
    // changeTerminalColor(HELP_DESC_COLOR, true);
    // Jerial.println("              ●──────────────────────────○ Touch & Click");
    // Jerial.println("           Connect               Remove\n");
    
    changeTerminalColor(HELP_TITLE_COLOR, true);
    Jerial.println(" HELP CATEGORIES - Type 'help <category>' for details:");
    Jerial.println();
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" basics");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("     - Essential commands you'll use every day");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" probe");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("      - How to use the probe for connecting/removing");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" voltage");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("    - Power, measurement, and analog signals");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" arduino");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("    - Arduino integration and UART connections");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" python");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("     - MicroPython REPL, scripts, and hardware control");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" apps");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("       - Built-in applications and utilities");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" display");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("    - OLED display and LED control");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" slots");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("      - Save and load different circuit configurations");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" scripts");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("    - Python script management and examples");
    
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.print(" debug");
    changeTerminalColor(HELP_DESC_COLOR, false);
    Jerial.println("      - Troubleshooting and technical internals");
    
            changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(" config");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("     - Configuration file and persistent settings");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(" advanced");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("   - Advanced commands and technical features");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(" glossary");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("   - Definitions of nets, nodes, bridges, and more");
    
    Jerial.println();
    changeTerminalColor(HELP_USAGE_COLOR, true);
    Jerial.println(" QUICK START:");
    Jerial.println("  1. Press probe Connect button (turns blue)");
    Jerial.println("  2. Touch two points to connect them");
    Jerial.println("  3. Type 'f' to load a full connection file");
    Jerial.println("  4. Type 'n' to see what's connected");
    Jerial.println("  5. Type 'p' to enter MicroPython REPL");
    
    changeTerminalColor(HELP_NORMAL_COLOR, true);
    Jerial.println();
}

void showCommandHelp(char command) {
    // Use registered command help when available (e.g. Y?, d?, etc.)
    const Command* cmd = singleCharCommands.getCommand(command);
    if (cmd != nullptr) {
        singleCharCommands.printCommandHelp(command);
        changeTerminalColor(HELP_NORMAL_COLOR, true);
        Jerial.println();
        return;
    }

    // Fallback: hardcoded verbose help for commands not in registry
    changeTerminalColor(HELP_TITLE_COLOR, true);
    Jerial.print("\nHelp for command: ");
    changeTerminalColor(HELP_COMMAND_COLOR, false);
    Jerial.println(command);
    Jerial.println();

    switch (command) {
        case 'f':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Load a full set of connections");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: f");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Then type connections like:");
            Jerial.println("  f 1-5 (connects breadboard holes 1 and 5)");
            Jerial.println("  f D2-A3 (connects Arduino D2 to A3)");
            Jerial.println("  f GND-30 (connects ground rail to hole 30)");
            Jerial.println("  f 1-5,7-12,D2-A3 (multiple connections at once)");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            // Jerial.println("This is the main way to wire up your circuit!");
            break;
            
        case '+':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Add connections to your current setup");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: +");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Then type new connections like: + 1-5, D2-A3");
            Jerial.println("This adds to existing connections without clearing them.");
            break;
            
        case '-':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Remove specific connections");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: -");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Then type connections to remove like: - 1-5, D2-A3");
            Jerial.println("Only removes the connections you specify.");
            break;
            
        case 'x':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Clear ALL connections - nuclear option!");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: x");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("This removes everything in the current netlist.");
            break;
            
        case 'n':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show current connections (netlist)");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: n");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows all active connections in a nice list.");
            // Jerial.println("Great for seeing what's currently wired up.");
            break;
            
        case '^':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Set DAC voltage output");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: ^3.3  (sets DAC 1 voltage to 3.3V)");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("DAC features:");
            Jerial.println("  - High precision 12-bit output");
            Jerial.println("  - Multiple DAC channels available");
            Jerial.println("  - Range: -8V to +8V");
            Jerial.println("  - Also available via Python: dac_set(0, 3.3)");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            // Jerial.println("Perfect for testing circuits with precise known voltages!");
            break;
            
        case 'v':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Read voltages and currents with high precision");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage:");
            Jerial.println("  v       - show all ADC readings");
            Jerial.println("  v0-v4   - show specific ADC (0-4)");
            Jerial.println("  vi      - show current sensor readings");
            Jerial.println("  vi1     - show current sensor 1");
            Jerial.println("  vl      - toggle live readings display");
            //Jerial.println("  vp      - read probe voltage");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Features:");
            Jerial.println("  - High resolution 12-bit ADC readings");
            Jerial.println("  - Real-time monitoring capabilities");
            Jerial.println("  - Python access: adc_get(0)");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            // Jerial.println("Perfect for precision circuit debugging and monitoring!");
            break;
            
        case 'A':
        case 'a':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Connect or disconnect Jumperless Routable UART to Arduino D0 and D1 pins");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: A to connect, a to disconnect");
            changeTerminalColor(HELP_DESC_COLOR, true);
            //Jerial.println("This lets you program and communicate with an Arduino.");
            Jerial.println("Add '?' to check connection status: A?");
            break;
            
        
            // changeTerminalColor(HELP_DESC_COLOR, true);
            // Jerial.println("Disconnect UART from Arduino");
            // changeTerminalColor(HELP_USAGE_COLOR, true);
            // Jerial.println("Usage: a");
            // changeTerminalColor(HELP_DESC_COLOR, true);
            // Jerial.println("Breaks the connection so you can use D0/D1 for other stuff.");
            // break;
            
        case 'r':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Reset Arduino or Jumperless");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage:");
            Jerial.println("  r   - reset both Arduino Reset Pins");
            Jerial.println("  rt  - reset top Arduino Reset Pin only");
            Jerial.println("  rb  - reset bottom Arduino Reset Pin only");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Sometimes you just need to turn it off and on again.");
            break;
            
        case 'p':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Start MicroPython REPL with full scripting support!");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: p");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("REPL features:");
            Jerial.println("  - Command history (up/down arrows)");
            Jerial.println("  - Multi-line input with smart indentation");
            Jerial.println("  - Script save/load functionality");
            Jerial.println("  - File management and eKilo editor integration");
            Jerial.println("\nHardware control (no prefix needed):");
            Jerial.println("  - connect(1, 5)       - Make connections");
            Jerial.println("  - disconnect(1, 5)    - Remove connections");
            Jerial.println("  - gpio_set(2, True)   - Digital I/O");
            Jerial.println("  - adc_get(0)          - Read voltages");
            Jerial.println("  - run_app('i2c')      - Run built-in apps");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("This is where the real magic happens - full Python control!");
            break;
            
        case '>':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Execute a single Python command");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: >");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Then type a Python command like:");
            Jerial.println("  connect(1, 5)");
            Jerial.println("  gpio_set(2, True)");
            Jerial.println("  run_app('i2c')");
            Jerial.println("  print(adc_get(0))");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Quick way to run commands without entering full REPL.");
            break;
            
        // case 'P':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Show all connectable nodes and Python capabilities");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: P");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Displays node reference including:");
        //     Jerial.println("  - All breadboard holes (1-60)");
        //     Jerial.println("  - Arduino pins (D0-D13, A0-A5)");
        //     Jerial.println("  - Power rails (GND, +5V, +3.3V)");
        //     Jerial.println("  - GPIO pins and special functions");
        //     Jerial.println("  - Available jumperless Python commands");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("Essential reference for Python scripting and connections!");
        //     break;
            
        case '.':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Connect and initialize the I2C OLED display");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: .");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Toggles the little screen on/off.");
            Jerial.println("Note: connecting GPIO to the OLED is not the same as actually initializing the I2C display, this will do both");
            break;
            
        case 'l':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("LED brightness control and test pattern menu");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: l");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Opens a menu to:");
            Jerial.println("  - Adjust LED brightness");
            Jerial.println("  - Run test patterns");
            Jerial.println("  - Check if LEDs are working");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("If your connections look dim, crank up the brightness");
            break;
            
        case '\'':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show the startup animation");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: '");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Replays that cool swirly animation you saw when it booted up.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Pure eye candy, but hey, we all need some joy in debugging.");
            break;
            
        case '<':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Cycle through saved configuration slots");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: <");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Jumperless has multiple slots to save different circuit configs.");
            Jerial.println("This cycles backwards through them (use 'o' to pick specific ones).");
            break;
            
        case 'o':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Load a specific configuration slot");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: o");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows a menu of saved configurations you can load.");
            Jerial.println("Each slot can hold a complete circuit setup.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Great for switching between different projects!");
            break;
            
        case 's':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show all saved slot files");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: s");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Lists all your saved configurations with their slot numbers.");
            Jerial.println("You can copy and paste this output to reload it later");
            break;
            
        case 'b':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show bridge array and routing paths - the technical guts");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage:");
            Jerial.println("  b   - show everything");
            Jerial.println("  b0  - hide duplicates"); 
            Jerial.println("  b2  - show extra details");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("This shows how Jumperless actually routes your connections");
            //Jerial.println("through the internal crossbar switches. Very technical!");
            break;
            
        case 'c':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show crossbar chip connection status");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: c");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows the state of all the internal switching chips.");
            //Jerial.println("Useful for debugging weird connection issues.");
            break;
            
        case 'd':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Debug flags menu - for troubleshooting");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: d");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Opens a menu to enable/disable various debug outputs:");
            Jerial.println("  - File parsing debug");
            Jerial.println("  - Connection manager debug");
            Jerial.println("  - LED debug info");
            Jerial.println("  - Jerial passthrough options");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Use d0-d9 to directly toggle specific debug categories.");
            break;
            
        case '?':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show firmware version");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: ?");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Tells you what version of Jumperless firmware you're running.");
            //Jerial.println("Helpful when reporting bugs or checking for updates.");
            break;
            
        case '@':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("I2C device scanner with flexible row targeting");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage:");
            Jerial.println("  @           - Interactive mode (prompts for SDA/SCL rows)");
            Jerial.println("  @5,10       - Scan with SDA on row 5, SCL on row 10");
            Jerial.println("  @5          - Auto-try 4 combinations around row 5:");
            Jerial.println("                SDA=5 SCL=6, SDA=6 SCL=5, SDA=5 SCL=4, SDA=4 SCL=5");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("I2C scanning features:");
            Jerial.println("  - Comprehensive device detection");
            Jerial.println("  - Flexible pin assignment");
            Jerial.println("  - Auto-discovery mode for unknown wiring");
            Jerial.println("  - Detailed device information with addresses");
            Jerial.println("  - Also available via Python: run_app('i2c')");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            //Jerial.println("Perfect for finding I2C devices when you're not sure of the wiring!");
            break;
            
        case '$':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Calibrate the DAC outputs");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: $");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Runs a calibration routine to make sure DAC voltages are accurate.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Do this occasionally, especially if voltages seem off.");
            break;
            
        case 'g':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Print current GPIO pin states");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: g");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows the current state of all GPIO pins.");
            //Jerial.println("Helpful for debugging digital circuits.");
            break;
            
        case '#':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Print text from menu to LED display");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: #");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Interactive mode - follow prompts to display text on the LEDs.");
            Jerial.println("Because sometimes you want your breadboard to say things.");
            break;
            
        case '~':
            // Use the comprehensive config help system
            printConfigHelp();
            break;
            
        case '`':
            // Use the comprehensive config help system
            printConfigHelp();
            break;
            
        //case 'E':
            // changeTerminalColor(HELP_DESC_COLOR, true);
            // Jerial.println("Toggle extra menu options display");
            // changeTerminalColor(HELP_USAGE_COLOR, true);
            // Jerial.println("Usage: E");
            // changeTerminalColor(HELP_DESC_COLOR, true);
            // Jerial.println("Hides/shows the extra commands in the main menu.");
            // Jerial.println("Makes the menu less cluttered if you don't need technical stuff.");
            // break;
            
        case 'e':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show/hide extra menu options");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: e");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Toggles display of extra commands in the menu.");
            break;
            
        case 'F':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Cycle through available OLED fonts");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: F");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Changes the font used on the OLED display.");
            //Jerial.println("Because comic sans is never the answer.");
            break;
            
        case '=':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Dump OLED frame buffer contents");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: =");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows the raw pixel data from the OLED display.");
            //Jerial.println("Very technical - mainly for display debugging.");
            break;
            
        case 'k':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Toggle OLED display in terminal");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: k");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows/hides a text version of the OLED display in your terminal.");
            //Jerial.println("Handy when you can't see the physical display clearly.");
            break;
            
        case 'R':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show board LEDs in terminal (dump LED states)");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: R");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Toggles a visual representation of the LED array in your terminal.");
            Jerial.println("This can make a mess of the terminal, but it's rad");
            break;
            
        case '_':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Print timing statistics (microseconds per byte)");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: _");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows performance statistics for data processing.");
            Jerial.println("Mainly useful for debugging Arduino Jerial passthrough");
            break;

        // case '&':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Load changed net colors from file");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: &");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Reloads the color configuration for current slot.");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("Advanced command for color management and debugging.");
        //     break;
            
        case 'U':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Enter USB Mass Storage mode");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: U");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Activates USB Mass Storage Device mode:");
            Jerial.println("  - Jumperless appears as a removable USB drive");
            Jerial.println("  - Edit files directly from your computer's file manager");
            Jerial.println("  - Access Python scripts, config files, and node files");
            Jerial.println("  - Jumperless becomes unresponsive during file editing");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("SAFETY: Always safely eject the drive before unplugging!");
            break;
            
        case 'u':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Exit USB Mass Storage mode");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: u");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Deactivates USB Mass Storage mode and returns to normal operation.");
            Jerial.println("Alternatively, safely eject the drive from your computer.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Always exit USB mode properly to prevent file corruption!");
            break;

        case 'm':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Show the main menu");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: m");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Displays the command menu again if it scrolled off screen.");
           // Jerial.println("Your lifeline when you forget what commands are available.");
            break;
            
        case '!':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Print the current node file contents");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: !");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows the raw node file data for the current slot.");
           // Jerial.println("Technical details about how connections are stored.");
            break;
            
        case 'G':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Reload config.txt changes (for USB Mass Storage mode)");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: G");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Reloads the configuration file without restarting the device.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Use this after editing config.txt via USB Mass Storage mode.");
            break;

        // case 'j':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Internal navigation command");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: j");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Internal command for menu navigation - typically not used directly.");
        //     break;

        case 'Z':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("USB Mass Storage Debug Control menu");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: Z");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Opens advanced USB debugging menu with options:");
            Jerial.println("  1. Toggle USB debug mode");
            Jerial.println("  2. Manual refresh from USB");
            Jerial.println("  3. Validate all slot files");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("For troubleshooting USB Mass Storage issues.");
            break;

        case '/':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("File Manager application");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: /");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Launches the built-in file manager for:");
            Jerial.println("  - Browsing files and directories");
            Jerial.println("  - Creating, editing, and deleting files");
            Jerial.println("  - Managing Python scripts and config files");
            Jerial.println("  - Viewing file contents and information");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Use file manager commands: h(help), v(view), e(edit), n(new), d(delete)");
            break;

        case 'C':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Toggle terminal colors");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: C");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Enables or disables colored terminal output.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Useful for terminal clients that don't support ANSI colors.");
            break;

        case 'E':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Toggle menu display");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: E");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Shows or hides the main menu after each command.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Different from 'e' - this controls menu visibility entirely.");
            break;

        // case 'i':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Initialize OLED display");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: i");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Manually initialize the OLED display if not already connected.");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("Usually not needed - the '.' command handles both connection and init.");
        //     break;

        // case '{':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Probe mode - explore connections");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: { (or press the probe button)");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Touch the probe to any point to see what it's connected to.");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("The probe is amazing for tracing circuits and debugging!");
        //     break;

        // case '}':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Probe mode - make connections");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: } (or long-press the probe button)");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Touch two points with the probe to connect them.");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("Super intuitive way to wire up your circuit!");
        //     break;

        case 'w':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Wave generator function (don't use, it's a janky pile of garbage)");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: w");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Generates various waveforms on DAC outputs.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("If wave generation fails, falls back to slot selection menu.");
            break;

        case 'y':
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Refresh connections and load files");
            changeTerminalColor(HELP_USAGE_COLOR, true);
            Jerial.println("Usage: y");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Refreshes all connections and loads any file changes.");
            Jerial.println("If USB Mass Storage is mounted, performs manual USB refresh.");
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.println("Use after editing files via USB Mass Storage mode.");
            break;

        // case 't':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Test MSC callbacks (disabled)");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: t");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Internal test function - currently disabled.");
        //     break;

        // case 'T':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Show detailed netlist information");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: T");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Displays comprehensive netlist details including:");
        //     Jerial.println("  - Complete connection information");
        //     Jerial.println("  - Bridge array data");
        //     Jerial.println("  - Path routing information");
        //     Jerial.println("  - Special nets and technical internals");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("More detailed than 'n' - shows technical implementation details.");
        //     break;

        // case ':':
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Machine mode activation");
        //     changeTerminalColor(HELP_USAGE_COLOR, true);
        //     Jerial.println("Usage: :: (type colon twice)");
        //     changeTerminalColor(HELP_DESC_COLOR, true);
        //     Jerial.println("Enters machine mode for automated control.");
        //     changeTerminalColor(HELP_NOTE_COLOR, true);
        //     Jerial.println("Advanced feature for programmatic control of Jumperless.");
        //     break;

        default:
            changeTerminalColor(HELP_NOTE_COLOR, true);
            Jerial.print("No specific help available for command '");
            Jerial.print(command);
            Jerial.println("'");
            changeTerminalColor(HELP_DESC_COLOR, true);
            Jerial.println("Try 'help' for a list of all available commands.");
            break;
    }
    
    changeTerminalColor(HELP_NORMAL_COLOR, true);
    Jerial.println();
}

void showCategoryHelp(const char* category) {
    changeTerminalColor(HELP_TITLE_COLOR, true);
    Jerial.print("\n╭───────────────────────────────────────────────────────────────────────────╮\n");
    
    // Center the category text in the header
    String headerText = String(category) + " HELP";
    int totalWidth = 74; // Available space between │ characters
    int textLen = headerText.length();
    int leftPadding = (totalWidth - textLen) / 2;
    int rightPadding = totalWidth - textLen - leftPadding;
    
    Jerial.print("│");
    for (int i = 0; i < leftPadding; i++) {
        Jerial.print(" ");
    }
    Jerial.print(headerText);
    for (int i = 0; i < rightPadding; i++) {
        Jerial.print(" ");
    }
    Jerial.print("│\n");
    Jerial.print("╰───────────────────────────────────────────────────────────────────────────╯\n");
    
    if (strcmp(category, "basics") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Essential commands you'll use every day:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("f  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Load a full set of connections (main wiring command)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("+  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Add connections to existing setup");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("-  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Remove specific connections");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("x  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Clear ALL connections (nuclear option!)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("n  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show current connections (netlist)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("m  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show main menu again");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Connection format examples:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  1-5          (breadboard holes)");
        Jerial.println("  D2-A3        (Arduino pins)");
        Jerial.println("  GND-30       (rail to hole)");
        Jerial.println("  1-5,7-12     (multiple connections)");
        
    } else if (strcmp(category, "probe") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
       // Jerial.println("Master the probe - your circuit's best friend!\n");
        
        // ASCII art probe diagram
        changeTerminalColor(HELP_COMMAND_COLOR, true);


char probe_art[] = R"""(
                     Select        Measure                     
                 .━━━━━━━━━.▁█▁▁▁▁▁.━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━.
           ▁▁.━━' Connect   ╭────────────────        ───────────────╮   \
  ───╼━━━━{       Remove    │Connect                      Remove    │    ┃
           ▔▔`━━. Measure   ╰────────────────        ───────────────╯   /
                 `━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━'
                                               
)""";
Jerial.println(probe_art);




        // Old ASCII art replaced with modern probe diagram above
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println(" CONNECT MODE (Blue):");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  1. Press Connect button (turns blue)");
        Jerial.println("  2. Touch first point - probe 'holds' it");
        Jerial.println("  3. Touch second point - creates connection");
        Jerial.println("  4. Repeat for more connections");
        Jerial.println("  5. Press Connect again to exit");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n REMOVE MODE (Red):");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  1. Press Remove button (turns red)");
        Jerial.println("  2. Touch any connected point");
        Jerial.println("  3. That connection gets removed");
        Jerial.println("  4. Press Remove again to exit");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n ENCODER CONNECTIONS:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Use the clickwheel to make connections without the probe:");
        Jerial.println("  1. Click > Connect > Add (or Remove)");
        Jerial.println("     OR just turn the clickwheel while in probe mode");
        Jerial.println("  2. Turn clickwheel to scroll through:");
        Jerial.println("     - Breadboard rows (1-60)");
        Jerial.println("     - Nano header pins (D0-A7)");
        Jerial.println("     - Rails (Top, Bottom, GND)");
        Jerial.println("     - DAC (0, 1)");
        Jerial.println("     - ADC (0-4, Probe)");
        Jerial.println("     - GPIO (1-8)");
        Jerial.println("     - UART (TX, RX)");
        Jerial.println("     - Current sense (I+, I-)");
        Jerial.println("  3. Click encoder button to select node");
        Jerial.println("  4. Hold encoder button to exit");
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("  Cursor auto-hides after 5 seconds of no movement");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n IMPORTANT - Switch Position:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Keep switch on 'SELECT' mode for best results");
        Jerial.println("  'MEASURE' mode is experimental and flaky");
        
        // changeTerminalColor(HELP_USAGE_COLOR, true);
        // Jerial.println("\n KEYBOARD SHORTCUTS:");
        // changeTerminalColor(HELP_DESC_COLOR, true);
        // Jerial.println("  {  - Probe explore mode (same as pressing probe button)");
        // Jerial.println("  }  - Probe connect mode (same as long-pressing probe button)");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Special Functions:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Tap the pads near the logo for:");
        Jerial.println("  - GPIO pins (programmable digital I/O)");
        Jerial.println("  - ADC inputs (read voltages)");
        Jerial.println("  - DAC outputs (generate voltages)");
        Jerial.println("  - UART (TX/RX for serial communication)");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Tap the building pads (in connect/remove mode) for:");
        Jerial.println("  - Current sense (I+/I-) - Shows marching ants animation");
     
        
    } else if (strcmp(category, "voltage") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Work with power, signals, and measurements:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("^  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Set DAC voltage output (^3.3 for 3.3V)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("v  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Read ADC voltages and currents");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("$  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Calibrate DACs (run this occasionally)");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Voltage Reading Options:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  v     - All ADC readings");
        Jerial.println("  v0-v4 - Specific ADC channel");
        Jerial.println("  vi    - Current sensor readings");
        Jerial.println("  vi1   - Current sensor 1 only");
        Jerial.println("  vl    - Toggle live readings");
        Jerial.println("  vp    - Read probe voltage");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n PWM Signal Generation (Python):");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  pwm(1, 1000, 0.5)           - 1kHz PWM on GPIO_1, 50% duty");
        Jerial.println("  pwm(2, 0.1, 0.25)           - 0.1Hz slow PWM on GPIO_2, 25% duty");
        Jerial.println("  pwm_set_frequency(1, 500)   - Change frequency to 500Hz");
        Jerial.println("  pwm_set_duty_cycle(1, 0.75) - Change duty cycle to 75%");
        Jerial.println("  pwm_stop(GPIO_1)            - Stop PWM on GPIO_1");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n PWM Frequency Ranges:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Hardware PWM: 10Hz to 62.5MHz (high precision)");
        Jerial.println("  Slow PWM: 0.001Hz to 10Hz (hardware timer based)");
        Jerial.println("  Automatic mode selection based on frequency");
        Jerial.println("  Ultra-slow PWM: 0.001Hz = 1000 second period!");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Animated Voltage Display:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Green (0V) → Red (5V) → Pink (8V+)");
        Jerial.println("  Blue/icy colors for negative voltages");
        Jerial.println("  Rails pulse toward top/bottom");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Current Sensing Marching Ants:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  When both I+ and I- are connected to nets:");
        Jerial.println("  Note: I+ and I- are shorted together through a 2 ohm sense resistor");
        Jerial.println("  1. Tap building pads (in connect/remove mode) to access I+/I-");
        Jerial.println("  2. Connect I+ and I- to different nets in your circuit");
        Jerial.println("  3. Animated 'marching ants' show current flow direction!");
        Jerial.println("  4. Virtual wire drawn between closest breadboard nodes");
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("  Visual feedback helps you see the current path in real-time");
        Jerial.println("  Automatically picks optimal breadboard nodes for display");
        
    } else if (strcmp(category, "arduino") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Arduino integration and UART connections:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("A  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Connect UART to Arduino D0/D1");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("a  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Disconnect UART from Arduino");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("r  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Reset Arduino (rt=top, rb=bottom)");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n UART Passthrough:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  When UART is connected:");
        Jerial.println("  - Second serial port appears");
        Jerial.println("  - Arduino IDE can flash directly");
        Jerial.println("  - Jerial Monitor works normally");
        Jerial.println("  - Auto-detects upload attempts");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Auto-flashing Magic:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Jumperless detects when Arduino IDE uploads");
        Jerial.println("  Automatically handles reset timing");
        Jerial.println("  Works with just one USB cable!");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Commands from Arduino:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Send commands using XML-style tags:");
        Jerial.println("  Serial1.println(\"<j>+ 1-2</j>\");");
        Jerial.println("  Serial1.println(\"<jumperless>x</jumperless>\");");
        Jerial.println("  Tags: <j>, <jumperless>, <jumperlessCommand>");
        Jerial.println("  Commands execute silently without echoing");
        
    } else if (strcmp(category, "python") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("MicroPython REPL, scripting, and hardware control:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("p  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Start full MicroPython REPL with history");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(">  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Execute single Python command");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("P  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Python command mode / show all nodes");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Hardware Control (no 'jumperless.' prefix needed):");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  connect(1, 5)              - Make connections");
        Jerial.println("  disconnect(1, 5)           - Remove connections");
        Jerial.println("  is_connected(1, 5)         - Check if connected");
        Jerial.println("  nodes_clear()              - Clear all connections");
        Jerial.println("  gpio_set(2, True)          - Digital output");
        Jerial.println("  gpio_get(2)                - Read digital input");
        Jerial.println("  adc_get(0)                 - Read voltage");
        Jerial.println("  dac_set(0, 3.3)            - Set voltage");
        Jerial.println("  pwm(1, 1000, 0.5)          - PWM output (1kHz, 50% duty)");
        Jerial.println("  run_app('i2c')             - Run built-in apps");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Slot & Net Management:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  nodes_save()               - Save connections to current slot");
        Jerial.println("  nodes_save(3)              - Save to specific slot");
        Jerial.println("  switch_slot(2)             - Switch to different slot");
        Jerial.println("  nodes_discard()            - Discard unsaved changes");
        Jerial.println("  nodes_has_changes()        - Check for unsaved changes");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Net Information API:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  get_net_name(0)            - Get name of net 0");
        Jerial.println("  set_net_name(0, 'VCC')     - Set custom net name");
        Jerial.println("  set_net_color(0, 'red')    - Set net color by name");
        Jerial.println("  get_net_info(0)            - Get full net info as dict");
        Jerial.println("  get_num_nets()             - Get number of active nets");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n REPL Features:");
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Wave generator:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  wavegen_set_output(DAC1)           - Output on DAC1 (aliases: TOP_RAIL, BOTTOM_RAIL, DAC0)");
        Jerial.println("  wavegen_set_freq(100.0)            - Frequency in Hz (0.0001 to 10000)");
        Jerial.println("  wavegen_set_wave(SINE)             - Waveform: SINE, TRIANGLE, RAMP, SQUARE (ARBITRARY later)");
        Jerial.println("  wavegen_set_amplitude(3.3)         - Amplitude in Vpp (0.0 to 16.0)");
        Jerial.println("  wavegen_set_offset(1.65)           - DC offset in Volts (-8.0 to +8.0)");
        Jerial.println("  wavegen_set_sweep(10, 1000, 2.0)   - Sweep start/end Hz over N seconds (config only)");
        Jerial.println("  wavegen_start(True)                - Start output (default True if no arg)");
        Jerial.println("  wavegen_stop()                     - Stop output");
        Jerial.println("  Note: You can change frequency/wave/amplitude/offset while running.");
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("  IMPORTANT: Wavegen runs on core2 and is fully blocking while active.");
        Jerial.println("  LEDs and routing updates will pause until wavegen_stop() is called.");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Command history (up/down arrows)");
        Jerial.println("  - Multi-line input support");
        Jerial.println("  - Smart indentation");
        Jerial.println("  - Script save/load functionality");
        Jerial.println("  - Tab completion for functions");
        Jerial.println("  - File manager and eKilo editor integration");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n REPL Commands:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  run     - Execute code buffer");
        Jerial.println("  clear   - Clear input buffer");
        Jerial.println("  quit    - Exit REPL");
        Jerial.println("  help    - Show REPL help");
        Jerial.println("  save    - Save current session as script");
        Jerial.println("  load    - Load and run a saved script");
        Jerial.println("  new     - Create new script with eKilo editor");
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  context ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Toggle connection context");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Connection Context Switching:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Type 'context' in the REPL to toggle between:");
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  - global context: ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("Connections persist after exiting Python");
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  - python context: ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("Connections restored on exit (saved to slots/slotPython.yaml)");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("\n  How it works:");
        Jerial.println("  - 'global' mode: Changes are permanent, like normal commands");
        Jerial.println("  - 'python' mode: State saved on entry, restored on exit");
        Jerial.println("  - Perfect for experimenting without affecting your main circuit");
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("  Context shown in REPL prompt - toggle anytime!");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Python Features:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Full MicroPython standard library");
        Jerial.println("  - Filesystem access for script storage");
        Jerial.println("  - Real-time hardware interaction");
        Jerial.println("  - Complete Jumperless module with all functions");
        
    } else if (strcmp(category, "apps") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Built-in applications and utilities:\n");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println(" Access Apps:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Navigate to Apps menu");
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  - Or run from Python: ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("run_app('appname')");
        
        changeTerminalColor(HELP_COMMAND_COLOR, true);
        Jerial.println("\n Available Apps:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  i2c        ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- I2C device scanner");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  scope      ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Simple oscilloscope functionality");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  calibrate  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- DAC/ADC calibration utility");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  custom     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Custom user application");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  python     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- MicroPython REPL (same as 'p' command)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  /          ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- File Manager (browse/edit files)");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n App Examples:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  Connect I2C device, run i2c app to find address");
        Jerial.println("  Use scope app to visualize signals on ADC pins");
        Jerial.println("  Run calibrate app if voltages seem off");
        Jerial.println("  Use / to browse and edit files directly");
        
    } else if (strcmp(category, "scripts") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Python script management and examples:\n");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println(" Script Management in REPL:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  save                    - Save current session");
        Jerial.println("  save 'filename'         - Save with specific name");
        Jerial.println("  load 'filename'         - Load and run script");
        Jerial.println("  list                    - Show saved scripts");
        Jerial.println("  delete 'filename'       - Remove script");
        
        changeTerminalColor(HELP_COMMAND_COLOR, true);
        Jerial.println("\n Examples Available:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  jumperless_demo.py        ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Basic hardware demo");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  external_python_control.py ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- External control examples");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  sync_demo.py             ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Synchronization examples");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("  custom_boolean_types.py  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Custom data types");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Script Features:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Automatic numbering for easy loading");
        Jerial.println("  - History persistence across reboots");
        Jerial.println("  - Full filesystem access");
        Jerial.println("  - Error handling and debugging");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Getting Started:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  1. Enter REPL with 'p'");
        Jerial.println("  2. Write some code");
        Jerial.println("  3. Type 'save' to store it");
        Jerial.println("  4. Type 'load 1' to run script #1");
        Jerial.println("  5. Check examples/ directory for inspiration");
        
    } else if (strcmp(category, "display") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("OLED display and LED control:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(".  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Connect/disconnect OLED display");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("l  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- LED brightness and test patterns");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("'  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show startup animation");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("F  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Cycle OLED fonts");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("k  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Toggle OLED display in terminal");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("R  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show board LEDs in terminal");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n OLED Setup:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  1. Get 128x32 SSD1306 OLED display");
        Jerial.println("  2. Friction fit into SBC board");
        Jerial.println("  3. Type '.' to connect data lines");
        Jerial.println("  4. Auto-disconnects if not found");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n OLED Config Options:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  `[top_oled] connect_on_boot = true;");
        Jerial.println("  `[top_oled] lock_connection = true;");
        Jerial.println("  `[top_oled] startup_message = Your Text;");
        Jerial.println("  `[top_oled] width = 128;");
        Jerial.println("  `[top_oled] height = 32; (or 64)");
        Jerial.println("  Type '~?' for full config help");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n LED Features:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Connections show as colored lines");
        Jerial.println("  - Voltages animate with color coding");
        Jerial.println("  - GPIO states show red/green");
        Jerial.println("  - Custom colors per slot saved");
        
    } else if (strcmp(category, "slots") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Save and load different circuit configurations:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("<  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Cycle through saved slots (backwards)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("o  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Load specific slot (shows menu)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("s  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show all saved slot files");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Slot System:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - 8 slots by default (0-7)");
        Jerial.println("  - Each slot saves connections and colors");
        Jerial.println("  - Switch projects instantly");
        Jerial.println("  - Slot files stored in /slots/slot[0-7].yaml");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Python Slot Functions:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  nodes_save()           - Save to current slot");
        Jerial.println("  nodes_save(3)          - Save to slot 3");
        Jerial.println("  switch_slot(2)         - Switch to slot 2");
        Jerial.println("  nodes_discard()        - Discard unsaved changes");
        Jerial.println("  nodes_has_changes()    - Check for unsaved changes");
        Jerial.println("  CURRENT_SLOT           - Get current slot number");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Pro Tips:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Use different slots for different projects");
        Jerial.println("  - Slot files are YAML (human-readable and editable)");
        Jerial.println("  - Colors and settings persist across reboots");
        
    } else if (strcmp(category, "debug") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Troubleshooting and technical internals:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("b  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Bridge array and routing paths");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("c  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Crossbar chip connection status");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("d  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Debug flags menu (d0-d9 for specific flags)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("?  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show firmware version");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("g  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Print GPIO states");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("T  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Detailed netlist information");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("Z  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- USB debug control menu");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("!  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Print raw node file contents");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n When Things Go Wrong:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  1. Check 'n' (netlist) for connections");
        Jerial.println("  2. Use 'b' to see routing internals");
        Jerial.println("  3. Try 'T' for detailed technical info");
        Jerial.println("  4. Enable debug flags with 'd' menu");
        Jerial.println("  5. Message me on Discord or wherever");
        
    } else if (strcmp(category, "config") == 0) {
        // Use the existing detailed config help system
        printConfigHelp();
        
    } else if (strcmp(category, "advanced") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Advanced commands and technical features:");
        Jerial.println("A lot of these are for my own debugging, but you can use them if you want.\n\r");
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("G  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Reload config.txt changes");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("C  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Toggle terminal colors");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("E  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Toggle menu display");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("i  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Initialize OLED display");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("w  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Wave generator function");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("y  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Refresh connections and load files");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("t  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Test MSC callbacks (disabled)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print(":: ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Machine mode activation");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("m  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Show main menu");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("j  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Internal navigation command");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("&  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Load changed net colors from file");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("_  ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Print timing statistics");
        
        changeTerminalColor(HELP_USAGE_COLOR, true);
        Jerial.println("\n Advanced Python Functions:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  pause_core2(True)           - Pause core2 processing");
        Jerial.println("  pause_core2(False)          - Resume core2 processing");
        Jerial.println("  send_raw('A', 1, 2, 1)      - Raw crossbar control (chip, x, y, set)");
        Jerial.println("  pwm(1, 0.001, 0.5)          - Ultra-slow PWM (0.001Hz)");
        Jerial.println("  context_toggle()            - Toggle global/python context");
        Jerial.println("  context_get()               - Get current context name");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Advanced Features:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Most users won't need these commands");
        Jerial.println("  - Some are for internal system operation");
        Jerial.println("  - Others are for power users and debugging");
        
    } else if (strcmp(category, "glossary") == 0) {
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("Definitions of key terms:\n");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("net      ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Group of all nodes connected together");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("node     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Any connectable point (breadboard, pins, GPIO, etc)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("row      ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Breadboard column (I know it's wrong) or nano header pin");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("rail     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Power rails (top, bottom, GND)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("bridge   ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Connection between exactly two nodes");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("path     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Crossbar routing needed for a bridge");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("slot     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- Saved configuration file (/slots/slot[0-7].yaml)");
        
        changeTerminalColor(HELP_COMMAND_COLOR, false);
        Jerial.print("chip     ");
        changeTerminalColor(HELP_DESC_COLOR, false);
        Jerial.println("- CH446Q crossbar switch (A-L)");
        
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.println("\n Key Concepts:");
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("  - Nodes are connected into nets");
        Jerial.println("  - Bridges connect pairs of exactly two nodes");
        Jerial.println("  - Paths route bridges through chips");
        Jerial.println("  - Slots save complete netlists");
        
    } else {
        changeTerminalColor(HELP_NOTE_COLOR, true);
        Jerial.print("Unknown category: ");
        Jerial.println(category);
        changeTerminalColor(HELP_DESC_COLOR, true);
        Jerial.println("\nAvailable categories:");
        Jerial.println("  basics, probe, voltage, arduino, python, apps");
        Jerial.println("  display, slots, scripts, debug, config, advanced, glossary");
    }
    
    changeTerminalColor(HELP_NORMAL_COLOR, true);
    Jerial.println();
}
