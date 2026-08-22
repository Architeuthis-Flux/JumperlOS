/**
 * @file SingleCharCommands.cpp
 * @brief Implementation of single-character command system
 */

#include "SingleCharCommands.h"
#include "CoreMailbox.h" // core1req (path-send requests to core 1; T2.2b)
#include "Apps.h"
#include "ArduinoStuff.h"
#include "AsyncPassthrough.h"
#include "CH446Q.h"
#include "Commands.h"
#include "Debugs.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"
#include "GraphicOverlays.h"
#include "Graphics.h"
#include "HelpDocs.h"
#include "Highlighting.h"
#include "Jerial.h" // TermControl is now part of Jerial
#include "JumperlOS.h"
#include "KickGap.h"   // watchdog measure-only stage: X prints the kick gaps, X! resets them
#include "XbarLatency.h" // tap->crossbar->LEDs latency probe: X prints it, X! resets it
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MCP4728.h"
#include "MatrixState.h"
#include "Menus.h"
#include "NetManager.h"
#include "NetVoltageScan.h"
#include "NetsToChipConnections.h"
#include "InfraPaths.h"
#include "RouteSafety.h"
#include "Peripherals.h"
#include "USBAudio.h"
#include "CrashLog.h"
#include "FlashPark.h"
#include "IrqSlots.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "ProjectsApp.h" // z: guided-project runner (headless/HIL entry)
#include "GuidedFlow.h"  // z band: parsePartValue / guideResistorBand report
#include "WaveGen.h"    // X: the wavegen stream line (T3.3)
#include "AdcRing.h"    // X: the ADC ring line (T2.1)
#include "Python_Proper.h"
#include "RotaryEncoder.h"
#include "States.h"
#include "FakeGpio.h"
#include "USBfs.h"
#include "WokwiParser.h"
#include "configManager.h"
#include "externVars.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "oled.h"
#include "JsonState.h"
#include "Debugs.h"
#include "Undo.h"
#include <algorithm>

// Global instance
SingleCharCommands singleCharCommands;

// Global state
volatile bool inMainMenu = false;

// External variables
extern int showExtraMenu;
extern int netSlot;
extern volatile int slotChanged;
extern int dontShowMenu;
extern int firstLoop;
extern char connectFromArduino;
extern String currentCommandLine;
extern TermControl termJerial;
extern int termInInteractiveMode;
extern const int highSaturationBrightColorsCount;
extern const int highSaturationSpectrumColorsCount;
extern const int highSaturationSpectrumColors[];
extern const int highSaturationBrightColors[];

// Forward declarations for command handlers
CommandResult cmd_showCrossbarFull( char c, const String& line );
CommandResult cmd_fakeGpioDebug( char c, const String& line );
CommandResult cmd_netCurrents( char c, const String& line );

// Forward declaration for the shared arg-reader (defined below).
// Needed so handlers earlier in the file (e.g. cmd_cycleSlots) can read
// trailing characters from Jerial in char-by-char mode.
static String getCommandArgs( const String& line, unsigned int timeoutMs );

// ============================================================================
// SingleCharCommands Class Implementation
// ============================================================================

SingleCharCommands::SingleCharCommands( ) {
    commandCount = 0;
    initializeCommands( );
}

ServiceStatus SingleCharCommands::service( ) {
    // This service doesn't need periodic servicing
    // Commands are executed synchronously when input is received
    return ServiceStatus::IDLE;
}

bool SingleCharCommands::registerCommand( const Command& cmd ) {
    if ( commandCount >= MAX_COMMANDS ) {
        return false;
    }

    // Check if command already exists
    int existingIndex = findCommandIndex( cmd.trigger );
    if ( existingIndex >= 0 ) {
        // Update existing command
        commands[ existingIndex ] = cmd;
        return true;
    }

    // Add new command
    commands[ commandCount++ ] = cmd;
    sortCommands( );
    return true;
}

bool SingleCharCommands::registerCommand( char trigger, const char* shortDesc,
                                          const char* helpText, CommandCallback callback,
                                          MenuLevel level, CommandCategory category,
                                          bool showInMenu, Ser3Access access ) {
    Command cmd( trigger, shortDesc, helpText, callback, level, category, showInMenu, access );
    return registerCommand( cmd );
}

Ser3Access SingleCharCommands::getBackchannelAccess( char trigger ) const {
    const Command* cmd = getCommand( trigger );
    if ( cmd == nullptr ) return SER3_NOT_A_COMMAND;
    return cmd->ser3Access;
}

bool SingleCharCommands::unregisterCommand( char trigger ) {
    int index = findCommandIndex( trigger );
    if ( index < 0 ) {
        return CMD_DONT_SHOW_MENU;
    }

    // Shift remaining commands
    for ( int i = index; i < commandCount - 1; i++ ) {
        commands[ i ] = commands[ i + 1 ];
    }
    commandCount--;
    return CMD_SHOW_MENU;
}

CommandResult SingleCharCommands::executeCommand( char cmdChar, const String& commandLine ) {
    const Command* cmd = getCommand( cmdChar );
    if ( cmd == nullptr || cmd->callback == nullptr ) {
        return CMD_SHOW_MENU; // Command not found, show menu
    }

    // Execute the callback
    return cmd->callback( cmdChar, commandLine );
}

void SingleCharCommands::printMenu( int extraMenuLevel ) {
    if ( Jerial.available( ) >
         20 ) { // this is so if you dump a lot of data into the Jerial buffer, it
                // will consume it and not keep looping
        while ( Jerial.available( ) > 0 ) {
            char c = Jerial.read( );
            // Jerial.print(c);
            // Jerial.flush();
        }
    }

    // (The lastProbePowerDAC change detector is gone: probe-power source
    // moves are handled by InfraPaths' rebuild-head evaluation + nudges.)

    // Jerial.print("clearing highlighting");
    // Jerial.flush();

    crashlogReportOnce( Jerial ); // once per boot, if the last reset was a fault
    clearHighlighting( );

    // Jerial.print("clearHighlighting");
    // Jerial.flush();

    // Keep the app's mode in sync with the config (idempotent; only emits SO/SI
    // when the state actually changed).
    pushLineBufferingToApp( );

    int shownMenuItems = 0;
    int menuItemCount[ 4 ] = { 0, 0, 0, 0 };
    int menuItemCounts[ 4 ] = { 14, 22, 37, 46 };

    if ( dontShowMenu == 0 ) {
    forceprintmenu:

        int numberOfMenuItems = menuItemCounts[ showExtraMenu ];
        float steps =
            (float)highSaturationBrightColorsCount / ( (float)numberOfMenuItems );
        // Jerial.print("steps = ");
        // Jerial.println(steps);
        int shownMenuItems = 0;
        // printSpectrumOrderedColorCube();
        cycleTerminalColor( true, steps, true, &Jerial );
        shownMenuItems += printMenuLine( "\n\n\r\t\tMenu\n\r\n\r" );
        shownMenuItems += printMenuLine( "\t'help' for docs or [command]?\n\r" );
        shownMenuItems += printMenuLine( "\n\r" );
        shownMenuItems += printMenuLine( "\tm = show this menu\n\r" );

        shownMenuItems += printMenuLine( showExtraMenu, 0, "\te = show extra options (%d)\n\r", showExtraMenu );

        //  Jerial.println();

        shownMenuItems += printMenuLine( showExtraMenu, 0, "\tn = show net list\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tb = show bridge array\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tc = show crossbar status\n\r" );

        // shownMenuItems += printMenuLine( showExtraMenu, 1, "\ts = show all slot files\n\r" );
        if ( showExtraMenu >= 0 ) {
            Jerial.println( );
        }

        // Jerial.println();

        shownMenuItems += printMenuLine( showExtraMenu, 2, "\t? = show firmware version\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\t' = show startup animation\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\td = set debug flags\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tD = show debug menu\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tl = LED brightness / test\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\t\b\b`/~ = edit / print config\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\tp = microPython REPL\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\t> = send Python formatted command\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\t/ = show filesystem / run script\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\t\b\bU/u = enable/disable USB Mass Storage\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\tX = resource status\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tj = graphic overlay test menu\n\r" );
 

        // Jerial.print("\tu = disable USB Mass Storage drive\n\r");
        // cycleTerminalColor();

        shownMenuItems += printMenuLine( showExtraMenu, 1, "\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tJ = print JSON state\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tL = load JSON state (paste)\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tY = print YAML (0/1/2)\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tS = load YAML state (paste)\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tW = parse Wokwi diagram (paste)\n\r" );
// shownMenuItems += printMenuLine( showExtraMenu, 1, "\n\r" );

        shownMenuItems += printMenuLine( showExtraMenu, 2, "\ty = refresh connections\n\r" );
        // shownMenuItems++;
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\t< = cycle slots (<N selects slot)\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tG = reload config.txt\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\to = load node file by slot\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tP = PSRAM test\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\tF = cycle font\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\t_ = print micros per byte\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\t@ = scan I2C (@[sda],[scl] or @[row])\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\t$ = calibrate DACs\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\t= = dump oled frame buffer\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tk = show oled in terminal\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tt = OLED terminal mode\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tR = show board LEDs\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\t* = raw speed test\n\r" );

        // shownMenuItems += printMenuLine( showExtraMenu, 3, "\t% = list all filesystem contents\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\tE = don't show this menu\n\r" );
        // shownMenuItems += printMenuLine( showExtraMenu, 3, "\tW = disable terminal colors\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\tB = toggle line buffering\n\r" );

        if ( showExtraMenu >= 2 ) {

            // Jerial.print("\n\r");
        }
        Jerial.println( );
        // shownMenuItems += printMenuLine(showExtraMenu, 1, "\n\r");
        //  Jerial.print("\t$ = calibrate DACs\n\r");
        shownMenuItems += printMenuLine( showExtraMenu, 3, "\t^ = set DAC 1 voltage\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tv = get ADC reading\n\r" );
        // Jerial.println();

        shownMenuItems += printMenuLine( showExtraMenu, 3, "\t# = print text from menu\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tg = print gpio state\n\r" );
        // Jerial.print("\t\b\b\b\b[0-9] = run app by index\n\r");
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\t. = connect oled\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tO = cycle OLED pins (%s)\n\r",
                                         getOledConnectionTypeShortName( jumperlessConfig.top_oled.connection_type ) );
        shownMenuItems += printMenuLine( showExtraMenu, 2, "\tr = reset Arduino (rt/rb)\n\r" );

        shownMenuItems += printMenuLine( showExtraMenu, 1, "\t\b\ba/A = dis/connect UART to D0/D1\n\r" );

        shownMenuItems += printMenuLine( showExtraMenu, 1, "\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 1, "\tf = load node file\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\tx = clear all connections\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\t+ = add connections\n\r" );
        shownMenuItems += printMenuLine( showExtraMenu, 0, "\t- = remove connections\n\r" );
        // Jerial.print("\te = extra menu options\n\r");
        // Jerial.println();

        Jerial.println( );

        Jerial.flush( );

        menuItemCount[ showExtraMenu ] = shownMenuItems;
    }
 
}

void SingleCharCommands::printCommandHelp( char cmdChar ) {
    const Command* cmd = getCommand( cmdChar );
    if ( cmd == nullptr ) {
        Jerial.print( "Command '" );
        Jerial.print( cmdChar );
        Jerial.println( "' not found" );
        return;
    }

    Jerial.println( "\n\r╭────────────────────────────────────╮" );
    Jerial.print( "│   Command: " );
    Jerial.print( cmd->trigger );
    Jerial.println( "                       │" );
    Jerial.println( "╰────────────────────────────────────╯\n\r" );

    Jerial.print( "Description: " );
    Jerial.println( cmd->shortDesc );
    Jerial.println( );

    if ( cmd->helpText != nullptr && cmd->helpText[ 0 ] != '\0' ) {
        Jerial.print( "Details: " );
        Jerial.println( cmd->helpText );
    }

    Jerial.println( );
}

void SingleCharCommands::printAllHelp( int category ) {
    Jerial.println( "\n\r╭────────────────────────────────────╮" );
    Jerial.println( "│        Command Reference           │" );
    Jerial.println( "╰────────────────────────────────────╯\n\r" );

    for ( int i = 0; i < commandCount; i++ ) {
        if ( category >= 0 && commands[ i ].category != category ) {
            continue;
        }

        Jerial.print( commands[ i ].trigger );
        Jerial.print( " - " );
        Jerial.println( commands[ i ].shortDesc );
    }
}

const Command* SingleCharCommands::getCommand( char trigger ) const {
    int index = findCommandIndex( trigger );
    return ( index >= 0 ) ? &commands[ index ] : nullptr;
}

int SingleCharCommands::getCommandsByCategory( CommandCategory cat, const Command** outCommands, int maxCount ) const {
    int count = 0;
    for ( int i = 0; i < commandCount && count < maxCount; i++ ) {
        if ( commands[ i ].category == cat ) {
            outCommands[ count++ ] = &commands[ i ];
        }
    }
    return count;
}

int SingleCharCommands::findCommandIndex( char trigger ) const {
    for ( int i = 0; i < commandCount; i++ ) {
        if ( commands[ i ].trigger == trigger ) {
            return i;
        }
    }
    return -1;
}

void SingleCharCommands::sortCommands( ) {
    // Simple bubble sort by category, then by trigger
    for ( int i = 0; i < commandCount - 1; i++ ) {
        for ( int j = 0; j < commandCount - i - 1; j++ ) {
            bool shouldSwap = false;

            if ( commands[ j ].category > commands[ j + 1 ].category ) {
                shouldSwap = true;
            } else if ( commands[ j ].category == commands[ j + 1 ].category &&
                        commands[ j ].trigger > commands[ j + 1 ].trigger ) {
                shouldSwap = true;
            }

            if ( shouldSwap ) {
                Command temp = commands[ j ];
                commands[ j ] = commands[ j + 1 ];
                commands[ j + 1 ] = temp;
            }
        }
    }
}

// int SingleCharCommands::printMenuLine(const char* text, int extraMenuLevel, MenuLevel requiredLevel) {
//     if (extraMenuLevel >= requiredLevel) {
//         Jerial.print(text);
//         return 1;
//     }
//     return 0;
// }

// ============================================================================
// Command Initialization
// ============================================================================

void SingleCharCommands::initializeCommands( ) {
    // === Connection commands ===
    registerCommand( 'f', "load node file",
                     "Load connections from a node file. Prompts for file selection.",
                     cmd_loadNodeFile, MENU_STANDARD, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    registerCommand( 'x', "clear all connections",
                     "Clears all connections and resets the board.",
                     cmd_clearConnections, MENU_BASIC, CAT_CONNECTIONS, true, SER3_MODIFIES_STATE );

    registerCommand( '+', "add connections",
                     "Add new connections. Format: node1-node2,node3-node4",
                     cmd_addConnections, MENU_BASIC, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    registerCommand( '-', "remove connections",
                     "Remove existing connections. Format: node1-node2,node3-node4",
                     cmd_removeConnections, MENU_BASIC, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    registerCommand( 'y', "refresh connections",
                     "Reload and refresh all connections from current slot.",
                     cmd_refreshConnections, MENU_ADVANCED, CAT_CONNECTIONS, true, SER3_MODIFIES_STATE );

    registerCommand( '<', "cycle slots / select slot",
                     "Cycle slots, or jump directly with <N (e.g. <5).",
                     cmd_cycleSlots, MENU_ADVANCED, CAT_CONNECTIONS, true, SER3_MODIFIES_STATE );

    registerCommand( 'o', "load node file by slot",
                     "Load a specific slot by number.",
                     cmd_loadSlot, MENU_ADVANCED, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    registerCommand( 'W', "parse Wokwi diagram",
                     "Paste or load Wokwi diagram.json. Usage: W [slot], W [file], W [file] [slot]",
                     cmd_parseWokwi, MENU_ADVANCED, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    // === Display commands ===
    registerCommand( 'm', "show this menu",
                     "Display the main menu with all available commands.",
                     cmd_showMenu, MENU_BASIC, CAT_DISPLAY, true, SER3_IRRELEVANT );

    registerCommand( 'e', "show extra options",
                     "Toggle through extra menu levels (0-3) for more commands.",
                     cmd_toggleExtraMenu, MENU_BASIC, CAT_DISPLAY, true, SER3_IRRELEVANT );

    registerCommand( 'n', "show net list",
                     "Display current network connections and routing.",
                     cmd_showNetlist, MENU_BASIC, CAT_DISPLAY );

    registerCommand( 'b', "show bridge array",
                     "Display the internal bridge array and paths.",
                     cmd_showBridgeArray, MENU_STANDARD, CAT_DISPLAY );

    registerCommand( 'c', "show crossbar (c! live)",
                     "Display crossbar - compact view. Use c! to toggle live mode.",
                     cmd_showCrossbar, MENU_STANDARD, CAT_DISPLAY );

    registerCommand( 'C', "show crossbar (full)",
                     "Display crossbar switches - full color view with details.",
                     cmd_showCrossbarFull, MENU_STANDARD, CAT_DISPLAY );



    registerCommand( 'Q', "query active slot",
                     "Return the currently active slot number.",
                     cmd_queryActiveSlot, MENU_STANDARD, CAT_DISPLAY );

    // === Python commands ===
    registerCommand( 'p', "microPython REPL",
                     "Enter MicroPython REPL interactive mode.",
                     cmd_pythonREPL, MENU_BASIC, CAT_PYTHON, true, SER3_INTERACTIVE );

    registerCommand( 'P', "PSRAM test (memory integrity + speed)",
                     "Run comprehensive PSRAM tests: size info, integrity check, speed comparison vs SRAM.",
                     cmd_psramTest, MENU_ADVANCED, CAT_HARDWARE, true, SER3_IRRELEVANT );

    registerCommand( '>', "send Python formatted command",
                     "Execute a single Python command. Usage: > print('hello')",
                     cmd_pythonCommand, MENU_BASIC, CAT_PYTHON, true, SER3_INTERACTIVE );

    // === File system commands ===
    registerCommand( '/', "show filesystem / run script",
                     "Open file manager, or /filename.py to run a script directly.",
                     cmd_showFilesystem, MENU_BASIC, CAT_FILE_SYSTEM, true, SER3_INTERACTIVE );

    registerCommand( 'U', "enable USB Mass Storage",
                     "Enable USB drive mode for file access from computer.",
                     cmd_enableUSBStorage, MENU_BASIC, CAT_FILE_SYSTEM, true, SER3_MODIFIES_STATE );

    registerCommand( 'u', "disable USB Mass Storage",
                     "Disable USB drive mode.",
                     cmd_disableUSBStorage, MENU_BASIC, CAT_FILE_SYSTEM, true, SER3_MODIFIES_STATE );

    // registerCommand( '%', "list all filesystem contents",
    //                  "Recursively list all files on the filesystem.",
    //                  cmd_listFilesystem, MENU_DEBUG, CAT_FILE_SYSTEM );

    // === Config commands ===
    registerCommand( '`', "edit config",
                     "Enter config editor to modify configuration.",
                     cmd_editConfig, MENU_BASIC, CAT_SETTINGS, true, SER3_INTERACTIVE );

    registerCommand( '~', "print config",
                     "Display current configuration to serial.",
                     cmd_printConfig, MENU_BASIC, CAT_SETTINGS, true, SER3_IRRELEVANT );

    // === Hardware commands ===
    registerCommand( 'r', "reset Arduino (rt/rb)",
                     "Reset Arduino. Use 'rt' for top, 'rb' for bottom.",
                     cmd_resetArduino, MENU_ADVANCED, CAT_HARDWARE, true, SER3_MODIFIES_STATE );

    registerCommand( 'a', "disconnect UART from D0/D1",
                     "Disconnect Arduino UART from D0 and D1.",
                     cmd_disconnectArduino, MENU_STANDARD, CAT_HARDWARE, true, SER3_MODIFIES_STATE );

    registerCommand( 'A', "connect UART to D0/D1",
                     "Connect Arduino UART to D0 and D1.",
                     cmd_connectArduino, MENU_STANDARD, CAT_HARDWARE, true, SER3_MODIFIES_STATE );

    registerCommand( 'v', "get ADC reading",
                     "Read voltage from ADC. Usage: v[0-4] or vi for current.",
                     cmd_readADC, MENU_STANDARD, CAT_HARDWARE );

    registerCommand( '^', "set DAC voltage",
                     "Set DAC output voltage. Usage: ^ followed by voltage.",
                     cmd_setDAC, MENU_DEBUG, CAT_HARDWARE, true, SER3_INTERACTIVE );

    registerCommand( 'M', "toggle USB audio mic (M01/Ms/M?)",
                     "Toggle the USB Audio microphone. Streams two ADC channels to "
                     "the host as a 2ch/16kHz input device. Optional channel pair, "
                     "e.g. M01 or M23. Ms saves it so it is enumerated from boot "
                     "with no port drop. M? prints status. Re-enumerates USB, so "
                     "this port drops and returns with the same name.",
                     cmd_usbAudio, MENU_STANDARD, CAT_HARDWARE, true, SER3_MODIFIES_STATE );

    registerCommand( '@', "scan I2C",
                     "Scan for I2C devices. Usage: @[row] or @[sda],[scl]",
                     cmd_i2cScan, MENU_ADVANCED, CAT_HARDWARE, true, SER3_MODIFIES_STATE );

    registerCommand( '$', "calibrate DACs",
                     "Run DAC calibration routine.",
                     cmd_calibrateDACs, MENU_DEBUG, CAT_HARDWARE, true, SER3_MODIFIES_STATE );

    // === Debug commands ===
    registerCommand( '?', "show firmware version",
                     "Display current firmware version.",
                     cmd_showVersion, MENU_ADVANCED, CAT_DEBUG );

    registerCommand( 'd', "set debug flags",
                     "Open debug flags menu.",
                     cmd_setDebugFlags, MENU_ADVANCED, CAT_DEBUG, true, SER3_INTERACTIVE );

    registerCommand( 'X', "resource status",
                     "Show system resource allocation and status.",
                     cmd_resourceStatus, MENU_DEBUG, CAT_DEBUG );

    // Phase 5 PSRAM / undo / cache telemetry
    registerCommand( '^', "undo last change",
                     "Revert the last connection or DAC change.",
                     cmd_undo, MENU_STANDARD, CAT_DEBUG, true, SER3_MODIFIES_STATE );

    registerCommand( '&', "redo last undone change",
                     "Reapply the last change that was undone.",
                     cmd_redo, MENU_STANDARD, CAT_DEBUG, true, SER3_MODIFIES_STATE );

    registerCommand( '_', "history status",
                     "Print the undo log status (size, position, recent labels).",
                     cmd_historyStatus, MENU_DEBUG, CAT_DEBUG );

    registerCommand( '(', "print full undo history",
                     "List every transaction in the undo log with a cursor marker.",
                     cmd_historyList, MENU_DEBUG, CAT_DEBUG );

    registerCommand( ')', "PSRAM / file cache status",
                     "Show PSRAM arena, file cache, and undo log status.",
                     cmd_psramStatus, MENU_DEBUG, CAT_DEBUG, true, SER3_IRRELEVANT );

    registerCommand( '%', "toggle PSRAM debug trace",
                     "Toggle verbose trace prints for PSRAM/FileCache/Undo.",
                     cmd_psramDebugToggle, MENU_DEBUG, CAT_DEBUG, true, SER3_MODIFIES_STATE );

    registerCommand( 'g', "print gpio state",
                     "Display state of all GPIO pins.",
                     cmd_gpioState, MENU_ADVANCED, CAT_DEBUG );

#if defined(OG_JUMPERLESS)
    // OG-only diagnostic for the split chip-select banks (A-H on GPIO 6-13,
    // I-L on GPIO 20-23). Kept off V5 so the V5 build stays byte-identical.
    registerCommand( 'I', "test chip-select GPIOs",
                     "Dump pad/IO state for every crossbar chip-select pin "
                     "(A-H + I-L). 'I <chip 0-11>' pulses one chip's CS for scoping.",
                     cmd_testChipSelect, MENU_DEBUG, CAT_DEBUG );
#endif

    registerCommand( 'Z', "USB debug menu",
                     "Open USB debugging options menu.",
                     cmd_usbDebugMenu, MENU_DEBUG, CAT_DEBUG, true, SER3_INTERACTIVE );

    registerCommand( ';', "print wire status",
                     "Print wire status to terminal.",
                     cmd_printWireStatus, MENU_DEBUG, CAT_DEBUG);

    registerCommand( 'H', "fakeGPIO debug (live)",
                     "Live-updating FakeGPIO status showing TDM voltages and pin states.",
                     cmd_fakeGpioDebug, MENU_ADVANCED, CAT_DEBUG, true, SER3_INTERACTIVE );

    registerCommand( 'i', "toggle net current scan (i!/i?/i#/i@)",
                     "Toggle the background net voltage scan / current ants (persists to config).\n\r"
                     "'i!' report  'i?' RouteSafety self-check+audit  'i#' toggle sendXYraw short-check\n\r"
                     "'i@' infra connections status (probe power / OLED / UART arbitration).",
                     cmd_netCurrents, MENU_ADVANCED, CAT_SETTINGS, true, SER3_MODIFIES_STATE );

    registerCommand( 'D', "status diagnostics menu",
                     "Interactive status & diagnostics menu with arrow key navigation.",
                     cmd_statusDiagnosticsMenu, MENU_STANDARD, CAT_DEBUG, true, SER3_INTERACTIVE );

    // === Settings commands ===
    registerCommand( 'l', "LED brightness / test",
                     "Adjust LED brightness or run LED test.",
                     cmd_ledBrightness, MENU_ADVANCED, CAT_SETTINGS, true, SER3_INTERACTIVE );

    registerCommand( '.', "connect oled",
                     "Connect/disconnect OLED display.",
                     cmd_toggleOLED, MENU_STANDARD, CAT_SETTINGS, true, SER3_MODIFIES_STATE );

    registerCommand( 'O', "cycle OLED pins",
                     "Cycle OLED connection type (GPIO 7/8 -> RP6/RP7 -> internal I2C0). "
                     "Pass an argument to jump to a specific type: O0/O1/O2 or e.g. 'O i2c0', 'O gpio_7_8'.",
                     cmd_cycleOledConnectionType, MENU_STANDARD, CAT_SETTINGS, true, SER3_MODIFIES_STATE );

    registerCommand( 'G', "disable terminal colors",
                     "Toggle terminal color output on/off.",
                     cmd_toggleTerminalColors, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    registerCommand( 'B', "toggle line buffering",
                     "Toggle line buffering on/off. For raw terminals without line buffering.",
                     cmd_toggleLineBuffering, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    // Line-buffering sync (single-char protocol, symmetric with what the
    // firmware sends the app): SO (0x0E) enables, SI (0x0F) disables. The legacy
    // DC3 (0x13) trigger is kept as a quiet toggle for backward compatibility.
    registerCommand( '\x0E', "line buffering on (quiet)",
                     "Enable line buffering. Sent by the app to sync state.",
                     cmd_toggleLineBufferingQuiet, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    registerCommand( '\x0F', "line buffering off (quiet)",
                     "Disable line buffering. Sent by the app to sync state.",
                     cmd_toggleLineBufferingQuiet, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    registerCommand( '\x13', "toggle line buffering quietly",
                     "Toggle line buffering on/off. For raw terminals without line buffering.",
                     cmd_toggleLineBufferingQuiet, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    registerCommand( 'E', "don't show this menu",
                     "Toggle automatic menu display.",
                     cmd_dontShowMenu, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    registerCommand( 'k', "show oled in terminal",
                     "Toggle OLED mirroring to terminal.",
                     cmd_oledInTerminal, MENU_ADVANCED, CAT_SETTINGS, true, SER3_MODIFIES_STATE );

    registerCommand( 'F', "cycle font",
                     "Cycle through available OLED fonts.",
                     cmd_cycleFont, MENU_DEBUG, CAT_SETTINGS, true, SER3_IRRELEVANT );

    // === Display (JSON/YAML) ===
    registerCommand( 'J', "show JSON state",
                     "Display state as JSON. J = full; J power|nets|gpio|overlays = that section only.",
                     cmd_showJsonState, MENU_STANDARD, CAT_DISPLAY );

    registerCommand( 'L', "load JSON state",
                     "Load state from JSON. L = full (paste all); L overlays|power|... = paste that section only.",
                     cmd_loadJsonState, MENU_STANDARD, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    registerCommand( 'R', "show board LEDs (R!=toggle R=one-shot)",
                     "Display board LEDs in terminal. R to toggle persistent mode, R! for one-shot dump.",
                     cmd_showBoardLEDs, MENU_ADVANCED, CAT_APPS, true, SER3_MODIFIES_STATE );

    registerCommand( '\'', "show startup animation",
                     "Play the startup animation.",
                     cmd_startupAnimation, MENU_ADVANCED, CAT_APPS, true, SER3_IRRELEVANT );

    registerCommand( 'Y', "print YAML (Y0=plain Y1=colored hex Y2=colored blocks)",
                     "Display current state in YAML format.",
                     cmd_printYAML, MENU_DEBUG, CAT_ADVANCED );

    registerCommand( 'S', "load YAML state",
                     "Paste YAML state (end with an empty line). Same format as Y output.",
                     cmd_loadYAMLState, MENU_STANDARD, CAT_CONNECTIONS, true, SER3_INTERACTIVE );

    registerCommand( '*', "raw speed test",
                     "Run raw crossbar switching speed test.",
                     cmd_rawSpeedTest, MENU_DEBUG, CAT_ADVANCED, true, SER3_IRRELEVANT );

    registerCommand( '=', "dump oled frame buffer",
                     "Dump OLED frame buffer contents.",
                     cmd_dumpOLED, MENU_DEBUG, CAT_ADVANCED, true, SER3_IRRELEVANT );

    registerCommand( '_', "print micros per byte",
                     "Display timing information.",
                     cmd_printMicrosPerByte, MENU_DEBUG, CAT_ADVANCED, true, SER3_IRRELEVANT );

    registerCommand( '#', "print text from menu",
                     "Print text from menu system.",
                     cmd_printTextFromMenu, MENU_DEBUG, CAT_ADVANCED, true, SER3_IRRELEVANT );

    registerCommand( 'q', "DMX Serial mode",
                     "Enter DMX Serial application mode.",
                     cmd_dmxSerial, MENU_DEBUG, CAT_APPS, true, SER3_INTERACTIVE );

    registerCommand( '|', "eratta clear GPIO",
                     "Clear GPIO eratta workaround.",
                     cmd_erattaClear, MENU_DEBUG, CAT_ADVANCED, true, SER3_MODIFIES_STATE );

    registerCommand( 'w', "wavegen",
                     "Wavegen test.",
                     cmd_wavegen, MENU_DEBUG, CAT_ADVANCED, true, SER3_MODIFIES_STATE );

    registerCommand( 't', "OLED terminal mode",
                     "Interactive OLED terminal - type text to display on OLED. Press ESC to exit, 'c' to clear.",
                     cmd_printTextFromTerminal, MENU_ADVANCED, CAT_SETTINGS, true, SER3_INTERACTIVE );

    registerCommand( 'T', "show switch position",
                     "Show switch position.",
                     cmd_showSwitchPosition, MENU_DEBUG, CAT_ADVANCED, true, SER3_IRRELEVANT );

    registerCommand( 'j', "Test overlay",
                     "Test overlay.",
                     cmd_testOverlay, MENU_DEBUG, CAT_ADVANCED, true, SER3_IRRELEVANT );

    registerCommand( 'z', "run project (z <project>[ new|load|run=N])",
                     "Run a project headless - guided or not - the HIL / scripted entry.\n"
                     "Usage: z <project>[ new|load|run=<N>][ noscript]\n"
                     "<project> = a directory name (555) or a wiring path\n"
                     "  (/projects/555/wiring.alt.yaml - picks that variant for a new run).\n"
                     "No mode arg = load the latest run when one exists, else start new.\n"
                     "  new      force a fresh /projects/<dir>/<dir>_<N+1>.yaml\n"
                     "  load     force the latest run file (errors when there is none)\n"
                     "  run=<N>  open that specific run file\n"
                     "  noscript skip the companion script\n"
                     "The run file becomes the ACTIVE CONTEXT and stays it - destination\n"
                     "slots are gone. Guided projects resume from the run file's own\n"
                     "guideProgress; there is no prompt on this path.\n"
                     "Guide keys on this stream: n/space=next  p=back  s=skip\n"
                     "v=verify  q=quit  t <row>=probe-tap override.",
                     cmd_guidedProject, MENU_DEBUG, CAT_APPS, true, SER3_INTERACTIVE );
}


// ============================================================================
// Command Handlers
// ============================================================================

CommandResult cmd_showSwitchPosition( char c, const String& line ) {

    unsigned long currentTime = millis();

    for (int j = 0; j < 10; j++) {
    for (int i = 0; i < 10; i++) {
        showSwitchPosition(i, " ", 0x000000, 0x000000);
        requestLedShow( 2 );
        delay(100);
    }
    for (int i = 9; i >= 0; i--) {
        showSwitchPosition(i, "Fuck", 0x000000, 0x000000);
        requestLedShow( 2 );
        delay(100);
    }
}
        
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_testOverlay( char c, const String& line ) {
    graphicOverlayState.debugMenu();
    return CMD_DONT_SHOW_MENU;
}

// z <project>[ new|load|run=<N>][ noscript]: the project runner's headless
// entry (design-launcher §5.1). Destination slots are GONE - a launch opens or
// creates /projects/<dir>/<dir>_<N>.yaml and leaves it as the active context,
// so there is nothing left for a slot argument to mean.
//
// <project> is a project directory name ("555") or a wiring path
// ("/projects/555/wiring.alt.yaml", which selects that variant for a NEW run).
// No mode arg = load latest when runs exist, else new: the launcher's own
// defaults but WITHOUT the interactive prompt, because headless has to be
// deterministic.
//
// LOUD-FAIL on the old grammar. A stale `z <path> 3` must break visibly, not
// silently into a slot write, so a bare all-digit token ANYWHERE after the
// project is a usage error. Note the token-wise parse: `z 555` is a perfectly
// good all-digit PROJECT name and must keep working - only a digit token that
// FOLLOWS the project is the old destination slot.
static bool zTokenIsRunEquals( const String& tok, int& nOut ) {
    if ( !tok.startsWith( "run=" ) ) return false;
    String num = tok.substring( 4 );
    if ( num.length( ) == 0 || num.length( ) > 4 ) return false;
    for ( unsigned int i = 0; i < num.length( ); i++ ) {
        if ( num.charAt( i ) < '0' || num.charAt( i ) > '9' ) return false;
    }
    nOut = num.toInt( );
    return nOut >= 1;
}

CommandResult cmd_guidedProject( char c, const String& line ) {
    static const char* USAGE =
        "Usage: z <project>[ new|load|run=<N>][ noscript]  "
        "(destination slots are gone - runs live in the project folder)";

    String args = ( line.length( ) > 1 ) ? line.substring( 1 ) : String( "" );
    args.trim( );
    if ( args.length( ) == 0 ) {
        Jerial.println( USAGE );
        return CMD_DONT_SHOW_MENU;
    }

    // `z band <value> [type] [tol]` - the value parser and continuity band
    // derivation, off-bench (invest-measurement.md §5 item 1). It runs no
    // guide and touches no hardware: pure functions in, one machine-parseable
    // GUIDEBAND line out, so the band table can be regression-tested without
    // a part in a hole. Lives on `z` rather than a new single char because
    // this harness is single-char-plus-args and the band IS guide machinery.
    if ( args.startsWith( "band" ) ) {
        String rest = args.substring( 4 );
        rest.trim( );
        Stream* target = Jerial.getResponseTarget( );
        if ( target == nullptr ) target = &Jerial;
        if ( rest.length( ) == 0 ) {
            target->println( "Usage: z band <value> [type] [tolPercent]   "
                             "e.g. 'z band 47k resistor'" );
            return CMD_DONT_SHOW_MENU;
        }
        String v, t, tolTok;
        int p = 0;
        for ( int f = 0; f < 3 && p < (int)rest.length( ); f++ ) {
            int sp = rest.indexOf( ' ', p );
            String tok = ( sp < 0 ) ? rest.substring( p ) : rest.substring( p, sp );
            p = ( sp < 0 ) ? rest.length( ) : sp + 1;
            tok.trim( );
            if ( tok.length( ) == 0 ) { f--; continue; }
            if ( f == 0 ) v = tok;
            else if ( f == 1 ) t = tok;
            else tolTok = tok;
        }
        if ( t.length( ) == 0 ) t = "resistor";
        guideBandReport( v.c_str( ), t.c_str( ),
                         ( tolTok.length( ) > 0 ) ? (int)tolTok.toInt( ) : 0, target );
        return CMD_DONT_SHOW_MENU;
    }

    String project;
    ProjectRunMode mode = ProjectRunMode::DEFAULT;
    int runN = 0;
    bool noScript = false;
    bool modeSeen = false;

    int pos = 0;
    while ( pos < (int)args.length( ) ) {
        int sp = args.indexOf( ' ', pos );
        String tok = ( sp < 0 ) ? args.substring( pos ) : args.substring( pos, sp );
        pos = ( sp < 0 ) ? args.length( ) : sp + 1;
        tok.trim( );
        if ( tok.length( ) == 0 ) continue;

        if ( project.length( ) == 0 ) {
            project = tok;
            continue;
        }

        int n = 0;
        if ( tok == "noscript" ) {
            noScript = true;
        } else if ( tok == "new" || tok == "load" || zTokenIsRunEquals( tok, n ) ) {
            if ( modeSeen ) {
                Jerial.println( USAGE );
                return CMD_DONT_SHOW_MENU;
            }
            modeSeen = true;
            if ( tok == "new" ) {
                mode = ProjectRunMode::NEW;
            } else if ( tok == "load" ) {
                mode = ProjectRunMode::LOAD;
            } else {
                mode = ProjectRunMode::RUN_N;
                runN = n;
            }
        } else {
            // Includes the old grammar's bare destination slot.
            Jerial.println( USAGE );
            return CMD_DONT_SHOW_MENU;
        }
    }

    if ( project.length( ) == 0 ) {
        Jerial.println( USAGE );
        return CMD_DONT_SHOW_MENU;
    }

    // Contradictory input: a wiring PATH names a variant, but load/run=<N>
    // opens an existing run file whose own runSource decides the variant. The
    // argument would be silently ignored, so refuse instead.
    if ( project.indexOf( '/' ) >= 0 &&
         ( mode == ProjectRunMode::LOAD || mode == ProjectRunMode::RUN_N ) ) {
        Jerial.println( USAGE );
        return CMD_DONT_SHOW_MENU;
    }

    if ( project.indexOf( '/' ) >= 0 && !safeFileExists( project.c_str( ) ) ) {
        Jerial.println( "PROJECT error file not found: " + project );
        return CMD_DONT_SHOW_MENU;
    }

    runProjectHeadless( project, mode, runN, noScript );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_printTextFromTerminal( char c, const String& line ) {
    // Interactive OLED terminal mode
    Jerial.println( "\n\r╭────────────────────────────────────╮" );
    Jerial.println( "│     OLED Terminal Mode             │" );
    Jerial.println( "├────────────────────────────────────┤" );
    Jerial.println( "│ Type text to display on OLED       │" );
    Jerial.println( "│ Press Ctrl+Q to exit               │" );
    Jerial.println( "│ Ctrl+A to clear display            │" );
    Jerial.println( "╰────────────────────────────────────╯\n\r" );
    
    if (!oled.isConnected()) {
        Jerial.println( "✗ OLED not connected" );
        Jerial.println( "  Use '.' command to connect OLED first" );
        return CMD_DONT_SHOW_MENU;
    }
    // Force line buffering ON so we receive raw keystrokes, remembering the
    // previous state so we can restore it on exit (instead of forcing it OFF).
    bool prevLineBuffering = setTerminalLineBuffering( true );
    delay(10);

    // Clear OLED and prepare for text
    OLEDOut.clear();
    OLEDOut.println( "OLED Terminal" );
    OLEDOut.println( "Type below:" );
    
    Jerial.println( "Ready. Type your text:" );
    
    String inputLine = "";
    bool exitMode = false;
    
    while (!exitMode) {
        if (Jerial.available() > 0) {
            char ch = Jerial.read();
            
            // Check for exit conditions
            if (ch == 17) {  // Ctrl+Q 
                exitMode = true;
                Jerial.println( "\n\r✓ Exiting OLED terminal mode" );
                break;
            }
            
            // Handle special commands
            if (ch == 0x01) {
                // Clear display
                OLEDOut.clear();
                Jerial.println( "[Display cleared]" );
                continue;
            }


            

            OLEDOut.write(ch);

            if (ch == '\n') {
                Serial.println(" -");
                Serial.flush();
                //continue;
            } else if (ch == ' ' || ch == '\t' || ch == 20) {
                Serial.print(" ");
                Serial.flush();
            }else {
                Serial.print(ch);
                Serial.flush();
            }
            // Echo to serial
  
            
            // Jerial.write(ch);
            // Jerial.flush();
            
            // Handle newline
            // if (ch == '\n' || ch == '\r') {
            //     if (inputLine.length() > 0) {
            //         // Send line to OLED
            //         OLEDOut.println(inputLine);
            //         inputLine = "";
            //     }
            //     continue;
            // }
            
            // Handle backspace

            
            // // Add character to line buffer
            // if (ch >= 32 && ch < 127) {  // Printable characters
            //     inputLine += ch;
                
            //     // Auto-send if line gets too long
            //     if (inputLine.length() >= 21) {  // Max chars per line on OLED
            //         OLEDOut.println(inputLine);
            //         Jerial.println();  // Newline on serial
            //         inputLine = "";
            //     }
            // }
        }
        
        // Small delay to prevent busy-waiting
        delay(1);
    }
    // Restore whatever line-buffering state we had before entering OLED mode.
    setTerminalLineBuffering( prevLineBuffering );
    delay(10);
    return CMD_SHOW_MENU;
}
// ============================================================================
// Command Callback Implementations
// ============================================================================
// These are the actual functions that get called when a command is executed.
// Many of these will just call existing functions from other files.

// Connection commands
CommandResult cmd_clearConnections( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    pinMode( RESETPIN, OUTPUT );
    digitalWrite( RESETPIN, HIGH );
    delay( 6 );
    refreshPaths( );
    clearAllNTCC( );
    // Clear the ACTIVE context, whatever backs it. Passing netSlot here would
    // hand -2 to saveSlot from a file context.
    clearActiveContext( );
    refreshConnections( -1, 1, 1 );
    digitalWrite( RESETPIN, LOW );
    target->println( "Cleared all connections" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_addConnections( char c, const String& line ) {
    // Use source 3 if we have a complete command line (from line buffering or relay)
    // Otherwise use source 0 to read interactively from Jerial
    // Serial.println("Adding connections");
    int source = ( currentCommandLine.length( ) > 1 ) ? 3 : 0;
    // Serial.println("source = ");
    // Serial.print(source);
    // Serial.print("currentCommandLine = ");
    // Serial.println(currentCommandLine);
    // unsigned long startTime = micros();
    readStringFromSerial( source, 0 );
    // After reading connections, they need to be loaded
    firstLoop = 0; // Prevent first-loop logic
    // unsigned long endTime = micros();
    // unsigned long duration = endTime - startTime;
    // Serial.print("Time taken: ");
    // Serial.print(duration);
    // Serial.println(" us");
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_removeConnections( char c, const String& line ) {
    // Use source 3 if we have a complete command line (from line buffering or relay)
    // Otherwise use source 0 to read interactively from Jerial
    int source = ( currentCommandLine.length( ) > 1 ) ? 3 : 0;
    readStringFromSerial( source, 1 );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_loadNodeFile( char c, const String& line ) {
    extern volatile int probeActive;
    extern int readInNodesArduino;
    extern int serSource;
    extern volatile int rotaryEncoderMode;
    extern int input;

    probeActive = 1;
    readInNodesArduino = 1;
    // Jerial.println("Loading node file...");
    // Jerial.println(line);

    // savePreformattedNodeFile handles parsing to RAM, marking dirty, and refresh
    // No need to call refreshConnections again - that would do the work twice!
    // -1, not netSlot: FileParsing's "no Slot N prefix" branch resolves -1 to
    // the ACTIVE context and persists it path-aware. Forwarding netSlot would
    // hand -2 down from a file context.
    savePreformattedNodeFile( serSource, -1, rotaryEncoderMode, line );

    // Validation happens inside savePreformattedNodeFile via refreshLocalConnections
    // which calls the same validation logic. Don't duplicate the work here.

    input = ' ';
    probeActive = 0;

    // The user explicitly loaded a node file, which mutated state.
    // Flush now so it's on flash before they move on.
    // (Unlike the probe-mode exit in Probing.cpp, we don't gate this on
    // a connections-this-session counter - a load that didn't actually
    // parse anything is a parse error and shouldn't reach here anyway.)
    extern void fileCacheFlushNowAll(const char* reason);
    fileCacheFlushNowAll("load_node_file");

    if ( connectFromArduino != '\0' ) {
        connectFromArduino = '\0';
        readInNodesArduino = 0;
        return CMD_DONT_SHOW_MENU;
    }

    connectFromArduino = '\0';
    readInNodesArduino = 0;
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_refreshConnections( char c, const String& line ) {
    return CMD_LOAD_FILE;
}

CommandResult cmd_cycleSlots( char c, const String& line ) {
    extern volatile int slotPreview;

    // Support direct slot selection: "<5" or "< 5".
    // In char-by-char mode the trigger arrives alone, so we have to read
    // the digit(s) from Jerial just like @, J, Y, etc. all do.
    // If no argument arrives within the short window, preserve cycle behavior.
    String slotArg = getCommandArgs( line, 30 );

    if ( slotArg.length( ) > 0 ) {
        bool isNumeric = true;
        for ( unsigned int i = 0; i < slotArg.length( ); i++ ) {
            if ( !isDigit( slotArg[ i ] ) ) {
                isNumeric = false;
                break;
            }
        }

        if ( !isNumeric ) {
            Jerial.print( "Invalid slot argument: " );
            Jerial.println( slotArg );
            return CMD_DONT_SHOW_MENU;
        }

        int requestedSlot = slotArg.toInt( );
        if ( requestedSlot < 0 || requestedSlot > 7 ) {
            Jerial.print( "Invalid slot: " );
            Jerial.println( requestedSlot );
            return CMD_DONT_SHOW_MENU;
        }
        netSlot = requestedSlot;
    } else if ( netSlot == SLOT_FILE_CONTEXT ) {
        // Bare '<' from a file context goes to slot 0 rather than trying to
        // "increment" a sentinel. Leaving the file context is fine: the dirty
        // pre-save in loadfile: flushes it to its own file first.
        netSlot = 0;
    } else if ( netSlot == 7 ) {
        netSlot = 0;
    } else {
        netSlot++;
    }

    Jerial.print( "Slot " );
    Jerial.println( netSlot );

    // Send slot change notification for app synchronization
    Jerial.print( "SLOT_CHANGED:" );
    Jerial.println( netSlot );
    Jerial.flush( );

    slotPreview = netSlot;
    slotChanged = 1;

    // Tell the undo subsystem to swap to this slot's history. Per-slot
    // rings are lazy-allocated, so the new slot's undo/redo state is
    // independent from the slot we just left. (The auto-detect would
    // catch it on the next undo API call too, but doing it eagerly keeps
    // any record* call that lands between here and the next user gesture
    // on the right slot.)
    undoOnSlotSwitch( netSlot );
    return CMD_LOAD_FILE;
}

CommandResult cmd_loadSlot( char c, const String& line ) {
    extern volatile int rotaryEncoderMode;
    inputNodeFileList( rotaryEncoderMode );
    requestLedShow( -1 );
    return CMD_LOAD_FILE;
}

CommandResult cmd_parseWokwi( char c, const String& line ) {
    // Parse Wokwi diagram.json and save to slot
    // Format:
    //   "W"              - Wait for user to paste JSON, save to current slot
    //   "W [slot]"       - Wait for paste, save to specified slot
    //   "W [filename]"   - Load from file, save to current slot
    //   "W [filename] [slot]" - Load from file, save to specified slot

    String filename = "";
    // Use ACTIVE slot (what's loaded), not netSlot (which might be preview/cycling)
    SlotManager& mgr = SlotManager::getInstance( );
    int slotNum = mgr.getActiveSlot( ); // Default to currently LOADED slot
    bool waitForPaste = true;
    bool fromApp = false; // True if JSON was pasted immediately after W

    // Clear any stale command line data to prevent parameter pollution
    extern String currentCommandLine;
    if ( currentCommandLine.length( ) > 10 || currentCommandLine.indexOf( '{' ) >= 0 ) {
        // Command line contains JSON or is suspiciously long - clear it
        currentCommandLine = String( (char)c ); // Reset to just the W command
    }

    if ( debugFP ) {
        Jerial.println( "◆ W command: netSlot=" + String( netSlot ) +
                        ", activeSlot=" + String( slotNum ) +
                        ", previewMode=" + String( mgr.isPreviewMode( ) ? "YES" : "NO" ) );
        Jerial.println( "  Input line: '" + line + "' (length=" + String( line.length( ) ) + ")" );
    }

    // Detect if JSON was pasted immediately (from app) vs interactive user
    // If line contains JSON or Jerial data available within 100ms, it's from app
    if ( line.length( ) > 1 && ( line.indexOf( '{' ) > 0 || line.indexOf( '[' ) > 0 ) ) {
        fromApp = true;
        if ( debugFP ) {
            Jerial.println( "  Detected app paste (JSON in line)" );
        }
    } else {
        // Check if more data is coming soon (app sends it all at once)
        delay( 100 );
        if ( Jerial.available( ) > 0 ) {
            fromApp = true;
            if ( debugFP ) {
                Jerial.println( "  Detected app paste (data available after 100ms)" );
            }
        }
    }

    // Parse command line if provided
    // IMPORTANT: When user pastes JSON after "W", the line may be "W{...}" or contain garbage
    // We need to detect if params starts with '{' or other non-slot characters
    // and ignore them (they're part of the JSON paste, not command args)
    if ( line.length( ) > 1 ) {
        String params = line.substring( 1 );
        params.trim( );

        if ( debugFP ) {
            Jerial.println( "  Parsing params: '" + params + "' (length=" + String( params.length( ) ) +
                            ", first char='" + ( params.length( ) > 0 ? String( params[ 0 ] ) : "" ) + "')" );
        }

        // Skip JSON content: if params starts with '{', '[', or '"', it's JSON not args
        if ( params.length( ) > 0 && ( params[ 0 ] == '{' || params[ 0 ] == '[' || params[ 0 ] == '"' ) ) {
            if ( debugFP ) {
                Jerial.println( "  → Detected JSON in params, ignoring - using slot " + String( slotNum ) );
            }
            // Don't parse parameters - user is pasting JSON
            // Keep slotNum at default (activeSlot)
        }
        // Check if parameters provided
        else if ( params.length( ) > 0 ) {
            int spaceIdx = params.indexOf( ' ' );
            if ( spaceIdx > 0 ) {
                // Two parameters: filename and slot
                filename = params.substring( 0, spaceIdx );
                filename.trim( );
                String slotStr = params.substring( spaceIdx + 1 );
                slotStr.trim( );
                slotNum = slotStr.toInt( );
                waitForPaste = false;
                if ( debugFP ) {
                    Jerial.println( "  Parsed: filename='" + filename + "', slot=" + String( slotNum ) );
                }
            } else {
                // Single parameter: filename or slot number
                if ( params[ 0 ] >= '0' && params[ 0 ] <= '9' ) {
                    // It's a slot number - wait for paste
                    slotNum = params.toInt( );
                    waitForPaste = true;
                    if ( debugFP ) {
                        Jerial.println( "  Parsed slot number: " + String( slotNum ) );
                    }
                } else {
                    // It's a filename
                    filename = params;
                    waitForPaste = false;
                    if ( debugFP ) {
                        Jerial.println( "  Parsed filename: '" + filename + "'" );
                    }
                }
            }
        }
    }

    if ( debugFP ) {
        Jerial.println( "  Final target slot: " + String( slotNum ) +
                        " (waitForPaste=" + String( waitForPaste ) +
                        ", filename='" + filename + "', fromApp=" + String( fromApp ) + ")" );
    }

    // Validate slot number
    if ( slotNum < 0 || slotNum >= NUM_SLOTS ) {
        Jerial.println( "◇ Invalid slot number: " + String( slotNum ) );
        return CMD_SHOW_MENU;
    }

    String errorMsg;
    bool success = false;

    if ( waitForPaste ) {
        // Only show prompts if this is interactive (not from app)
        if ( !fromApp ) {
            Jerial.println( "◆ Paste Wokwi diagram.json content (ends with '}')\n" );
            Jerial.println( "  Target slot: " + String( slotNum ) );
        } else if ( debugFP ) {
            Jerial.println( "  Target slot: " + String( slotNum ) + " (app mode - no prompt)" );
        }

        // Read pasted JSON content
        String jsonContent = "";
        jsonContent.reserve( 8192 ); 

        int braceCount = 0;
        bool foundOpenBrace = false;

        // Check if we already have JSON content in the command line (e.g. "W{...")
        int startBrace = line.indexOf('{');
        if (startBrace == -1) startBrace = line.indexOf('[');
        
        if (startBrace != -1) {
            String initialChunk = line.substring(startBrace);
            jsonContent = initialChunk;
            foundOpenBrace = true;
            for (unsigned int i = 0; i < initialChunk.length(); i++) {
                if (initialChunk[i] == '{' || initialChunk[i] == '[' || initialChunk[i] == '(') braceCount++;
                else if (initialChunk[i] == '}' || initialChunk[i] == ']' || initialChunk[i] == ')') braceCount--;
            }
            if (debugFP) {
                Jerial.println("  Initial chunk: " + String(initialChunk.length()) + " bytes, braceCount=" + String(braceCount));
            }
        }

        if (!(foundOpenBrace && braceCount <= 0)) {
            // Only show hint if interactive and we don't have a complete object yet
            unsigned long humanTime = millis( );
            int shown = 0;
            while ( Jerial.available( ) == 0 && jsonContent.length() == 0 ) {
                if ( !fromApp && millis( ) - humanTime == 2000 && shown == 0 ) {
                    Jerial.println( "\n  Waiting for JSON paste..." );
                    Jerial.println( "  (Copy from Wokwi editor: diagram.json tab)" );
                    shown = 1;
                }
                delay( 10 );
                if (millis() - humanTime > 10000) break; // 10s timeout waiting for start
            }

            unsigned long lastCharTime = millis( );
            while ( true ) {
                if ( Jerial.available( ) > 0 ) {
                    char c = Jerial.read( );
                    jsonContent += c;
                    lastCharTime = millis( );

                    // Track braces to detect complete JSON
                    if ( c == '{' || c == '[' || c == '(' ) {
                        foundOpenBrace = true;
                        braceCount++;
                    } else if ( c == '}' || c == ']' || c == ')' ) {
                        braceCount--;
                        // If we found opening brace and brace count is back to 0, we're done
                        if ( foundOpenBrace && braceCount <= 0 ) {
                            break;
                        }
                    }

                    // Show progress every 256 bytes (only if interactive or debug)
                    if ( !fromApp || debugFP ) {
                        if ( jsonContent.length( ) % 256 == 0 ) {
                            Jerial.print( "." );
                        }
                    }
                } else {
                    // No data available
                    if ( jsonContent.length( ) > 0 ) {
                        // Check timeout (increased to 1000ms after last character for safety)
                        if ( millis( ) - lastCharTime > 1000 ) {
                            if ( debugFP ) {
                                Jerial.println( "\n  Timeout: 1000ms since last character, braceCount=" + String(braceCount) );
                            }
                            break;
                        }
                        delay( 5 ); // Small delay waiting for more data
                    } else {
                        delay( 5 ); 
                        if (millis() - humanTime > 15000) break; // Final exit if nothing happens
                    }
                }

                // Safety: max 8KB
                if ( jsonContent.length( ) > 8000 ) {
                    Jerial.println( "\n◇ Warning: JSON too large (>8KB), truncating" );
                    break;
                }
            }
            
            // Allow trailing characters if object is complete
            if (foundOpenBrace && braceCount <= 0) {
                delay( 50 );
                while ( Jerial.available( ) > 0 ) {
                    char trailing = Jerial.read( );
                    if ( trailing == '\n' || trailing == '\r' || trailing == ' ' ) continue;
                    // The app brackets bulk sends with SI before / SO after to keep
                    // the device out of line-buffering mode for the message body. The
                    // trailing SO (restore) lands right after the JSON; honor it
                    // here so it re-enables buffering instead of being appended as
                    // a stray control char that corrupts the JSON (and leaves the
                    // line editor stuck off).
                    if ( trailing == 0x0E ) { acknowledgeAppLineBuffering( true ); continue; }
                    if ( trailing == 0x0F ) { acknowledgeAppLineBuffering( false ); continue; }
                    jsonContent += trailing;
                }
                }
        }
        jsonContent.trim( );

        // Print final JSON once as requested
        // Jerial.println(jsonContent);

        // Only show "Received" message if interactive or debug
        if ( !fromApp ) {
            Jerial.println( "\n◆ Received " + String( jsonContent.length( ) ) + " bytes" );
        } else if ( debugFP ) {
            Jerial.println( "◆ Received " + String( jsonContent.length( ) ) + " bytes (from app)" );
        }

        // Debug: Show what we received if debugFP is on
        if ( debugFP ) {
            Jerial.println( "◆ First 200 chars:" );
            Jerial.println( jsonContent.substring( 0, 200 ) );
        }

        // Parse into target slot WITHOUT affecting active state or hardware
        // mgr already declared earlier in function
        int currentActiveSlot = mgr.getActiveSlot( );
        bool isActiveSlot = ( currentActiveSlot == slotNum );

        if ( isActiveSlot ) {
            // ========== ACTIVE CONTEXT: Parse directly and apply to hardware ==========
            // Covers a numbered slot AND a file context: slotNum defaulted to
            // getActiveSlot() above, so from a file context both sides of the
            // isActiveSlot test are SLOT_FILE_CONTEXT and we land here. The
            // only difference is that persistence and the reapply-reload go
            // through the path instead of the number.
            const bool intoFile = ( slotNum == SLOT_FILE_CONTEXT );
            const String ctxPath = String( mgr.getActiveSlotPath( ) );
            const String ctxLabel = intoFile ? ctxPath : ( "slot " + String( slotNum ) );

            // Only clear connections, not power settings to prevent LED flicker
            // When we avoid clearing power, the LEDs won't blink to 0V between updates
            mgr.getActiveState( ).connections.clear( );
            // Power settings remain unchanged unless the parser explicitly updates them
            // This prevents LEDs from briefly showing 0V on rails during updates

            // Parse directly into active state
            if ( parseWokwiDiagram( jsonContent, mgr.getActiveState( ), slotNum, errorMsg, fromApp ) ) {
                bool saved = intoFile ? mgr.saveActiveSlot( errorMsg )
                                      : mgr.saveSlot( slotNum, errorMsg );
                if ( saved ) {
                    if ( !fromApp ) {
                        Jerial.println( "  ✓ Saved and applied to " + ctxLabel );
                    } else if ( debugFP ) {
                        Jerial.println( "  ✓ Saved and applied to " + ctxLabel + " (app mode)" );
                    }
                    success = true;

                    // Apply to hardware
                    if ( !fromApp || debugFP ) {
                        Jerial.println( "  ↻ Applying to hardware..." );
                    }
                    bool applied = intoFile ? mgr.loadSlotFromPath( ctxPath, errorMsg )
                                            : mgr.loadSlot( slotNum, errorMsg );
                    if ( applied ) {
                        if ( !fromApp || debugFP ) {
                            Jerial.println( "  ✓ Applied to hardware" );
                        }
                    } else {
                        Jerial.println( "  ✗ Failed to apply: " + errorMsg );
                    }
                } else {
                    Jerial.println( "  ✗ Failed to save: " + errorMsg );
                }
            } else {
                Jerial.println( "  ✗ Parse error: " + errorMsg );
            }
        } else {
            // ========== INACTIVE SLOT: Parse directly to file (ZERO-COPY) ==========
            // This avoids creating ANY JumperlessState objects (50KB each)
            // parseWokwiDiagramDirectToFile builds minimal YAML and writes to file
            if ( parseWokwiDiagramDirectToFile( jsonContent, slotNum, errorMsg, fromApp ) ) {
                if ( !fromApp ) {
                    Jerial.println( "  ✓ Saved to inactive slot " + String( slotNum ) );
                } else if ( debugFP ) {
                    Jerial.println( "  ✓ Saved to inactive slot " + String( slotNum ) + " (no hardware change)" );
                }
                success = true;
            } else {
                Jerial.println( "  ✗ Parse error: " + errorMsg );
            }
        }

    } else {
        // Load from file
        Jerial.println( "◆ Parsing Wokwi diagram: " + filename );
        Jerial.println( "  Target slot: " + String( slotNum ) );

        extern bool parseWokwiDiagramFromFile( const String&, int, String& );
        success = parseWokwiDiagramFromFile( filename, slotNum, errorMsg );

        if ( success ) {
            Jerial.println( "◆ Wokwi diagram successfully converted and saved!" );
        } else {
            Jerial.println( "◇ Failed to parse Wokwi diagram: " + errorMsg );
        }
    }

    // Only show hint if interactive and slot needs to be cycled to
    if ( !fromApp && success && slotNum != netSlot ) {
        Jerial.println( "  Use '<' to cycle to slot " + String( slotNum ) + " to activate it" );
    }

    return CMD_DONT_SHOW_MENU;
}

// Display commands
CommandResult cmd_showMenu( char c, const String& line ) {
    return CMD_SHOW_MENU;
}

CommandResult cmd_toggleExtraMenu( char c, const String& line ) {
    showExtraMenu++;
    if ( showExtraMenu > 3 ) {
        showExtraMenu = 0;
    }
    return CMD_SHOW_MENU;
}

CommandResult cmd_showNetlist( char c, const String& line ) {
    extern volatile int core1passthrough;
    couldntFindPath( 1 );
    
    Stream* target = Jerial.getResponseTarget();
    if (target == nullptr) target = &Jerial;
  
    target->print( "\n\n\rnetlist\n\r" );
    listNets( anythingInteractiveConnected( -1 ), target );
    return CMD_DONT_SHOW_MENU;
}

// Get the argument string that follows the trigger character.
// In line-buffered mode, the full command is in `line` (e.g. "J power").
// In char-by-char mode, only the trigger char arrived and the rest is still in the serial buffer.
// This function handles both cases transparently.
// True when the command being executed arrived as a COMPLETE line (line
// mode, a relayed <j> command, the port-7 backchannel): its arguments are in
// `line` and nothing more is coming, so getCommandArgs() must not wait on the
// stream. loop() clears it only on the char-mode single-char read path, where
// the arguments (if any) are still arriving. Before this every arg-taking
// command in line mode - the jumperless app's mode - paid its 20-100 ms
// timeout for nothing (T1.7b, 2026-08-17).
bool g_commandInputIsLine = true;

static String getCommandArgs( const String& line, unsigned int timeoutMs = 50 ) {
    // If line already has content after the trigger character, use it
    if ( line.length( ) > 1 ) {
        String args = line.substring( 1 );
        args.trim( );
        if ( args.length( ) > 0 ) return args;
    }

    // A complete line with nothing after the trigger: no arguments, and no
    // point waiting - the terminator already came.
    if ( g_commandInputIsLine ) {
        return String( );
    }

    // Otherwise read from Jerial (char-by-char mode)
    String args;
    unsigned long start = millis( );
    while ( millis( ) - start < timeoutMs ) {
        while ( Jerial.available( ) > 0 ) {
            char ch = (char) Jerial.read( );
            if ( ch == '\n' || ch == '\r' ) {
                args.trim( );
                return args;
            }
            args += ch;
        }
        delay( 1 );
    }
    args.trim( );
    return args;
}

// ---------------------------------------------------------------------------
// Pasted-block reader shared by L (JSON) and S (YAML). Reads RAW from Serial
// (port 1): while a paste command is collecting, the main loop - and with it
// Jerial - is not serviced, so the bytes are still in the CDC FIFO.
//
// Why this is not "read lines until an empty line" any more: Y's YAML has blank
// lines between its sections (after sourceOfTruth:, after the bridges, after
// power:), so a pasted Y round-trip ended at the FIRST blank line and every line
// after it fell through to the menu as a command. It only "used to work" when the
// terminal sent CR-only line ends: readStringUntil('\n') then swallowed the whole
// paste as one 1-second-timeout blob (blank lines invisible) and Enter finished
// it. The jumperless app now sends '\n' per Enter, which exposed the design.
//
// Rules: '\n', '\r' and "\r\n" all end a line (each line is trimmed, as before -
// the parsers do not depend on indentation). Blank lines BEFORE any content are
// skipped (a char-mode "S<CR><LF>" leaves its line end in the FIFO); blank lines
// INSIDE the paste are kept. The block ends when an empty line is followed by
// PASTE_QUIET_MS of silence - a paste burst never pauses that long, even
// forwarded one byte per USB write - or, as before, when nothing arrives for
// PASTE_IDLE_MS after content (a partial last line is flushed). Returns false if
// nothing at all arrived within PASTE_IDLE_MS.
// ---------------------------------------------------------------------------
static const unsigned long PASTE_QUIET_MS = 500;
static const unsigned long PASTE_IDLE_MS = 30000;

static bool readPastedBlock( String& out ) {
    out = "";
    String lineBuf;
    lineBuf.reserve( 256 );

    bool gotContent = false;      // at least one non-empty line collected
    bool prevWasCR = false;       // to fold "\r\n" into one line end
    bool pendingEmpty = false;    // an empty line completed after content - the
                                  // terminator unless more bytes follow it
    unsigned long emptyAtMs = 0;
    unsigned long lastByteMs = millis( );

    auto completeLine = [ & ]( ) {
        lineBuf.trim( );
        if ( lineBuf.length( ) == 0 ) {
            if ( gotContent ) {
                if ( pendingEmpty ) out += "\n"; // a run of blank lines: keep the earlier one
                pendingEmpty = true;
                emptyAtMs = millis( );
            }
            return; // leading blank lines are skipped
        }
        if ( pendingEmpty ) {
            out += "\n"; // that blank line was inside the paste, not the end
            pendingEmpty = false;
        }
        out += lineBuf;
        out += "\n";
        gotContent = true;
        lineBuf = "";
    };

    while ( true ) {
        while ( Serial.available( ) ) {
            char ch = (char)Serial.read( );
            lastByteMs = millis( );
            if ( ch == '\r' ) {
                completeLine( );
                lineBuf = "";
                prevWasCR = true;
                continue;
            }
            if ( ch == '\n' ) {
                if ( !prevWasCR ) {
                    completeLine( );
                    lineBuf = "";
                }
                prevWasCR = false;
                continue;
            }
            prevWasCR = false;
            lineBuf += ch;
        }

        unsigned long now = millis( );
        if ( pendingEmpty && now - emptyAtMs >= PASTE_QUIET_MS ) {
            break; // empty line + quiet = end of paste
        }
        if ( now - lastByteMs >= PASTE_IDLE_MS ) {
            break; // nothing for a long time: apply what we have (as before)
        }
        yield( );  // pump USB and flush the prompt while we wait
        delay( 1 );
    }

    // A last line without a terminator (paste that does not end in a newline
    // and no Enter) is still content.
    lineBuf.trim( );
    if ( lineBuf.length( ) > 0 ) {
        if ( pendingEmpty ) out += "\n";
        out += lineBuf;
        out += "\n";
        gotContent = true;
    }
    return gotContent;
}

CommandResult cmd_showJsonState( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    String section = getCommandArgs( line );
    const char* sectionPtr = ( section.length( ) > 0 ) ? section.c_str( ) : nullptr;
    String json = JsonState::getJumperlessStateJSON( sectionPtr );
    target->print( "\n\n\r" );
    target->print( json );
    target->print( "\n\r" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_loadJsonState( char c, const String& line ) {
    String section = getCommandArgs( line );
    bool partialLoad = ( section.length( ) > 0 );
    if ( partialLoad )
        Jerial.print( "\n\rPaste JSON for '" + section + "' (end with an empty line):\n\r" );
    else
        Jerial.print( "\n\rPaste JSON state (end with an empty line):\n\r" );
    Jerial.flush( );

    String jsonBuffer;
    jsonBuffer.reserve(8192);

    // Blank lines inside the paste are kept; an empty line followed by quiet
    // ends it (see readPastedBlock - the old "first empty line ends it" cut
    // multi-section pastes short and fed the rest to the menu as commands).
    if ( !readPastedBlock( jsonBuffer ) ) {
        Jerial.print( "\r\nNo JSON received\n\r" );
        return CMD_SHOW_MENU;
    }

    // Normalize pasted JSON so the parser sees an object
    jsonBuffer.trim();
    if ( jsonBuffer.length( ) > 0 && jsonBuffer.charAt( 0 ) != '{' ) {
        if ( section.equalsIgnoreCase( "overlays" ) && jsonBuffer.charAt( 0 ) == '[' )
            jsonBuffer = "{\"overlays\":" + jsonBuffer + "}";
        else if ( jsonBuffer.charAt( 0 ) == '"' )
            jsonBuffer = "{" + jsonBuffer + "}";
    }

    Jerial.print( "\r\nApplying state...\n\r" );

    // Partial load: only update the given section, do not clear connections
    bool success = JsonStateParser::applyJSONState( jsonBuffer, !partialLoad );

    if (success) {
        Jerial.print( "State applied successfully!\n\r" );
    } else {
        Jerial.print( "Error: " );
        Jerial.print( JsonStateParser::getLastError() );
        Jerial.print( "\n\r" );
    }

    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_showBridgeArray( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;

    int showDupes = 1;
    String arg = getCommandArgs( line, 20 );
    if ( arg.length( ) > 0 ) {
        if ( arg[ 0 ] == '0' ) {
            showDupes = 0;
        } else if ( arg[ 0 ] == '2' ) {
            showDupes = 2;
        }
    }

    target->print( "\n\rpathDuplicates: " );
    target->println( jumperlessConfig.routing.stack_paths );
    target->print( "dacDuplicates: " );
    target->println( jumperlessConfig.routing.stack_dacs );
    target->print( "railsDuplicates: " );
    target->println( jumperlessConfig.routing.stack_rails );
    target->print( "railPriority: " );
    target->println( jumperlessConfig.routing.rail_priority );
    couldntFindPath( 1 );
    target->print( "\n\rBridge Array\n\r" );
    printBridgeArray( target );
    target->print( "\n\n\n\rPaths\n\r" );
    printPathsCompact( showDupes, target );
    target->print( "\n\n\rChip Status\n\r" );
    printChipStatus( target );
    target->print( "\n\n\r" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_showCrossbar( char c, const String& line ) {
    // Check if user typed 'c!' to toggle live mode
    String arg = getCommandArgs( line, 100 );
    if ( arg.length( ) > 0 && arg[ 0 ] == '!' ) {
        extern bool liveCrossbarEnabled;
        setLiveCrossbarEnabled( !liveCrossbarEnabled );
        return CMD_DONT_SHOW_MENU;
    }
    // c0 / c1 — force live mode off or on
    if ( arg.length( ) > 0 && ( arg[ 0 ] == '0' || arg[ 0 ] == '1' ) ) {
        extern bool liveCrossbarEnabled;
        bool enable = ( arg[ 0 ] == '1' );
        setLiveCrossbarEnabled( enable );
        Jerial.println( enable ? "Live crossbar enabled" : "Live crossbar disabled" );
        return CMD_SHOW_MENU;
    }

    Stream* target = Jerial.getResponseTarget();
    if (target == nullptr) target = &Jerial;

    // Otherwise show compact crossbar view
    printChipStateArrayColorCompact( 12 , '.', target );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_showCrossbarFull( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget();
    if (target == nullptr) target = &Jerial;
    printChipStateArrayColor( target );  // Full detailed view with 3-char symbols
    return CMD_DONT_SHOW_MENU;
}



CommandResult cmd_queryActiveSlot( char c, const String& line ) {
    SlotManager& mgr = SlotManager::getInstance( );
    int activeSlot = mgr.getActiveSlot( );

    Stream* target = Jerial.getResponseTarget();
    if (target == nullptr) target = &Jerial;

    // Output in a format easy for the app to parse.
    //
    // ACTIVE_SLOT is 0-7 or 99 for a numbered slot and -1 for a file context.
    // -1, not the internal -2: -1 already means "no numbered slot" to every
    // existing integer parser, so nothing downstream has to learn a new value.
    //
    // ACTIVE_PATH ALWAYS follows - numbered slots report their canonical
    // /slots/slotN.yaml too. The path line is the new truth; the int line is
    // back-compat. (There is deliberately no ACTIVE_CONTEXT: line.)
    target->print( "ACTIVE_SLOT:" );
    target->println( activeSlot == SLOT_FILE_CONTEXT ? -1 : activeSlot );
    target->print( "ACTIVE_PATH:" );
    target->println( mgr.getActiveSlotPath( ) );
    target->flush( );

    return CMD_DONT_SHOW_MENU;
}


CommandResult cmd_toggleLineBuffering( char c, const String& line ) {
    // Firmware-side toggle (the visible 'B' command). Notify the app of the
    // change through the single control path.
    String arg = getCommandArgs( line, 50 );
    bool target;
    if ( arg.length( ) > 0 && ( arg[ 0 ] == '0' || arg[ 0 ] == '1' ) ) {
        target = ( arg[ 0 ] == '1' );
    } else {
        target = ( jumperlessConfig.display.terminal_line_buffering == 0 );
    }
    setTerminalLineBuffering( target );
    if (millis() > 10000) {
        Jerial.print( "Line buffering " );
        Jerial.println( jumperlessConfig.display.terminal_line_buffering ? "enabled" : "disabled" );
        configChanged = true;
        
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_toggleLineBufferingQuiet( char c, const String& line ) {
    // Line-buffering sync (raw / unbuffered input path). The trigger char itself
    // is the directive, matching the bytes the firmware sends the app:
    //   SO (0x0E) -> ON, SI (0x0F) -> OFF — app-initiated, so adopt silently
    //   (no echo back). Any other trigger (legacy DC3 0x13) toggles and notifies.
    if ( c == '\x0E' ) {
        acknowledgeAppLineBuffering( true );
    } else if ( c == '\x0F' ) {
        acknowledgeAppLineBuffering( false );
    } else {
        setTerminalLineBuffering( jumperlessConfig.display.terminal_line_buffering == 0 );
    }
    return CMD_DONT_SHOW_MENU;
}


// Python commands
CommandResult cmd_pythonREPL( char c, const String& line ) {
    // Set Python output streams to Jerial for the interactive REPL with correct interrupt char
    // global_mp_stream is already declared in Python_Proper.h (included above)
    extern void setGlobalStreamWithInterrupt(Stream *stream);  // From Python_Proper.cpp
    setGlobalStreamWithInterrupt(&Jerial);

    changeTerminalColor(208, true, &Jerial);
    Jerial.print( "\n\n\rThere's now a better way to do this! ");
    changeTerminalColor(214, true, &Jerial);
    Jerial.println( "Go to:\n\r" );

    changeTerminalColor(190, true, &Jerial);
    Jerial.println( "https://viper-ide.org/\n\r");

    changeTerminalColor(112, true, &Jerial);
    Jerial.println( "and connect to the 3rd Jumperless serial port.\n\n\n\r");

    delay(1000);

    enterMicroPythonREPL( );
    refreshConnections( -1, 1, 1 );
    // Returning to the menu: resync the app to the user's line-buffering config
    // (don't force a specific mode here).
    pushLineBufferingToApp( );
    return CMD_SHOW_MENU;
}

CommandResult cmd_psramTest( char c, const String& line ) {

    action_psramTest();
    return CMD_DONT_SHOW_MENU;
    Serial.println( "\n=== PSRAM Test Suite ===" );
    Serial.flush();
    Serial.println( "Config psram_installed: " + String( jumperlessConfig.hardware.psram_installed ) );
    Serial.flush();
    
    // Show regular SRAM info first (this is always safe)
    Serial.println( "\n--- SRAM Info ---" );
    Serial.println( "SRAM Total: " + String( rp2040.getTotalHeap() / 1024 ) + " KB" );
    Serial.println( "SRAM Free: " + String( rp2040.getFreeHeap() / 1024 ) + " KB" );
    Serial.flush();
    
    // Try to get PSRAM size - this may crash if no PSRAM is present
    Serial.println( "\n--- PSRAM Detection ---" );
    Serial.println( "Checking PSRAM size..." );
    Serial.flush();
    
    size_t psramSize = rp2040.getPSRAMSize();
    Serial.println( "PSRAM Chip Size: " + String( psramSize / 1024 / 1024 ) + " MB (" + String( psramSize ) + " bytes)" );
    Serial.flush();
    
    if ( psramSize == 0 ) {
        Serial.println( "\nNo PSRAM detected!" );
        Serial.println( "If you have installed the PSRAM mod, check:" );
        Serial.println( "  - PSRAM chip is properly soldered" );
        Serial.println( "  - CS pin (GPIO 19) connection" );
        Serial.println( "  - Power and ground connections" );
        Serial.flush();
        return CMD_DONT_SHOW_MENU;
    }
    
    // PSRAM detected - get heap info
    Serial.println( "Getting PSRAM heap info..." );
    Serial.flush();
    
    size_t psramTotal = rp2040.getTotalPSRAMHeap();
    size_t psramUsed = rp2040.getUsedPSRAMHeap();
    size_t psramFree = rp2040.getFreePSRAMHeap();
    
    Serial.println( "\n--- PSRAM Info ---" );
    Serial.println( "PSRAM Heap Total: " + String( psramTotal / 1024 ) + " KB" );
    Serial.println( "PSRAM Heap Used: " + String( psramUsed / 1024 ) + " KB" );
    Serial.println( "PSRAM Heap Free: " + String( psramFree / 1024 ) + " KB" );
    Serial.flush();
    
    // Memory integrity test - start small
    Serial.println( "\n--- Memory Integrity Test ---" );
    Serial.flush();
    
    // Try a small allocation first
    Serial.println( "Testing small allocation (256 bytes)..." );
    Serial.flush();
    
    uint32_t* testSmall = (uint32_t*)pmalloc( 256 );
    if ( testSmall == nullptr ) {
        Serial.println( "ERROR: Small pmalloc() failed!" );
        Serial.flush();
        return CMD_DONT_SHOW_MENU;
    }
    
    // Quick write/read test
    testSmall[0] = 0xDEADBEEF;
    testSmall[1] = 0xCAFEBABE;
    Serial.flush();
    
    if ( testSmall[0] != 0xDEADBEEF || testSmall[1] != 0xCAFEBABE ) {
        Serial.println( "ERROR: Basic read/write test FAILED!" );
        Serial.println( "  Wrote: 0xDEADBEEF, Read: 0x" + String( testSmall[0], HEX ) );
        Serial.println( "  Wrote: 0xCAFEBABE, Read: 0x" + String( testSmall[1], HEX ) );
        free( testSmall );
        Serial.flush();
        return CMD_DONT_SHOW_MENU;
    }
    Serial.println( "Small allocation test: PASS" );
    free( testSmall );
    Serial.flush();
    
    // Now try larger test
    const size_t testSize = 64 * 1024; // 64KB test block
    Serial.println( "Allocating " + String( testSize / 1024 ) + " KB test block..." );
    Serial.flush();
    
    uint32_t* psramBlock = (uint32_t*)pmalloc( testSize );
    if ( psramBlock == nullptr ) {
        Serial.println( "ERROR: Failed to allocate PSRAM test block!" );
        Serial.flush();
        return CMD_DONT_SHOW_MENU;
    }
    Serial.println( "Allocation successful at address: 0x" + String( (uint32_t)psramBlock, HEX ) );
    Serial.flush();
    
    size_t numWords = testSize / sizeof(uint32_t);
    int errors = 0;
    
    // Test 1: Sequential pattern
    Serial.print( "Test 1: Sequential pattern... " );
    Serial.flush();
    for ( size_t i = 0; i < numWords; i++ ) {
        psramBlock[i] = i;
    }
    for ( size_t i = 0; i < numWords; i++ ) {
        if ( psramBlock[i] != i ) {
            errors++;
            if ( errors <= 5 ) {
                Serial.println( "Error at " + String(i) + ": expected " + String(i) + ", got " + String(psramBlock[i]) );
            }
        }
    }
    Serial.println( errors == 0 ? "PASS" : "FAIL (" + String(errors) + " errors)" );
    Serial.flush();
    
    // Test 2: Alternating bits pattern (0x55555555 / 0xAAAAAAAA)
    errors = 0;
    Serial.print( "Test 2: Alternating bits (0x55/0xAA)... " );
    Serial.flush();
    for ( size_t i = 0; i < numWords; i++ ) {
        psramBlock[i] = ( i & 1 ) ? 0xAAAAAAAA : 0x55555555;
    }
    for ( size_t i = 0; i < numWords; i++ ) {
        uint32_t expected = ( i & 1 ) ? 0xAAAAAAAA : 0x55555555;
        if ( psramBlock[i] != expected ) {
            errors++;
        }
    }
    Serial.println( errors == 0 ? "PASS" : "FAIL (" + String(errors) + " errors)" );
    Serial.flush();
    
    // Test 3: Walking ones
    errors = 0;
    Serial.print( "Test 3: Walking ones pattern... " );
    Serial.flush();
    for ( size_t i = 0; i < numWords; i++ ) {
        psramBlock[i] = 1 << ( i % 32 );
    }
    for ( size_t i = 0; i < numWords; i++ ) {
        uint32_t expected = 1 << ( i % 32 );
        if ( psramBlock[i] != expected ) {
            errors++;
        }
    }
    Serial.println( errors == 0 ? "PASS" : "FAIL (" + String(errors) + " errors)" );
    Serial.flush();
    
    // Test 4: All zeros and all ones
    errors = 0;
    Serial.print( "Test 4: All zeros/ones... " );
    Serial.flush();
    for ( size_t i = 0; i < numWords; i++ ) {
        psramBlock[i] = 0x00000000;
    }
    for ( size_t i = 0; i < numWords; i++ ) {
        if ( psramBlock[i] != 0x00000000 ) {
            errors++;
        }
    }
    for ( size_t i = 0; i < numWords; i++ ) {
        psramBlock[i] = 0xFFFFFFFF;
    }
    for ( size_t i = 0; i < numWords; i++ ) {
        if ( psramBlock[i] != 0xFFFFFFFF ) {
            errors++;
        }
    }
    Serial.println( errors == 0 ? "PASS" : "FAIL (" + String(errors) + " errors)" );
    Serial.flush();
    
    // Speed test
    Serial.println( "\n--- Speed Comparison Test ---" );
    Serial.flush();
    
    const size_t speedTestSize = 32 * 1024; // 32KB for speed test
    size_t speedWords = speedTestSize / sizeof(uint32_t);
    
    // Allocate SRAM block for comparison
    uint32_t* sramBlock = (uint32_t*)malloc( speedTestSize );
    if ( sramBlock == nullptr ) {
        Serial.println( "Warning: Could not allocate SRAM comparison block" );
        free( psramBlock );
        Serial.flush();
        return CMD_DONT_SHOW_MENU;
    }
    
    unsigned long startTime, endTime;
    
    // PSRAM sequential write speed
    startTime = micros();
    for ( size_t i = 0; i < speedWords; i++ ) {
        psramBlock[i] = i;
    }
    endTime = micros();
    unsigned long psramWriteTime = endTime - startTime;
    float psramWriteSpeed = ( speedTestSize / 1024.0 ) / ( psramWriteTime / 1000000.0 ); // KB/s
    
    // PSRAM sequential read speed
    volatile uint32_t dummy = 0;
    startTime = micros();
    for ( size_t i = 0; i < speedWords; i++ ) {
        dummy += psramBlock[i];
    }
    endTime = micros();
    unsigned long psramReadTime = endTime - startTime;
    float psramReadSpeed = ( speedTestSize / 1024.0 ) / ( psramReadTime / 1000000.0 ); // KB/s
    
    // SRAM sequential write speed
    startTime = micros();
    for ( size_t i = 0; i < speedWords; i++ ) {
        sramBlock[i] = i;
    }
    endTime = micros();
    unsigned long sramWriteTime = endTime - startTime;
    float sramWriteSpeed = ( speedTestSize / 1024.0 ) / ( sramWriteTime / 1000000.0 ); // KB/s
    
    // SRAM sequential read speed  
    startTime = micros();
    for ( size_t i = 0; i < speedWords; i++ ) {
        dummy += sramBlock[i];
    }
    endTime = micros();
    unsigned long sramReadTime = endTime - startTime;
    float sramReadSpeed = ( speedTestSize / 1024.0 ) / ( sramReadTime / 1000000.0 ); // KB/s
    
    Serial.println( "Test block size: " + String( speedTestSize / 1024 ) + " KB" );
    Serial.println( "" );
    Serial.println( "PSRAM Write: " + String( psramWriteTime ) + " us (" + String( psramWriteSpeed / 1024, 2 ) + " MB/s)" );
    Serial.println( "PSRAM Read:  " + String( psramReadTime ) + " us (" + String( psramReadSpeed / 1024, 2 ) + " MB/s)" );
    Serial.println( "SRAM Write:  " + String( sramWriteTime ) + " us (" + String( sramWriteSpeed / 1024, 2 ) + " MB/s)" );
    Serial.println( "SRAM Read:   " + String( sramReadTime ) + " us (" + String( sramReadSpeed / 1024, 2 ) + " MB/s)" );
    Serial.println( "" );
    Serial.println( "Speed ratio (SRAM/PSRAM):" );
    Serial.println( "  Write: " + String( sramWriteSpeed / psramWriteSpeed, 2 ) + "x" );
    Serial.println( "  Read:  " + String( sramReadSpeed / psramReadSpeed, 2 ) + "x" );
    Serial.flush();
    
    // Cleanup
    free( psramBlock );
    free( sramBlock );
    
    Serial.println( "\n=== PSRAM Test Complete ===" );
    Serial.flush();
    
    // Use dummy to prevent optimizer from removing the reads
    (void)dummy;
    
    return CMD_DONT_SHOW_MENU;
}

// Helper function to strip '>' characters from the beginning of lines
static String stripLeadingArrows( const String& input ) {
    String result;
    result.reserve( input.length( ) );

    bool atLineStart = true;
    for ( size_t i = 0; i < input.length( ); i++ ) {
        char c = input[ i ];

        // Skip '>' at the beginning of lines
        if ( atLineStart && c == '>' ) {
            continue;
        }

        // Skip whitespace at line start (after we've removed '>')
        if ( atLineStart && ( c == ' ' || c == '\t' ) ) {
            continue;
        }

        result += c;

        // Track line boundaries
        if ( c == '\n' || c == '\r' ) {
            atLineStart = true;
        } else if ( c != ' ' && c != '\t' ) {
            atLineStart = false;
        }
    }

    return result;
}



CommandResult cmd_pythonCommand( char c, const String& line ) {
    // Use getCommandArgs to handle both line-buffered and char-by-char modes
    // It strips the trigger char '>' and reads from serial if needed
    String pythonCommand = getCommandArgs( line );

    // If getCommandArgs returned empty but line had content without '>' prefix, use as-is
    if ( pythonCommand.length( ) == 0 ) {
        String trimmedLine = line;
        trimmedLine.trim( );
        if ( trimmedLine.length( ) > 1 && trimmedLine[ 0 ] != '>' ) {
            pythonCommand = trimmedLine;
        }
    }

    // Strip '>' from the beginning of all lines
    pythonCommand = stripLeadingArrows( pythonCommand );
    pythonCommand.trim( );

    // Get the response target for this command (if any)
    Stream* response_target = Jerial.getResponseTarget( );

#if DEBUG_RELAYED_COMMANDS
    // Debug output - always enabled for now to track command execution
    Serial.print( "cmd_pythonCommand: Received line=[" );
    for ( size_t i = 0; i < line.length( ); i++ ) {
        char c = line[ i ];
        if ( c == '\n' )
            Serial.print( "\\n" );
        else if ( c == '\r' )
            Serial.print( "\\r" );
        else if ( c >= 32 && c < 127 )
            Serial.print( c );
        else
            Serial.printf( "<%02X>", (unsigned char)c );
    }
    Serial.print( "], extracted pythonCommand=[" );
    for ( size_t i = 0; i < pythonCommand.length( ); i++ ) {
        char c = pythonCommand[ i ];
        if ( c == '\n' )
            Serial.print( "\\n" );
        else if ( c == '\r' )
            Serial.print( "\\r" );
        else if ( c >= 32 && c < 127 )
            Serial.print( c );
        else
            Serial.printf( "<%02X>", (unsigned char)c );
    }
    Serial.printf( "] (len=%d), response_target=%p\n", pythonCommand.length( ), response_target );
    Serial.flush( );
#endif

    if ( pythonCommand.length( ) > 0 ) {
        // Execute command (output goes to USB Serial)
        // Note: response_target routing for UART is available but not used for MicroPython output
        // because MicroPython writes directly to global_mp_stream and capturing is complex

        // SAFETY: If command contains newlines, split into individual lines
        // and execute each separately. This way one bad command doesn't prevent
        // the others from running (each gets its own nlr error handler).
        if ( pythonCommand.indexOf( '\n' ) >= 0 ) {
            int lineStart = 0;
            for ( int i = 0; i <= (int)pythonCommand.length( ); i++ ) {
                if ( i == (int)pythonCommand.length( ) || pythonCommand[ i ] == '\n' ) {
                    if ( i > lineStart ) {
                        String singleLine = pythonCommand.substring( lineStart, i );
                        singleLine.trim( );
                        if ( singleLine.length( ) > 0 ) {
                            // Validate: skip lines with no printable content
                            bool hasPrintable = false;
                            for ( size_t j = 0; j < singleLine.length( ); j++ ) {
                                if ( singleLine[ j ] >= ' ' && singleLine[ j ] < 127 ) {
                                    hasPrintable = true;
                                    break;
                                }
                            }
                            if ( hasPrintable ) {
                                executeSinglePythonCommand( singleLine.c_str( ), nullptr, 0 );
                                yield( ); // pump USB + flush CDC between commands
                            }
                        }
                    }
                    lineStart = i + 1;
                }
            }
        } else {
            executeSinglePythonCommand( pythonCommand.c_str( ), nullptr, 0 );
        }

    } else {
        Jerial.println( "Usage: > <python_command>" );
    }
    Jerial.flush( );
    yield( ); // pump USB + flush CDC before return
    return CMD_DONT_SHOW_MENU;
}

// File system commands
CommandResult cmd_showFilesystem( char c, const String& line ) {
    String arg = getCommandArgs( line );


    if ( arg.length( ) > 0 ) {
        // User provided a path like "/adc_basics.py" — resolve and run it
        String resolved = resolvePythonScriptPath( arg );
        if ( resolved.length( ) > 0 ) {
            setGlobalStreamWithInterrupt( &Serial );
            runPythonScriptByPath( resolved );
        } else {
            Jerial.println( "Script not found: " + arg );
            Jerial.println( "Searched: /python_scripts, /python_scripts/examples, /python_scripts/lib, /python_scripts/modules, /" );
        }
        Jerial.flush( );
        return CMD_DONT_SHOW_MENU;
    }

    // No argument — open the file manager as before
    runApp( -1, (char*)"File Manager" );
    // Returning to the menu: resync the app to the user's line-buffering config.
    pushLineBufferingToApp( );
    return CMD_SHOW_MENU;
}

CommandResult cmd_enableUSBStorage( char c, const String& line ) {
    extern bool mscModeEnabled;
    if ( mscModeEnabled == false ) {
        Jerial.println( "Enabling USB Mass Storage drive..." );
        if ( initUSBMassStorage( ) ) {
            Jerial.println( "USB Mass Storage enabled - device will appear as 'JUMPERLESS' drive\n\r" );
            Jerial.println( "\tu = disable USB Mass Storage" );
            Jerial.println( "\tG = reload config.txt" );
            Jerial.println( "\ty = refresh connections when files change" );
            Jerial.println( "\tS = show status" );
            Jerial.println( "\n\r" );
            Jerial.flush( );
        } else {
            Jerial.println( "USB Mass Storage initialization failed" );
            Jerial.flush( );
        }
    } else {
        Jerial.println( "USB Mass Storage is already enabled" );
        printUSBMassStorageStatus( );
        refreshConnections( -1 );
        Jerial.flush( );
    }
    // Note: The USB Mass Storage loop is handled in main.cpp
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_disableUSBStorage( char c, const String& line ) {
    extern bool mscModeEnabled;
    if ( mscModeEnabled == true ) {
        Jerial.println( "Disabling USB Mass Storage drive..." );
        if ( disableUSBMassStorage( ) ) {
            Jerial.println( "USB Mass Storage disabled - device no longer appears as drive" );
            Jerial.println( "Use 'U' command to re-enable when needed" );
        } else {
            Jerial.println( "USB Mass Storage disable failed" );
        }
    } else {
        Jerial.println( "USB Mass Storage is already disabled" );
        Jerial.println( "Use 'U' command to enable" );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_listFilesystem( char c, const String& line ) {
    // Recursive, machine-readable listing via the global Python walk()
    // (defined at MicroPython init). Emits f|path|size / d|path|size lines.
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    runFilesystemWalk( target );
    return CMD_DONT_SHOW_MENU;
}

// Config commands
CommandResult cmd_editConfig( char c, const String& line ) {
    extern volatile bool core1busy;
    // core1busy = 1;
    // waitCore2();
    readConfigFromSerial( );
    // core1busy = 0;
    Jerial.flush( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_printConfig( char c, const String& line ) {
    // extern volatile bool core1busy;
    // core1busy = 1;
    // waitCore2();
    printConfigToSerial( );
    // core1busy = 0;
    Jerial.flush( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_reloadConfig( char c, const String& line ) {
    // This is the 'G' command which has special wavegen test code
    // For now, we'll just reload the config
    Jerial.println( "Reloading config.txt..." );
    extern bool configChanged;
    configChanged = true;
    return CMD_SHOW_MENU;
}

// Hardware commands
CommandResult cmd_resetArduino( char c, const String& line ) {
    String arg = getCommandArgs( line, 20 );
    if ( arg.length( ) > 0 ) {
        char ch = arg[ 0 ];
        if ( ch == '0' || ch == '2' || ch == 't' ) {
            resetArduino( 0 );
        }
        if ( ch == '1' || ch == '2' || ch == 'b' ) {
            resetArduino( 1 );
        }
    } else {
        resetArduino( );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_connectArduino( char c, const String& line ) {
    String arg = getCommandArgs( line, 20 );
    int justAsk = 0;
    if ( arg.length( ) > 0 && arg[ 0 ] == '?' ) {
        justAsk = 1;
        int isConnected = checkIfArduinoIsConnected( );
        int isPresent = checkArduinoPresence( );

        // Response format: "connection,presence"
        // connection: Y=connected, n=not connected
        // presence: Y=detected, n=not detected
        // NOTE: DC4 (0x14) in replyWithJerialInfo() is now the preferred method
        // for presence checks (faster response). This A? handler is kept for
        // backwards compatibility.
        Jerial.print( isConnected ? "Y," : "n," );
        Jerial.println( isPresent ? "Y" : "n" );
        Jerial.flush( );
    }
    if ( justAsk == 0 ) {
        connectArduino( 0 );
        Jerial.println( "UART connected to Arduino D0 and D1" );
        Jerial.flush( );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_disconnectArduino( char c, const String& line ) {
    String arg = getCommandArgs( line, 20 );
    int justAsk = 0;
    if ( arg.indexOf( '?' ) != -1 ) {
        justAsk = 1;
        if ( checkIfArduinoIsConnected( ) == 1 ) {
            Jerial.println( "Y" );
        } else {
            Jerial.println( "n" );
        }
        Jerial.flush( );
    }
    if ( justAsk == 0 ) {
        disconnectArduino( 0 );
        Jerial.println( "UART disconnected from Arduino D0 and D1" );
        Jerial.flush( );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_readADC( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;

    String arg = getCommandArgs( line, 20 );
    if ( arg.length( ) > 0 ) {
        char ch = arg[ 0 ];

        if ( isdigit( ch ) ) {
            int adc = ch - '0';
            if ( adc >= 0 && adc <= 4 ) {
                target->print( " adc" );
                target->print( adc );
                target->print( " = " );
                float adcVoltage = readAdcVoltage( adc, 32 );
                if ( adcVoltage > 0.00 ) {
                    target->print( " " );
                }
                target->println( adcVoltage );
            }
        } else if ( ch == 'i' ) {
            if ( arg.length( ) > 1 && arg[ 1 ] == '1' ) {
#if defined(OG_JUMPERLESS)
                float iSense = 0.0f;
#else
                extern INA219 INA1;
                float iSense = INA1.getCurrent_mA()- currentReadingOffset1_mA;
#endif
                target->print( "ina1 = " );
                target->print( iSense );
                target->println( "mA" );
            } else {
                extern INA219 INA0;
                float iSense = INA0.getCurrent_mA()- currentReadingOffset0_mA;
                target->print( "ina0 = " );
                target->print( iSense );
                target->print( "mA \t" );

                iSense = INA0.getBusVoltage( );
                target->print( iSense );
                target->print( "V \t" );

                iSense = INA0.getPower_mW( );
                target->print( iSense );
                target->println( "mW" );
            }
        } else if ( ch == 'l' ) {
            if ( showReadings == 1 ) {
                showReadings = 0;
                target->println( "showReadings = 0" );
            } else {
                showReadings = 1;
                target->println( "showReadings = 1" );
            }
            chooseShownReadings( );
        }
        target->flush( );
    } else {
        target->println( );
        for ( int i = 0; i < 5; i++ ) {
            target->print( "adc" );
            target->print( i );
            target->print( " = " );
            float adcVoltage = readAdcVoltage( i, 32 );
            if ( adcVoltage > 0.00 ) {
                target->print( " " );
            }
            target->println( adcVoltage );
        }
        target->print( "probe = " );
        float probeVoltage = readAdcVoltage( 7, 32 );
        if ( probeVoltage > 0.00 ) {
            target->print( " " );
        }
        target->println( probeVoltage );
    }
    target->flush( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_setDAC( char c, const String& line ) {
    // probePowerDAC is defined in Probing.h as int&
    extern bool configChanged;

    char f[ 8 ] = { ' ' };
    int index = 0;
    float f1 = 0.0;
    unsigned long timer = millis( );
    while ( Jerial.available( ) == 0 && millis( ) - timer < 1000 ) {
    }
    while ( index < 8 ) {
        f[ index ] = Jerial.read( );
        index++;
    }

    f1 = atof( f );
    // The non-probe DAC is always DAC1 now: DAC0 is the probe feed (the
    // only path INA1's switch sensing can see) and never the user's here.
    setDac1voltage( f1, 1, 1 );
    configChanged = true;
    Jerial.printf( "DAC 1 = %0.2f V\n", f1 );
    Jerial.flush( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_i2cScan( char c, const String& line ) {
    Jerial.flush( );

    String input = getCommandArgs( line, 100 );
    if ( input.length( ) > 0 ) {
        if ( input.indexOf( 'i' ) != -1 ) {
            i2cScan( 0, 0, 26, 27, 1, 1 );
            return CMD_DONT_SHOW_MENU;
        }
        if ( input.indexOf( ',' ) != -1 ) {
            // Format: @5,10 - SDA at row 5, SCL at row 10
            int commaIndex = input.indexOf( ',' );
            int sdaRow = input.substring( 0, commaIndex ).toInt( );
            int sclRow = input.substring( commaIndex + 1 ).toInt( );

            changeTerminalColor( 69, true );
            Jerial.print( "I2C scan with SDA=" );
            Jerial.print( sdaRow );
            Jerial.print( ", SCL=" );
            Jerial.println( sclRow );
            changeTerminalColor( 38, true );

            if ( i2cScan( sdaRow, sclRow, 26, 27, 1 ) > 0 ) {
                Jerial.println( "Found devices" );
                return CMD_DONT_SHOW_MENU;
            } else {
                removeBridgeFromState( RP_GPIO_26, sdaRow, true );
                removeBridgeFromState( RP_GPIO_27, sclRow, true );
            }
        } else if ( input.length( ) > 0 && isdigit( input[ 0 ] ) ) {
            // Format: @5 - try all 4 combinations around row 5
            int baseRow = input.toInt( );

            changeTerminalColor( 69, true );
            Jerial.print( "I2C scan trying all combinations around row " );
            Jerial.println( baseRow );
            changeTerminalColor( 38, true );

            int combinations[ 4 ][ 2 ] = {
                { baseRow, baseRow + 1 },
                { baseRow + 1, baseRow },
                { baseRow, baseRow - 1 },
                { baseRow - 1, baseRow } };

            for ( int i = 0; i < 4; i++ ) {
                int sdaRow = combinations[ i ][ 0 ];
                int sclRow = combinations[ i ][ 1 ];

                changeTerminalColor( 202, true );
                Jerial.print( "\nTrying SDA=" );
                Jerial.print( sdaRow );
                Jerial.print( ", SCL=" );
                Jerial.print( sclRow );
                Jerial.println( ":" );
                changeTerminalColor( 38, true );
                int devicesFound = i2cScan( sdaRow, sclRow, 26, 27, 0 );
                if ( devicesFound > 0 ) {
                    changeTerminalColor( 199, true );
                    Jerial.printf( "\n\rfound %d devices: SDA at row %d, SCL at row %d\n\r",
                                   devicesFound, sdaRow, sclRow );
                    changeTerminalColor( -1 );
                    return CMD_DONT_SHOW_MENU;
                }
                delay( 1 );
            }
        }
    } else {
        // Interactive mode
        Jerial.print( "Enter SDA row: " );
        Jerial.flush( );
        while ( Jerial.available( ) == 0 ) {
        }
        int rowSDA = Jerial.parseInt( );
        Jerial.print( "Enter SCL row: " );
        Jerial.flush( );
        while ( Jerial.available( ) == 0 ) {
        }
        int rowSCL = Jerial.parseInt( );

        changeTerminalColor( 69, true );
        Jerial.print( "I2C scan with SDA=" );
        Jerial.print( rowSDA );
        Jerial.print( ", SCL=" );
        Jerial.println( rowSCL );
        changeTerminalColor( 38, true );

        if ( i2cScan( rowSDA, rowSCL, 26, 27, 1 ) > 0 ) {
            // Success
        } else {
            removeBridgeFromState( RP_GPIO_26, rowSDA, true );
            removeBridgeFromState( RP_GPIO_27, rowSCL, true );
        }
    }

    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_calibrateDACs( char c, const String& line ) {
    // dacSpread and dacZero are defined in Peripherals.h
    extern float dacSpread[ 4 ];
    extern int dacZero[ 4 ];

    for ( int d = 0; d < 4; d++ ) {
        Jerial.print( "dacSpread[" );
        Jerial.print( d );
        Jerial.print( "] = " );
        Jerial.println( dacSpread[ d ] );
    }

    for ( int d = 0; d < 4; d++ ) {
        Jerial.print( "dacZero[" );
        Jerial.print( d );
        Jerial.print( "] = " );
        Jerial.println( dacZero[ d ] );
    }

    calibrateDacs( );
    return CMD_SHOW_MENU;
}

// Debug commands
CommandResult cmd_showVersion( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    target->print( "Jumperless firmware version: " );
    target->println( firmwareVersion );
    target->flush( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_setDebugFlags( char c, const String& line ) {
    debugFlagsMenu( );
    return CMD_SHOW_MENU;
}

CommandResult cmd_statusDiagnosticsMenu( char c, const String& line ) {
    statusDiagnosticsMenu( );
    return CMD_SHOW_MENU;
}



const char* pinNames[48] = {
    "UART_Tx",
    "UART_Rx",
    "LED_PROBE",
    "LED_TOP",
    "I2C0_SDA",
    "I2C0_SCL",
    "RP6",
    "RP7",
    "LDAC",
    "PROBE_BUTTON",
    "PROBE_PROBE",
    "ENC_PUSH",
    "ENC_A",
    "ENC_B",
    "CH_DATA",
    "CH_CLK",
    "CH_RESET",
    "LED_BB",
    "NANO_RESET_0",
    "NANO_RESET_1",
    "GPIO_1",
    "GPIO_2",
    "GPIO_3",
    "GPIO_4",
    "GPIO_5",
    "GPIO_6",
    "GPIO_7",
    "GPIO_8",
    "CH_CS_A",
    "CH_CS_B",
    "CH_CS_C",
    "CH_CS_D",
    "CH_CS_E",
    "CH_CS_F",
    "CH_CS_G",
    "CH_CS_H",
    "CH_CS_I",
    "CH_CS_J",
    "CH_CS_K",
    "CH_CS_L",
    "ADC_0",
    "ADC_1",
    "ADC_2",
    "ADC_3",
    "ADC_4_5V",
    "PROBE_PAD_SENS",
    "SUPPLY_MONITOR",
    "ADC_PROBE"
};
const char* PSRAM_CS = "PSRAM_CS";




// ============================================================================
// Phase 5 - PSRAM / undo / file-cache diagnostic commands
// ============================================================================
// (Undo.h moved up to the main include block - it's used by cmd_cycleSlots
// for slot-aware undo history. Keeping these here for the diagnostic
// commands below.)
#include "PsramArena.h"
#include "FileCache.h"

CommandResult cmd_undo( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    // Capture label BEFORE the step so OLED + serial show the same thing.
    char lblBuf[ 32 ];
    lblBuf[ 0 ] = '\0';
    int lblSplit = -1;
    if ( undoCanUndo( ) ) {
        strncpy( lblBuf, undoPeekUndoLabel( ), sizeof( lblBuf ) - 1 );
        lblBuf[ sizeof( lblBuf ) - 1 ] = '\0';
        lblSplit = undoLabelSplitAt( 0 );
    }
    if ( undoUndo( ) ) {
        target->printf( "\r[Undo] reverted: %s  (position %d / %d)\n",
                        lblBuf[ 0 ] ? lblBuf : "(unlabeled)",
                        undoPosition( ), undoTotalTxns( ) );
        undoToast( false, lblBuf, lblSplit );
    } else {
        target->println( "\r[Undo] nothing to undo" );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_redo( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    char lblBuf[ 32 ];
    lblBuf[ 0 ] = '\0';
    int lblSplit = -1;
    if ( undoCanRedo( ) ) {
        strncpy( lblBuf, undoPeekRedoLabel( ), sizeof( lblBuf ) - 1 );
        lblBuf[ sizeof( lblBuf ) - 1 ] = '\0';
        lblSplit = undoLabelSplitAt( +1 );
    }
    if ( undoRedo( ) ) {
        target->printf( "\r[Redo] reapplied: %s  (position %d / %d)\n",
                        lblBuf[ 0 ] ? lblBuf : "(unlabeled)",
                        undoPosition( ), undoTotalTxns( ) );
        undoToast( true, lblBuf, lblSplit );
    } else {
        target->println( "\r[Redo] nothing to redo" );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_historyStatus( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    target->println( "\n\r=== Undo Log Status ===" );
    undoDumpStatus( );
    int total = undoTotalTxns( );
    int pos = undoPosition( );
    target->printf( "  position %d (head=0)  total %d  canUndo=%d canRedo=%d\n",
                    pos, total, (int)undoCanUndo( ), (int)undoCanRedo( ) );
    int show = ( total < 8 ) ? total : 8;
    target->println( "  recent transactions:" );
    for ( int i = 0; i < show; i++ ) {
        const char* lbl = undoLabelAt( -i );
        target->printf( "    [%d] %s\n", -i, lbl );
    }
    return CMD_DONT_SHOW_MENU;
}

// Print the FULL history ring (every transaction in the undo log), with a
// ">" marker on the current cursor position. Older txns at the top, newest
// at the bottom; the marker shows where new mutations would be appended.
//
// undoLabelAt uses cursor-relative offsets where 0 = txn just behind the
// cursor (the next undo target). To walk the entire ring we need offsets
// from the future side too: positive = txns ahead of cursor (redo land),
// negative = txns behind cursor (undo land).
CommandResult cmd_historyList( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;

    int total = undoTotalTxns( );
    int pos = undoPosition( );  // 0 = at head; negative = undone

    target->printf( "\r\n=== Undo History (%d txns, cursor at %d) ===\n",
                    total, pos );
    if ( total == 0 ) {
        target->println( "  (empty)" );
        return CMD_DONT_SHOW_MENU;
    }

    // Walk from oldest to newest: undoLabelAt(-back) where back counts
    // from cursor-1 (most recent past). The full range is:
    //   back = (total - 1 + pos) ... 0      <- past (undoable)
    //   back = -1 ... pos                   <- future (redoable)
    //
    // Easier to express: for txnIdx from 0 (oldest) to total-1 (newest),
    // relative offset = txnIdx - (total - 1 + pos).
    //
    // The cursor "lives between" txn (total-1+pos) and (total+pos) when
    // viewed in the global txnIdx scale.
    int cursorTxnIdx = total - 1 + pos;  // index of the last past txn

    for ( int i = 0; i < total; i++ ) {
        int rel = i - cursorTxnIdx;  // 0 = next undo target; +1 = next redo
        const char* lbl = undoLabelAt( rel );
        if ( !lbl || !lbl[ 0 ] ) lbl = "(unlabeled)";
        char marker = ' ';
        if ( i == cursorTxnIdx ) marker = '>';
        // Position label in user terms (negative = into past, 0 = head):
        //   txn at cursorTxnIdx = position 0 - (total - 1 - cursorTxnIdx)
        // Simpler: pos value for THIS txn = (i - (total-1)).
        int displayPos = i - ( total - 1 );
        target->printf( " %c [%4d] %s\n", marker, displayPos, lbl );
    }
    target->printf( "       (cursor; redo land below, undo land above)\n" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_psramStatus( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    target->println( "\n\r=== PSRAM / FileCache / Undo Status ===" );
    psram_arena_dump_status( );
    fileCacheDumpStatus( );
    undoDumpStatus( );
    target->printf( "psram_debug = %d  (toggle with %% command)\n", psram_debug );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_psramDebugToggle( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;
    psram_debug = !psram_debug;
    target->printf( "\rpsram_debug = %d\n", psram_debug );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_netCurrents( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;

    String arg = getCommandArgs( line, 100 );
    if ( arg.length( ) > 0 ) {
        if ( arg[ 0 ] == '!' ) {
            printNetVoltageScanStats( target );
            return CMD_DONT_SHOW_MENU;
        }
        if ( arg[ 0 ] == '?' ) {
            routeSafetySelfCheck( target );
            return CMD_DONT_SHOW_MENU;
        }
        if ( arg[ 0 ] == '#' ) {
            sendXYrawCheckEnabled = !sendXYrawCheckEnabled;
            target->printf( "\rsendXYraw short-check %s (blocked=%lu)\n\r",
                            sendXYrawCheckEnabled ? "on" : "off",
                            (unsigned long)sendxy_blocked_count );
            return CMD_DONT_SHOW_MENU;
        }
        if ( arg[ 0 ] == '@' ) {
            target->println( "\r[infra] function     en   candidate / pairs" );
            infraPrintStatus( target );
            target->printf( "[infra] droop ohms: %.1f (%s)\n\r",
                            (double)infraProbeDroopOhms( ),
                            jumperlessConfig.calibration.probe_droop_ohms > 0.0f
                                ? "calibrated" : "computed" );
            // The feed's set-once proof: DAC0 (A) writes must not move across
            // rebuilds that don't change its voltage.
            target->print( "[infra] " );
            MCP4728::printWriteStats( target );
            return CMD_DONT_SHOW_MENU;
        }
    }

    bool enable = !jumperlessConfig.display.net_currents;
    // Applies immediately and persists to /config.txt
    updateConfigValue( "display", "net_currents", enable ? "1" : "0" );
    target->printf( "\rnet current scan %s\n\r", enable ? "on" : "off" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_resourceStatus( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;

    // "X!" resets the watchdog kick-gap maxima (measure-only stage, T1.6) so
    // one blocker at a time can be timed; everything else in X is cumulative.
    {
        String arg = getCommandArgs( line, 20 );
        if ( arg.length( ) > 0 && arg[ 0 ] == '!' ) {
            kickGapReset( );
            xbarLatReset( );
            encoderDriftReset( );
            {
                extern void probeDblStatsReset( void );
                probeDblStatsReset( );
            }
            target->println( "kick-gap maxima, crossbar-latency, encoder-drift and double-tap stats reset" );
            return CMD_DONT_SHOW_MENU;
        }
    }

    target->println( "\n\r╭──────────────────────────────────────────────────────────────────────╮" );
    target->println( "│                      SYSTEM RESOURCE STATUS                          │" );
    target->println( "╰──────────────────────────────────────────────────────────────────────╯\n\r" );

    target->println( "┌──────────────────────────────────┬───────────────────────────────────┐" );
    target->println( "│         SRAM MEMORY (Heap)       │         PSRAM MEMORY              │" );
    target->println( "├──────────────────────────────────┼───────────────────────────────────┤" );
    
    size_t sramTotal = rp2040.getTotalHeap( );
    size_t sramFree = rp2040.getFreeHeap( );
    size_t sramUsed = sramTotal - sramFree;
    int sramPercent = ( sramUsed * 100 ) / sramTotal;
    
    size_t psramSize = rp2040.getPSRAMSize( );
    size_t psramTotal = 0, psramFree = 0, psramUsed = 0;
    int psramPercent = 0;
    bool hasPSRAM = ( psramSize > 0 && jumperlessConfig.hardware.psram_installed == 1 );
    
    if ( hasPSRAM ) {
        psramTotal = rp2040.getTotalPSRAMHeap( );
        psramFree = rp2040.getFreePSRAMHeap( );
        psramUsed = rp2040.getUsedPSRAMHeap( );
        psramPercent = psramTotal > 0 ? ( psramUsed * 100 ) / psramTotal : 0;
    }
    
    if ( hasPSRAM ) {
        target->printf( "│ Total: %6u KB (%6u bytes)  │ Chip:     %4u MB (%7u bytes) │\n\r",
                       (unsigned)(sramTotal / 1024), (unsigned)sramTotal,
                       (unsigned)(psramSize / 1024 / 1024), (unsigned)psramSize );
        target->printf( "│ Free:  %6u KB (%6u bytes)  │ Total: %6u KB                  │\n\r",
                       (unsigned)(sramFree / 1024), (unsigned)sramFree,
                       (unsigned)(psramTotal / 1024) );
        target->printf( "│ Used:  %6u KB (%3d%%)          │ Free:  %6u KB (%3d%% used)      │\n\r",
                       (unsigned)(sramUsed / 1024), sramPercent,
                       (unsigned)(psramFree / 1024), psramPercent );
    } else {
        target->printf( "│ Total: %6u KB (%6u bytes)  │ Not installed                     │\n\r",
                       (unsigned)(sramTotal / 1024), (unsigned)sramTotal );
        target->printf( "│ Free:  %6u KB (%6u bytes)  │ Config: psram_installed=%d         │\n\r",
                       (unsigned)(sramFree / 1024), (unsigned)sramFree,
                       jumperlessConfig.hardware.psram_installed );
        target->printf( "│ Used:  %6u KB (%3d%%)          │                                   │\n\r",
                       (unsigned)(sramUsed / 1024), sramPercent );
    }
    
    target->println( "└──────────────────────────────────┴───────────────────────────────────┘" );

    target->println( "\n\r┌──────────────────────────────────┬───────────────────────────────────┐" );
    target->println( "│         OLED DISPLAY             │           PIO STATUS              │" );
    target->println( "├──────────────────────────────────┼───────────────────────────────────┤" );
    
    const char* connTypes[] = { "GPIO 7/8 (crossbar)", "RP6/RP7 (hardwired)", "I2C0 (internal)", "Custom" };
    int connType = jumperlessConfig.top_oled.connection_type;
    const char* connName = (connType >= 0 && connType <= 3) ? connTypes[connType] : "Unknown";
    
    // "@0" / "@16" = the block's GPIOBASE (RP2350B: which 32 of the 48 pins
    // it can reach; PIO2 is @16 for the crossbar chip-select strobe, T3.2).
    target->printf( "│ Status: %-23s  │ PIO0@%-2u: SM0:%s SM1:%s SM2:%s SM3:%s  │\n\r",
                   oled.isConnected( ) ? "Connected" : "Not connected",
                   (unsigned)pio_get_gpio_base( pio0 ),
                   pio_sm_is_claimed( pio0, 0 ) ? "●" : "○",
                   pio_sm_is_claimed( pio0, 1 ) ? "●" : "○",
                   pio_sm_is_claimed( pio0, 2 ) ? "●" : "○",
                   pio_sm_is_claimed( pio0, 3 ) ? "●" : "○" );
    
    target->printf( "│ Type: %-25s  │ PIO1@%-2u: SM0:%s SM1:%s SM2:%s SM3:%s  │\n\r",
                   connName,
                   (unsigned)pio_get_gpio_base( pio1 ),
                   pio_sm_is_claimed( pio1, 0 ) ? "●" : "○",
                   pio_sm_is_claimed( pio1, 1 ) ? "●" : "○",
                   pio_sm_is_claimed( pio1, 2 ) ? "●" : "○",
                   pio_sm_is_claimed( pio1, 3 ) ? "●" : "○" );
    
    // PIO2 only exists on RP2350; RP2040 (OG) has just pio0/pio1.
#if defined(PICO_RP2350)
    target->printf( "│ SDA: GPIO %2d  SCL: GPIO %2d       │ PIO2@%-2u: SM0:%s SM1:%s SM2:%s SM3:%s  │\n\r",
                   jumperlessConfig.top_oled.sda_pin,
                   jumperlessConfig.top_oled.scl_pin,
                   (unsigned)pio_get_gpio_base( pio2 ),
                   pio_sm_is_claimed( pio2, 0 ) ? "●" : "○",
                   pio_sm_is_claimed( pio2, 1 ) ? "●" : "○",
                   pio_sm_is_claimed( pio2, 2 ) ? "●" : "○",
                   pio_sm_is_claimed( pio2, 3 ) ? "●" : "○" );
#else
    target->printf( "│ SDA: GPIO %2d  SCL: GPIO %2d       │ PIO2: n/a (RP2040)                │\n\r",
                   jumperlessConfig.top_oled.sda_pin,
                   jumperlessConfig.top_oled.scl_pin );
#endif
    
    if ( jumperlessConfig.top_oled.sda_row >= 0 ) {
        target->printf( "│ SDA Row: %3s  SCL Row: %3s       │                                   │\n\r",
                       definesToChar(jumperlessConfig.top_oled.sda_row, 0),
                       definesToChar(jumperlessConfig.top_oled.scl_row, 0));
    }
    
    target->println( "└──────────────────────────────────┴───────────────────────────────────┘" );

    target->println( "\n\rgpio  up dn  func      hex  name            gpio  up dn  func      hex  name" );
    target->println(     "────  ─────  ────────  ───  ────────────    ────  ─────  ────────  ───  ────────────" );
    
    for ( int row = 0; row < 24; row++ ) {
        int gpio1 = row;
        int gpio2 = row + 24;
        
        uint32_t pad1 = pads_bank0_hw->io[gpio1];
        uint32_t pad2 = pads_bank0_hw->io[gpio2];
        
        bool up1 = gpio_is_pulled_up(gpio1);
        bool dn1 = gpio_is_pulled_down(gpio1);
        bool up2 = gpio_is_pulled_up(gpio2);
        bool dn2 = gpio_is_pulled_down(gpio2);
        
        gpio_function_t func1 = gpio_get_function( gpio1 );
        gpio_function_t func2 = gpio_get_function( gpio2 );
        const char* funcName1 = gpio_function_name_for_pin( gpio1, func1 );
        const char* funcName2 = gpio_function_name_for_pin( gpio2, func2 );
        
        target->printf( "%4d   %c  %c  %-8s  %-2X   %-14s  %4d   %c  %c  %-8s  %-2X   %-14s\n\r",
                       gpio1,
                       up1 ? '^' : ' ',
                       dn1 ? 'v' : ' ',
                       funcName1,
                       (int)func1,
                       (gpio1 == 19 && jumperlessConfig.hardware.psram_installed == 1) ? "PSRAM_CS" : pinNames[gpio1],
                       gpio2,
                       up2 ? '^' : ' ',
                       dn2 ? 'v' : ' ',
                       funcName2,
                       (int)func2,
                       pinNames[gpio2] );
    }
    
    target->println( "\n\r          ^ = pull-up  v = pull-down     ● = claimed, ○ = free\n\r" );

    // The shared-IRQ handler chain is a fixed 6-slot pool in the prebuilt SDK
    // and the last thing to ask for one is silently declined (src/IrqSlots.cpp),
    // so show who holds them - and whether the flash-write park is ours.
    jlIrqSlotsDump( *target );
    target->printf( "flash-write park: %s (timeouts %lu)\n\r",
                    flashParkActive( ) ? "FlashPark (src/FlashPark.cpp)" : "arduino-pico idleOtherCore()",
                    (unsigned long) flashParkTimeouts( ) );
    // MCP4728 I2C traffic per channel (A=DAC0 B=DAC1 C=top D=bottom): the
    // probe feed re-parks DAC0 on every rebuild, so "writes A" flat across
    // connects/disconnects is the set-once guarantee holding.
    MCP4728::printWriteStats( target );
    // CH446Q list send (T2.3): DMA-fed on core 1 when a channel was claimed;
    // pio timeouts = the per-crosspoint handshake timeouts (CPU path) plus
    // DMA stalls (each stall also marks the unsent chips suspect).
    {
        uint32_t sends = 0, words = 0, stalls = 0, maxWords = 0;
        bool en = false;
        ch446qDmaStats( &sends, &words, &stalls, &maxWords, &en );
        extern int ch446q_timeout_count;
        target->printf( "ch446q list send: %s  dma sends %lu  words %lu (max %lu per send)  dma stalls %lu  pio timeouts %d\n\r",
                        en ? "DMA (core 1)" : "CPU", (unsigned long)sends, (unsigned long)words,
                        (unsigned long)maxWords, (unsigned long)stalls, ch446q_timeout_count );
    }
    // T3.2: the chip-select strobe SM on PIO2 (base 16) - one completion IRQ
    // per list, no ISR per crosspoint. "fallback" = the legacy ISR strobe is
    // in use; the reason says why. Then where the probe LED/button SM landed
    // (the layout needs it on PIO0 - see LEDs.cpp) and whether its button
    // program fit.
    {
        int csSm = -1, why = 0;
        uint32_t listIrqs = 0, singles = 0, singleTo = 0;
        ch446qCsStrobeInfo( &csSm, &why, &listIrqs, &singles, &singleTo );
        static const char* const whyStr[] = { "active", "PIO2 already had a program (base stuck at 0)",
                                              "no PIO2 instruction room", "no free PIO2 SM",
                                              "no DMA channel", "SM config refused", "not built (OG)" };
        if ( why < 0 || why > 6 ) why = 0;
        int probePio = ( probeLEDs.getPIO( ) != nullptr ) ? (int)pio_get_index( probeLEDs.getPIO( ) ) : -1;
        int btn = probeButtonPioState( );
#if defined(PICO_RP2350)
        if ( csSm >= 0 ) {
            int csBlk = ch446qCsStrobeBlock( );
            target->printf( "cs strobe: PIO%d SM%d (base %u), one IRQ per list: %lu  singles %lu (timeouts %lu)  |  probe LED SM: PIO%d  button: %s\n\r",
                            csBlk, csSm, (unsigned)pio_get_gpio_base( PIO_INSTANCE( csBlk ) ), (unsigned long)listIrqs, (unsigned long)singles,
                            (unsigned long)singleTo, probePio,
                            btn == 1 ? "PIO" : ( btn == 2 ? "CPU (no PIO room!)" : "not tried" ) );
        } else
#endif
        {
            target->printf( "cs strobe: FALLBACK to the legacy ISR strobe - %s  |  probe LED SM: PIO%d  button: %s\n\r",
                            whyStr[ why ], probePio,
                            btn == 1 ? "PIO" : ( btn == 2 ? "CPU (no PIO room!)" : "not tried" ) );
        }
    }
    // T2.1: the always-on ADC ring - engine state, sweep count, IRQ blocks,
    // overruns/resyncs, reader stats. "LEGACY" = the START_ONCE burst path
    // (the D-menu A/B toggle, or the engine declined at boot - reason shown).
    {
        AdcRingStats rs; adcRingGetStats( &rs );
        if ( rs.active ) {
            target->printf( "adc ring: ACTIVE  48 kHz x 8 ch, DMA ch %lu, gen %lu, sweeps %lu, block irqs %lu, overruns %lu resyncs %lu, reads %lu, waits max %lu us stalls %lu\n\r",
                            (unsigned long)rs.dmaA, (unsigned long)rs.generation, (unsigned long)rs.sweeps,
                            (unsigned long)rs.blockIrqs, (unsigned long)rs.overruns, (unsigned long)rs.resyncs,
                            (unsigned long)rs.reads, (unsigned long)rs.maxWaitUs, (unsigned long)rs.stalls );
        } else {
            static const char* const why[] = { "toggled off (D menu)", "no DMA channel", "DMA_IRQ_1 taken", "no spinlock", "not built (OG)" };
            int r = ( rs.failReason >= 0 && rs.failReason <= 4 ) ? rs.failReason : 0;
            target->printf( "adc ring: LEGACY START_ONCE path (%s)  gen %lu  reads so far %lu\n\r",
                            why[ r ], (unsigned long)rs.generation, (unsigned long)rs.reads );
        }
    }
    // T3.3: the wavegen stream - DMA (image + address ring + pacing timer,
    // core 1 free) or the legacy core-1 loop; plan and health.
    {
        extern WaveGen wavegen;
        WaveGenDmaStatus w;
        wavegen.getDmaStatus( &w );
        static const char* const wgWhy[] = { "ok", "Wire is not I2C0", "no DMA channels", "no DMA timer", "no memory", "not built (OG)" };
        int why = ( w.fallbackReason <= 5 ) ? w.fallbackReason : 0;
        if ( w.dmaAvailable ) {
            target->printf( "wavegen: DMA%s%s  f req %.4f Hz -> %.4f Hz (N %lu, tick %.1f Hz = clk*%u/%u/%lu; bus cap %.0f sps)  laps %lu  tx aborts %lu  starts %lu stops %lu restarts %lu  stop wait max %lu us  wedge recoveries %lu  abort timeouts %lu  bus yields %lu (max %lu us, forced resumes %lu)\n\r",
                            w.streaming ? " STREAMING" : " idle", w.wedged ? " WEDGED" : "",
                            (double)wavegen.getFrequency( ), (double)w.actualHz, (unsigned long)w.tableSize,
                            (double)w.tickHz, (unsigned)w.timerX, (unsigned)w.timerY, (unsigned long)w.divider,
                            (double)w.capacityHz, (unsigned long)w.laps, (unsigned long)w.txAborts,
                            (unsigned long)w.starts, (unsigned long)w.stops, (unsigned long)w.restarts,
                            (unsigned long)w.stopWaitMaxUs, (unsigned long)w.wedgeRecoveries, (unsigned long)w.abortTimeouts,
                            (unsigned long)w.busYields, (unsigned long)w.busYieldMaxUs, (unsigned long)w.busForcedResumes );
        } else if ( why == 0 ) {
            target->println( "wavegen: not initialised yet (begin() runs on the first wavegen_start; the DMA claim happens then)" );
        } else {
            target->printf( "wavegen: LEGACY core-1 loop (%s)%s  f req %.4f Hz  writes ok %lu failed %lu\n\r",
                            wgWhy[ why ], wavegen.isRunning( ) ? " running" : "",
                            (double)wavegen.getFrequency( ), (unsigned long)wavegen.getSuccessfulWrites( ),
                            (unsigned long)wavegen.getFailedWrites( ) );
        }
    }
    // probe_current_zero calibration diagnostics (see Probing.cpp ProbeZeroDiag).
    {
        target->printf( "probe zero: %.2f mA (samples %d: %.2f..%.2f, led-off ack %lu ms, xbar idle %s, at %lu s, runs %lu)  live %.2f mA\n\r",
                        (double)probeZeroDiag.zero_mA, probeZeroDiag.goodSamples,
                        (double)probeZeroDiag.sampleMin_mA, (double)probeZeroDiag.sampleMax_mA,
                        (unsigned long)probeZeroDiag.ledOffAckMs, probeZeroDiag.xbarIdleBeforeSampling ? "yes" : "NO",
                        (unsigned long)( probeZeroDiag.atMs / 1000 ), (unsigned long)probeZeroDiag.runs,
                        (double)jumperlessConfig.calibration.probe_current_zero );
    }
    // Probe LED / button line: frames vs colour requests (shows >> requests
    // is the shared-GPIO9 constant re-send), button samples decoded.
    extern volatile uint32_t ledFrameAbortsPause; // main.cpp
    target->printf( "probe led frames %lu (requests %lu)  button samples %lu  led-frame aborts(pause) %lu  uptime %lus\n\r",
                    (unsigned long)probeLedShowCount, (unsigned long)probeLedRequestCount,
                    (unsigned long)probeButtonPIOReadCount, (unsigned long)ledFrameAbortsPause,
                    (unsigned long)( millis( ) / 1000 ) );
    // C5 (task #26): merged LED+button program state. In merged mode the PIO
    // streams frames itself (~2.4 kHz - button samples run ~2.4x the legacy
    // rate, that is expected, not an anomaly) and "puts" counts deduped
    // colour CHANGES, not frames.
    {
        extern volatile bool g_probeMergedActive;
        extern volatile uint32_t probeLedPutCount;
        target->printf( "probe led mode: %s  colour puts %lu\n\r",
                        g_probeMergedActive ? "MERGED (pulses ride the frame tail)" : "legacy swap",
                        (unsigned long)probeLedPutCount );
    }
    // T3.1 M3: what the pump steals between probe ticks (last session).
    // The milestone-4 decision (full scheduler vs session-lite between
    // ticks) gets made from this number - watch max during real probing.
    {
        extern volatile uint32_t probeTickGapMaxUs, probeTickGapAvgUs, probeTickCount;
        if ( probeTickCount > 0 ) {
            target->printf( "probe tick gap (last session): max %lu us  avg %lu us  over %lu ticks\n\r",
                            (unsigned long)probeTickGapMaxUs, (unsigned long)probeTickGapAvgUs,
                            (unsigned long)probeTickCount );
        }
    }
    // C7: the quadrature count + sampler/DMA status. X! resets nearOverruns.
    printEncoderC7Line( *target );
    // Task #30's instrument: where every registered firmware PIO program
    // actually sits (block@base, used words, offsets, SM) - measured, not
    // inferred from the docs' arithmetic.
    {
        extern void pioRegistryPrint( Stream& target );
        pioRegistryPrint( *target );
    }
    // Probe double-tap failure modes (X! resets): armed/confirmed says how
    // many second taps reached the confirm gate and survived it; expired =
    // the confirm gate ate one; noRelease = the two taps had no 30ms clean
    // release between them (cannot pair, history dropped); historyWipes = a
    // consumer cleared state while a first tap was waiting for its pair.
    {
        extern volatile uint32_t dblCandArmed, dblCandConfirmed, dblCandExpired,
            dblCandOppCancel, dblEdgeNoRelease, dblHistoryWipes, dblEdgeSuppressed;
        target->printf( "probe double-tap: armed %lu confirmed %lu expired %lu oppCancel %lu | edges: noRelease %lu suppressed %lu | historyWipes %lu\n\r",
                        (unsigned long)dblCandArmed, (unsigned long)dblCandConfirmed,
                        (unsigned long)dblCandExpired, (unsigned long)dblCandOppCancel,
                        (unsigned long)dblEdgeNoRelease, (unsigned long)dblEdgeSuppressed,
                        (unsigned long)dblHistoryWipes );
    }

    // Scheduler table: one row per registered service, in the order the
    // walk visits them (priority, then registration). period 0 = every pass.
    // runs counts EVERY service() call the scheduler made - serviceAll(),
    // the modal loops' serviceInner(), forceServiceBy* - so the modal
    // share is in here too. avg = total/runs; share = total time in the
    // service over uptime (core 0 CPU spent there); overruns = calls that
    // took longer than the period (period > 0 only). Counters wrap at 2^32.
    // '*' after prio = in the inner set (keeps running inside the modal
    // loops and while a BLOCKING service holds the loop).
    {
        uint64_t upUs = time_us_64( );
        target->printf( "\n\rscheduler: %lu passes, %u services   (* = inner set: runs inside modal loops)\n\r",
                        (unsigned long)jOS.getLoopCount( ), (unsigned)jOS.getServiceCount( ) );
        target->println( "  service           prio   period_us       runs  last_us   max_us   avg_us  overruns  share" );
        for ( uint8_t i = 0; i < jOS.getServiceCount( ); i++ ) {
            Service* s = jOS.getServiceAt( i );
            if ( s == nullptr ) continue;
            const char* prio = "?";
            switch ( s->getPriority( ) ) {
                case ServicePriority::CRITICAL: prio = "CRIT"; break;
                case ServicePriority::HIGH:     prio = "HIGH"; break;
                case ServicePriority::NORMAL:   prio = "NORM"; break;
                case ServicePriority::LOW:      prio = "LOW";  break;
            }
            unsigned long avg = s->runs ? (unsigned long)( s->totalUs / s->runs ) : 0;
            double share = upUs ? ( 100.0 * (double)s->totalUs / (double)upUs ) : 0.0;
            target->printf( "  %-17s %-4s%c %10lu %10lu %8lu %8lu %8lu %9lu %5.1f%%\n\r",
                            s->getName( ), prio, s->inInnerSet( ) ? '*' : ' ', (unsigned long)s->periodUs( ),
                            (unsigned long)s->runs, (unsigned long)s->lastUs,
                            (unsigned long)s->maxUs, avg, (unsigned long)s->overruns, share );
        }
    }
    // Watchdog measure-only stage (T1.6): the longest gap between the points
    // where a watchdog kick would go, per core. X! resets the maxima.
    target->println( "\r" );
    kickGapPrint( target );
    // Tap -> request -> send -> LEDs latency probe (T2.2 / T2.3 gate).
    target->println( "\r" );
    xbarLatPrint( target );
    // Core 0 -> core 1 request mailbox (T2.2b): pending bits / request gen /
    // done gen per slot. Idle = bits 0 and done == req.
    {
        uint32_t b0, r0, d0, b1, r1, d1;
        core1req::snapshot( core1req::REQ_SEND, &b0, &r0, &d0 );
        core1req::snapshot( core1req::REQ_BYPASS, &b1, &r1, &d1 );
        uint32_t b2 = 0, r2 = 0, d2 = 0;
        core1req::snapshot( core1req::REQ_SHOW_LEDS, &b2, &r2, &d2 );
        target->printf( "core1 mailbox: send bits 0x%lx req %lu done %lu   bypass bits 0x%lx req %lu done %lu   leds bits 0x%lx req %lu done %lu%s   %s\n\r",
                        (unsigned long)b0, (unsigned long)r0, (unsigned long)d0,
                        (unsigned long)b1, (unsigned long)r1, (unsigned long)d1,
                        (unsigned long)b2, (unsigned long)r2, (unsigned long)d2, ledGraphicsOwned( ) ? " (gfx owns)" : "",
                        core1req::allIdle( ) ? "(sends idle)" : "(send busy)" );
        // Core-1 frame hold depth per core (T3.4). Nonzero at idle = a leaked
        // hold (core 1 parked); this line is the leak detector.
        target->printf( "frame hold: core0 %lu  core1 %lu%s\n\r",
                        (unsigned long)core1FrameHoldDepth[ 0 ], (unsigned long)core1FrameHoldDepth[ 1 ],
                        core1FramesHeld( ) ? "  (CORE 1 PARKED)" : "" );
        extern volatile uint32_t ledFramesShown, ledIdleFramesShown;
        target->printf( "led frames shown %lu (idle renders %lu)  uptime %lus\n\r",
                        (unsigned long)ledFramesShown, (unsigned long)ledIdleFramesShown, (unsigned long)( millis( ) / 1000 ) );
        target->print( "led takes (oldest..newest): " );
        for ( int i = 0; i < 32; i++ ) {
            int k = ( ledTakeLogIdx + i ) & 31;
            if ( ledTakeLog[ k ].t == 0 ) continue;
            target->printf( "[%lu b%02x r%d m%d s%d x%u] ", (unsigned long)ledTakeLog[ k ].t, ledTakeLog[ k ].bits, ledTakeLog[ k ].rails, ledTakeLog[ k ].menu, ledTakeLog[ k ].shown, (unsigned)ledTakeLog[ k ].repeats + 1u );
        }
        target->println( "\r" );
    }
    target->println( "\r" );
    target->flush( );

    return CMD_SHOW_MENU;
}

CommandResult cmd_gpioState( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Serial;
    printGPIOState( target );
    return CMD_SHOW_MENU;
}

#if defined(OG_JUMPERLESS)
// Diagnostic: confirm the crossbar chip-select GPIOs are actually driven.
// On the OG, chips A-H sit on GPIO 6-13 and chips I-L on GPIO 20-23 (the V5
// routable-GPIO bank). "I" with no argument dumps the pad/IO state for every
// CS pin so a serial-only read distinguishes a firmware pad clobber (wrong
// FUNCSEL / output-enable) from a hardware mapping issue. "I <chip>" pulses
// that chip's CS line in a ~1 kHz square wave (setCSex high/low) for a few
// seconds so it can be caught on a scope / the debug probe.
// OG-only (gated so the V5 build is unaffected).
CommandResult cmd_testChipSelect( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Serial;

    // chip -> CS GPIO, matching setCSex() exactly.
    auto csGpioForChip = []( int chip ) -> int {
        if ( chip >= 0 && chip <= 7 ) return chip + 6;
        if ( chip >= 8 && chip <= 11 ) return chip + 12;
        return -1;
    };

    String arg = getCommandArgs( line );
    arg.trim( );

    if ( arg.length( ) > 0 ) {
        // Pulse mode: drive one chip's CS for scope capture.
        int chip = arg.toInt( );
        int pin = csGpioForChip( chip );
        if ( pin < 0 ) {
            target->println( "usage: I <chip 0-11>  (0=A .. 11=L), or 'I' for a register dump" );
            return CMD_DONT_SHOW_MENU;
        }
        target->print( "Pulsing CS for chip " );
        target->print( chipNumToChar( chip ) );
        target->print( " on GPIO " );
        target->print( pin );
        target->println( " at ~1 kHz for 3 s (scope it now)..." );
        target->flush( );
        unsigned long until = millis( ) + 3000;
        while ( millis( ) < until ) {
            setCSex( chip, 1 );
            delayMicroseconds( 500 );
            setCSex( chip, 0 );
            delayMicroseconds( 500 );
        }
        target->println( "done." );
        return CMD_DONT_SHOW_MENU;
    }

    // Register dump: FUNCSEL / direction / output-enable / output level.
    target->println( "\n\rCrossbar chip-select GPIO state" );
    target->println( "chip  gpio  func  dir  oe  out" );
    for ( int chip = 0; chip < 12; chip++ ) {
        int pin = csGpioForChip( chip );
        if ( pin < 0 ) continue;
        uint32_t oe = ( sio_hw->gpio_oe >> pin ) & 1u;
        uint32_t out = ( sio_hw->gpio_out >> pin ) & 1u;
        int func = (int)gpio_get_function( pin );
        bool dir = gpio_get_dir( pin );
        target->print( "  " );
        target->print( chipNumToChar( chip ) );
        target->print( "    " );
        if ( pin < 10 ) target->print( ' ' );
        target->print( pin );
        target->print( "    " );
        target->print( func ); // expect 5 (GPIO_FUNC_SIO)
        target->print( "    " );
        target->print( dir ? "out" : "in " );
        target->print( "  " );
        target->print( oe );
        target->print( "   " );
        target->println( out );
    }
    target->println( "\n\rExpect func=5 (SIO), dir=out, oe=1 for ALL rows." );
    target->println( "If chips I-L (GPIO 20-23) differ from A-H, it's a firmware" );
    target->println( "pad clobber; if they match but the chip still won't switch," );
    target->println( "it's hardware (CS not wired to 20-23). Use 'I <chip>' to scope." );
    return CMD_SHOW_MENU;
}
#endif // OG_JUMPERLESS

CommandResult cmd_usbDebugMenu( char c, const String& line ) {
    Jerial.println( "╭─────────────────────────────────╮" );
    Jerial.println( "│        USB Debug Control        │" );
    Jerial.println( "├─────────────────────────────────┤" );
    Jerial.println( "│ 1. Toggle USB debug mode        │" );
    Jerial.println( "│ 2. Manual refresh from USB      │" );
    Jerial.println( "│ 3. Validate all slots           │" );
    Jerial.println( "│ Any other key - Cancel          │" );
    Jerial.println( "╰─────────────────────────────────╯" );
    Jerial.print( "Choose option: " );
    Jerial.flush( );

    while ( Jerial.available( ) == 0 ) {
        delay( 1 );
    }
    char choice = Jerial.read( );
    Jerial.println( choice );

    switch ( choice ) {
    case '1':
        Jerial.println( "\nToggling USB debug mode..." );
        extern bool usb_debug_enabled;
        setUSBDebug( !usb_debug_enabled );
        break;
    case '2':
        if ( isUSBMassStorageMounted( ) ) {
            Jerial.println( "\nPerforming manual refresh from USB..." );
            manualRefreshFromUSB( );
        } else {
            Jerial.println( "\nUSB drive not mounted" );
        }
        break;
    case '3':
        Jerial.println( "\nValidating all slot files..." );
        // validateAllSlots(true);
        break;
    default:
        Jerial.println( "\nCancelled" );
        break;
    }

    Jerial.flush( );
    return CMD_DONT_SHOW_MENU;
}

// Settings commands
CommandResult cmd_ledBrightness( char c, const String& line ) {
    if ( LEDbrightnessMenu( ) == '!' ) {
        clearLEDs( );
        delayMicroseconds( 9200 );
        core1req::post( core1req::REQ_SEND, core1req::SEND_PATHS ); // re-send the paths (mailbox, T2.2b)
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_toggleOLED( char c, const String& line ) {
    String arg = getCommandArgs( line, 50 );
    bool enable;
    if ( arg.length( ) > 0 && ( arg[ 0 ] == '0' || arg[ 0 ] == '1' ) ) {
        enable = ( arg[ 0 ] == '1' );
    } else {
        enable = ( jumperlessConfig.top_oled.enabled == 0 ); // toggle
    }
    extern bool configChanged;
    if ( enable ) {
        Jerial.println( "oled enabled" );
        oled.init( );
        jumperlessConfig.top_oled.enabled = 1;
        configChanged = true;
    } else {
        oled.disconnect( );
        jumperlessConfig.top_oled.enabled = 0;
        oled.oledConnected = false;
        configChanged = true;
        Jerial.println( "oled disconnected" );
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_toggleTerminalColors( char c, const String& line ) {
    extern bool disableTerminalColors;
    String arg = getCommandArgs( line, 50 );
    if ( arg.length( ) > 0 && ( arg[ 0 ] == '0' || arg[ 0 ] == '1' ) ) {
        // '0' = enable colors (disableTerminalColors = false)
        // '1' = disable colors (disableTerminalColors = true)
        disableTerminalColors = ( arg[ 0 ] == '0' ) ? false : true;
    } else {
        disableTerminalColors = !disableTerminalColors;
    }
    if ( disableTerminalColors ) {
        Jerial.println( "Terminal colors disabled" );
    } else {
        Jerial.println( "Terminal colors enabled" );
    }
    Jerial.flush( );
    return CMD_SHOW_MENU;
}

CommandResult cmd_dontShowMenu( char c, const String& line ) {
    if ( dontShowMenu == 0 ) {
        dontShowMenu = 1;
    } else {
        dontShowMenu = 0;
    }
    return CMD_SHOW_MENU;
}

CommandResult cmd_oledInTerminal( char c, const String& line ) {
    String arg = getCommandArgs( line, 50 );
    if ( arg.length( ) > 0 && ( arg[ 0 ] == '0' || arg[ 0 ] == '1' ) ) {
        jumperlessConfig.top_oled.show_in_terminal = ( arg[ 0 ] == '1' ) ? 1 : 0;
    } else {
        jumperlessConfig.top_oled.show_in_terminal = ( jumperlessConfig.top_oled.show_in_terminal > 0 ) ? 0 : 1;
    }
    Jerial.print( "OLED in terminal " );
    Jerial.println( jumperlessConfig.top_oled.show_in_terminal ? "enabled" : "disabled" );
    extern bool configChanged;
    configChanged = true;
    return CMD_SHOW_MENU;
}

CommandResult cmd_cycleFont( char c, const String& line ) {
    oled.cycleFont( );
    return CMD_SHOW_MENU;
}

CommandResult cmd_cycleOledConnectionType( char c, const String& line ) {
    String arg = getCommandArgs( line, 50 );

    int newType;
    if ( arg.length( ) > 0 ) {
        // Single digit (O0/O1/O2/O3) selects directly. We allow '3' (custom)
        // through the explicit-selection path even though the cycle skips it,
        // so power users can still drive it from the terminal if they've
        // pre-configured sda_pin / scl_pin.
        if ( arg.length( ) == 1 && arg[ 0 ] >= '0' && arg[ 0 ] <= '3' ) {
            newType = arg[ 0 ] - '0';
        } else {
            // Fall back to the symbolic-name table so 'O i2c0', 'O gpio_7_8',
            // etc. all work. parseConnectionType returns -1 on miss; default
            // to GPIO 7/8 in that case rather than silently doing something
            // surprising.
            int parsed = parseConnectionType( arg.c_str( ) );
            newType = ( parsed >= 0 && parsed <= 3 ) ? parsed : 0;
        }
        applyOledConnectionType( newType, /*reinitDisplay=*/true, /*persist=*/true );
    } else {
        newType = cycleOledConnectionType( /*reinitDisplay=*/true, /*persist=*/true );
    }

    const char* shortName = getOledConnectionTypeShortName( newType );

    Jerial.print( "OLED connection type -> " );
    Jerial.println( shortName );
    Jerial.flush( );

    // Show a quick on-OLED confirmation if we managed to reconnect, so the
    // user can visually confirm the bus actually came up. If init() failed,
    // skip the print to avoid noisy "is it broken?" follow-ups.
    if ( oled.isConnected( ) ) {
        oled.clearPrintShow( shortName, 2, true, true, true );
    }

    return CMD_DONT_SHOW_MENU;
}

// App/Special mode commands
// Toggle the USB Audio Class microphone on and off.
//
// This is the friendlier way in than the MicroPython API: enabling rewrites the
// USB config descriptor and re-enumerates the device, which drops every CDC
// port - including whichever one you typed this on. The port name is unchanged
// (the serial string is deliberately left alone), so terminals reconnect on
// their own after a couple of seconds.
//
// Optional argument picks the channel pair, e.g. "M23" streams ADC2/ADC3.
CommandResult cmd_usbAudio( char c, const String& line ) {
#if USB_AUDIO_ENABLE
    // Sub-commands are parsed FIRST and work in BOTH states. They used to sit
    // below the enabled-toggle early return, which made every one of them
    // unreachable while the mic was on - so "M?" (the documented way to check
    // whether a recording is healthy) tore the USB stack down and dropped the
    // host's recording instead of printing anything.
    // getCommandArgs(): in line mode the sub-command is in `line` ("M?"); in
    // char mode `line` is the bare "M" and the '?' / "23" / "s" is still in
    // the input stream (up to 50 ms). Parsing `line` directly here meant that
    // in char mode EVERY "M?" / "M23" toggled the device (a USB re-enumeration
    // that drops every port) - only loop()'s help peek used to stand in the
    // way, and B6 (2026-08-17) made "[cmd]?" leave the '?' to commands that
    // own it. `arg` = what follows the 'M', trimmed.
    String arg = getCommandArgs( line, 50 );
    const char sub = ( arg.length( ) >= 1 ) ? arg[ 0 ] : '\0';

    // "M?" -> status and health counters, never a toggle. A clean recording has
    // frames_sent climbing at the sample rate and everything else flat
    // (late_irq/resyncs tick once per flash write, probe_pauses per probe use).
    if ( sub == '?' ) {
        usb_audio_status_t s;
        usb_audio_get_status( &s );
        Jerial.printf( "USB audio: %s, %s, host %s\n\r",
                       s.enabled ? "enabled" : "disabled",
                       s.streaming ? "streaming" : "idle",
                       s.host_open ? "open" : "closed" );
        Jerial.printf( "  ADC%d (L) + ADC%d (R) @ %lu Hz, full scale %.2f V, dc block %s\n\r",
                       s.left_ch, s.right_ch, (unsigned long) s.sample_rate, (double) s.full_scale,
                       s.dc_block ? "on" : "off" );
        if ( s.pending_rate ) {
            Jerial.printf( "  rate %lu Hz pending - applied when the host closes the mic\n\r",
                           (unsigned long) s.pending_rate );
        }
        Jerial.printf( "  frames_sent=%lu fifo_overflow=%lu adc_overrun=%lu late_irq=%lu resyncs=%lu\n\r",
                       (unsigned long) s.frames_sent, (unsigned long) s.fifo_overflow,
                       (unsigned long) s.adc_overrun, (unsigned long) s.late_irq,
                       (unsigned long) s.resyncs );
        Jerial.printf( "  probe_pauses=%lu claim_fail=%lu init_fail=%lu\n\r",
                       (unsigned long) s.probe_pauses, (unsigned long) s.claim_fail,
                       (unsigned long) s.init_fail );
        return CMD_DONT_SHOW_MENU;
    }

    // "Ms" saves the setup so the mic comes back at the NEXT BOOT already
    // enumerated - the only way to run with audio and never drop a port.
    // Enabling when it is already on is a no-op inside
    // usb_audio_set_device_enabled(), so this never re-enumerates.
    if ( sub == 's' || sub == 'S' ) {
        usb_audio_set_device_enabled( true );
        usb_audio_save_config( );
        Jerial.println( "USB audio saved - it will be enumerated from boot, "
                        "so no port drop next time." );
        return CMD_DONT_SHOW_MENU;
    }

    // "M23" -> left = ADC2, right = ADC3.
    if ( arg.length( ) >= 2 && isdigit( (unsigned char) arg[ 0 ] ) &&
         isdigit( (unsigned char) arg[ 1 ] ) ) {
        const int l = arg[ 0 ] - '0';
        const int r = arg[ 1 ] - '0';
        if ( !usb_audio_set_channels( l, r ) ) {
            Jerial.println( "Channels must be two distinct ADC channels 0-7, e.g. M01" );
            return CMD_DONT_SHOW_MENU;
        }
        if ( usb_audio_device_enabled( ) ) {
            // Already live: retune in place rather than dropping every port.
            Jerial.printf( "USB audio: now streaming ADC%d (left) + ADC%d (right)\n\r", l, r );
            return CMD_DONT_SHOW_MENU;
        }
        // Not enabled yet - fall through and bring it up with the new pair.
    }

    // Bare "M" toggles.
    if ( usb_audio_device_enabled( ) ) {
        Jerial.println( "USB audio device disabled - re-enumerating..." );
        Jerial.flush( );
        usb_audio_set_device_enabled( false );
        return CMD_DONT_SHOW_MENU;
    }

    usb_audio_status_t s;
    usb_audio_get_status( &s );

    changeTerminalColor( 46, true, &Jerial );
    Jerial.printf( "USB audio: streaming ADC%d (left) + ADC%d (right) at %lu Hz\n\r",
                   s.left_ch, s.right_ch, (unsigned long) s.sample_rate );
    changeTerminalColor( 39, true, &Jerial );
    Jerial.println( "Route rows to them first, e.g.  connect(ADC0, 20)" );
    Jerial.println( "Then pick 'JL Audio In' as an input device on your computer." );
    Jerial.println( "Re-enumerating - this port will drop and come back shortly..." );
    changeTerminalColor( -1, true, &Jerial );
    Jerial.flush( );

    if ( !usb_audio_set_device_enabled( true ) ) {
        Jerial.println( "Could not enable USB audio (no free DMA channel?)" );
    }
    return CMD_DONT_SHOW_MENU;
#else
    ( void ) c;
    ( void ) line;
    Jerial.println( "USB audio is not available on this board" );
    return CMD_DONT_SHOW_MENU;
#endif
}

CommandResult cmd_showBoardLEDs( char c, const String& line ) {
    // Capture response target first
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) {
        target = &Jerial;
    }

    // Use scrolling region approach for LED dump display
    // ledDumpEnabled is defined in Graphics.cpp
    String arg = getCommandArgs( line, 50 );
    
    // Check for one-shot request: command char 'B' or '!' in arguments
    bool toggleLive =( arg.indexOf( '!' ) != -1 );

    if ( !toggleLive ) {
        // Just dump once to the target stream
        dumpLEDs( -1, -1, 0, 0, 0, 0, target );
    } else {
        // Persistent toggle mode
        if ( arg.length( ) > 0 && ( arg[ 0 ] == '0' || arg[ 0 ] == '1' ) ) {
            setLedDumpEnabled( arg[ 0 ] == '1', target );
        } else {
            setLedDumpEnabled( !ledDumpEnabled, target );
        }
    }
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_startupAnimation( char c, const String& line ) {
    holdCore1Frames( ); // park core 1 while we own the LED strip
    delay( 1 );
    drawAnimatedImage( 0 );
    releaseCore1Frames( );
    return CMD_DONT_SHOW_MENU;
}

// Advanced/Test commands
CommandResult cmd_testStates( char c, const String& line ) {
    Jerial.println( "\n\r╭────────────────────────────────────╮" );
    Jerial.println( "│   States System Test (J command)  │" );
    Jerial.println( "╰────────────────────────────────────╯\n\r" );

    SlotManager& mgr = SlotManager::getInstance( );
    JumperlessState& state = mgr.getActiveState( );

    String commandLine = line;
    if ( commandLine.length( ) > 1 ) {
        commandLine = commandLine.substring( 1 ); // Remove 'J'
        commandLine.trim( );

        if ( commandLine.length( ) > 0 ) {
            Jerial.println( "Parsing connections: " + commandLine );

            int startIdx = 0;
            int connectionsAdded = 0;
            String errorMsg;

            while ( startIdx < (int)commandLine.length( ) ) {
                int commaIdx = commandLine.indexOf( ',', startIdx );
                if ( commaIdx == -1 ) {
                    commaIdx = commandLine.length( );
                }

                String conn = commandLine.substring( startIdx, commaIdx );
                conn.trim( );

                if ( conn.length( ) > 0 ) {
                    int dashIdx = conn.indexOf( '-' );
                    if ( dashIdx != -1 ) {
                        int node1 = conn.substring( 0, dashIdx ).toInt( );
                        int node2 = conn.substring( dashIdx + 1 ).toInt( );

                        Jerial.print( "  Adding connection: " + String( node1 ) + "-" + String( node2 ) + "... " );

                        if ( state.addConnection( node1, node2, errorMsg ) ) {
                            Jerial.println( "✓ Success" );
                            connectionsAdded++;
                        } else {
                            Jerial.println( "✗ Failed" );
                            Jerial.println( "    Error: " + errorMsg );
                        }
                    } else {
                        Jerial.println( "  Invalid format: " + conn + " (should be N1-N2)" );
                    }
                }

                startIdx = commaIdx + 1;
            }

            if ( connectionsAdded > 0 ) {
                Jerial.println( "\n\r─── Applying to Hardware ───" );
                Jerial.print( "Refreshing connections... " );
                state.markDirty( );
                refreshConnections( -1 );
                Jerial.println( "✓ Done" );
            }

            Jerial.println( "\n\r─── Current State ───" );
            Jerial.println( "Connections: " + String( state.connections.numBridges ) );
            Jerial.println( mgr.isPathContext( )
                                ? ( "Active File: " + String( mgr.getActiveSlotPath( ) ) )
                                : ( "Active Slot: " + String( mgr.getActiveSlot( ) ) ) );

            if ( state.connections.numBridges > 0 ) {
                Jerial.println( "\n\rConnections in state:" );
                for ( int i = 0; i < state.connections.numBridges; i++ ) {
                    int n1 = state.connections.bridges[ i ][ 0 ];
                    int n2 = state.connections.bridges[ i ][ 1 ];
                    int dup = state.connections.bridges[ i ][ 2 ];
                    Jerial.print( "  " + String( i + 1 ) + ". " );
                    Jerial.print( String( n1 ) + "-" + String( n2 ) );
                    if ( dup > 1 ) {
                        Jerial.print( " (x" + String( dup ) + " duplicates)" );
                    }
                    Jerial.println( );
                }
            }

            Jerial.println( "\n\r─── Testing YAML Jerialization ───" );
            String yamlOutput;
            if ( state.toYAML( yamlOutput ) ) {
                Jerial.println( "YAML output:" );
                Jerial.println( yamlOutput );

                Jerial.println( "\n\r─── Testing Slot Save ───" );
                Jerial.print( "Saving to slot 7... " );
                if ( mgr.saveSlot( 7, errorMsg ) ) {
                    Jerial.println( "✓ Success" );
                    Jerial.println( "  File: /slots/slot7.yaml" );

                    Jerial.print( "Loading from slot 7... " );
                    if ( mgr.loadSlot( 7, errorMsg ) ) {
                        Jerial.println( "✓ Success" );
                        Jerial.println( "  Loaded " + String( mgr.getActiveState( ).connections.numBridges ) + " connections" );
                    } else {
                        Jerial.println( "✗ Failed" );
                        Jerial.println( "  Error: " + errorMsg );
                    }
                } else {
                    Jerial.println( "✗ Failed" );
                    Jerial.println( "  Error: " + errorMsg );
                }
            } else {
                Jerial.println( "Failed to Jerialize to YAML" );
            }

            Jerial.println( "\n\r─── Memory Usage ───" );
            Jerial.println( "Active state RAM: ~" + String( mgr.getActiveStateRAMUsage( ) ) + " bytes" );
            Jerial.println( "State object size: ~" + String( state.estimateRAMUsage( ) ) + " bytes" );
            Jerial.println( "\n\r─── Test Complete ───" );
        } else {
            Jerial.println( "No connections specified!" );
            Jerial.println( "Usage: J 1-2  or  J 1-5,10-20,15-30" );
        }
    } else {
        Jerial.println( "States System Test Command" );
        Jerial.println( "\n\rUsage:" );
        Jerial.println( "  J 1-2              - Add connection 1-2" );
        Jerial.println( "  J 1-5,10-20        - Add multiple connections" );
        Jerial.println( "  J 1-5,1-5,1-5      - Add duplicates (increments count)" );
        Jerial.println( "\n\rFeatures:" );
        Jerial.println( "  • Validates connections" );
        Jerial.println( "  • Tracks duplicate counts" );
        Jerial.println( "  • YAML Jerialization" );
        Jerial.println( "  • Save/load from slots" );
        Jerial.println( "  • Undo/redo history" );
        Jerial.println( "\n\rExample:" );
        Jerial.println( "  J 1-5              - Creates connection 1-5" );
        Jerial.println( "  J TOP_RAIL-10      - Connects top rail to row 10" );
        Jerial.println( "  J GND-32           - Connects ground to row 32" );
    }

    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_printYAML( char c, const String& line ) {
    extern JumperlessState globalState;
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) target = &Jerial;

    target->println();

    if ( globalState.isDirty( ) ) {
        unsigned long timeSince = millis( ) - globalState.getLastModifiedTime( );
        target->print( "Time since last change: " );
        target->print( timeSince / 1000 );
        target->println( " seconds" );
    }

    int showANSI = 2;
    String yamlArg = getCommandArgs( line, 20 );
    if ( yamlArg.length( ) > 0 ) {
        if ( yamlArg[ 0 ] == '0' ) showANSI = 0;
        else if ( yamlArg[ 0 ] == '2' ) showANSI = 2;
        else if ( yamlArg[ 0 ] == '1' ) showANSI = 1;
    }

    // Hardware-truth advisory, not a rewrite. Y's body IS the save format -
    // S pastes it back and slot files are written by the same toYAML() - so it
    // must keep printing globalState.power verbatim. But a save=0 rail write
    // (the guide's exit restore) deliberately moves the rails without touching
    // that state, and a dump silently reading 0 V over live rails is the same
    // "rails aren't setting" false-bug the other readouts just stopped
    // telling. Name the divergence instead of hiding it.
    {
        float hwTop = getDacHardwareVoltage( 2 );
        float hwBot = getDacHardwareVoltage( 3 );
        if ( fabsf( hwTop - globalState.power.topRail ) > 0.02f ||
             fabsf( hwBot - globalState.power.bottomRail ) > 0.02f ) {
            target->print( "  (rails are physically at top=" );
            target->print( hwTop, 2 );
            target->print( "V bot=" );
            target->print( hwBot, 2 );
            target->println( "V - the power: below is this context's SAVED" );
            target->println( "   state, which is what S pastes back)" );
        }
    }

    String yamlOutput;
    if ( globalState.toYAML( yamlOutput, showANSI ) ) {
        target->print( yamlOutput );
        target->println();
    } else {
        target->println( "✗ Failed to generate YAML" );
    }

    target->println( "\n\r" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_loadYAMLState( char c, const String& line ) {
    Jerial.print( "\n\rPaste YAML state (end with an empty line):\n\r" );
    Jerial.flush( );

    String yamlBuffer;
    yamlBuffer.reserve( 16384 );

    // Y's own output has blank lines between sections, so "stop at the first
    // empty line" cut every Y round-trip at sourceOfTruth: and the rest ran as
    // menu commands. readPastedBlock keeps inner blank lines and ends on an
    // empty line followed by quiet.
    if ( !readPastedBlock( yamlBuffer ) ) {
        Jerial.print( "\r\nNo YAML received\n\r" );
        return CMD_SHOW_MENU;
    }

    Jerial.print( "\r\nApplying state...\n\r" );

    String errorMsg;
    if ( !globalState.fromYAML( yamlBuffer, errorMsg ) ) {
        Jerial.print( "Error: " );
        Jerial.print( errorMsg );
        Jerial.print( "\n\r" );
        return CMD_SHOW_MENU;
    }

    initializeFakeGpioFromLoadedState( );
    refreshConnections( -1, 1, 1 );
    finalizeFakeGpioAfterRouting( );
    applyStateToHardware( );

    Jerial.print( "State applied successfully!\n\r" );
    return CMD_SHOW_MENU;
}

CommandResult cmd_rawSpeedTest( char c, const String& line ) {
    Jerial.println( "Raw speed test..." );
    Jerial.println( "Read frequency on row 29\n\n\r" );

    holdCore1Frames( ); // park core 1 while we own the crossbar
    unsigned long cycles = 1000000;
    unsigned long start = micros( );
    sendXYraw( 10, 0, 4, 1 );
    for ( unsigned long i = 0; i < cycles; i++ ) {
        sendXYraw( 10, 0, 0, 1 );
        sendXYraw( 10, 0, 0, 0 );
    }
    unsigned long end = micros( );
    Jerial.print( "Time for " );
    Jerial.print( cycles );
    Jerial.print( " on off cycles: " );
    Jerial.print( end - start );
    Jerial.println( " microseconds" );
    Jerial.print( "Time per cycle: " );
    Jerial.print( ( end - start ) / cycles );
    Jerial.println( " microseconds" );
    Jerial.print( "Frequency: " );
    Jerial.print( ( (float)cycles / (float)( end - start ) ) * 1000 );
    Jerial.println( " kHz\n\r" );
    Jerial.flush( );
    releaseCore1Frames( );

    return CMD_SHOW_MENU;
}

CommandResult cmd_printColorSpectrum( char c, const String& line ) {
    // These are already declared at the top of the file as const
    for ( int i = 0; i < highSaturationSpectrumColorsCount; i++ ) {
        changeTerminalColorHighSat( i, true, &Jerial, 0 );
        Jerial.print( i );
        Jerial.print( ": " );
        if ( i < 10 ) {
            Jerial.print( " " );
        }
        Jerial.print( highSaturationSpectrumColors[ i ] );

        Jerial.print( "\t\t" );
        if ( i < highSaturationBrightColorsCount ) {
            changeTerminalColorHighSat( i, true, &Jerial, 1 );
            Jerial.print( i );
            Jerial.print( ": " );
            if ( i < 10 ) {
                Jerial.print( " " );
            }
            Jerial.print( highSaturationBrightColors[ i ] );
        }
        Jerial.println( );
    }

    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_dumpOLED( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    if ( target == nullptr ) {
        target = &Jerial;
    }
    target->println( "\n\r" );
    oled.dumpFrameBuffer( target );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_printMicrosPerByte( char c, const String& line ) {
    printMicrosPerByte( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_printTextFromMenu( char c, const String& line ) {
    while ( Jerial.available( ) == 0 && slotChanged == 0 ) {
        if ( slotChanged == 1 ) {
            // Early exit handled by slotChanged
        }
    }
    printTextFromMenu( );

    clearLEDs( );
    extern int& defconDisplay;
    requestLedShow( 1 );
    defconDisplay = -1;

    return CMD_SHOW_MENU;
}

CommandResult cmd_wavegen( char c, const String& line ) {
    // This is the complex wavegen test code from 'G' command
    // For now we'll just call reload config
    return cmd_reloadConfig( c, line );
}

CommandResult cmd_dmxJerial( char c, const String& line ) {
    runApp( -1, (char*)"DMX Jerial" );
    return CMD_DONT_SHOW_MENU;
}



CommandResult cmd_erattaClear( char c, const String& line ) {
    erattaClearGPIO( -1 );
    Jerial.println( "Eratta cleared" );
    Jerial.flush( );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_printWireStatus( char c, const String& line ) {
    Stream* target = Jerial.getResponseTarget( );
    printWireStatus( target );   // nullptr -> Jerial (main); backchannel target -> port 7
    return CMD_DONT_SHOW_MENU;
}

// Stub implementation for cmd_dmxSerial
CommandResult cmd_dmxSerial( char c, const String& line ) {
    Jerial.println( "DMX Serial functionality not yet implemented" );
    return CMD_DONT_SHOW_MENU;
}

CommandResult cmd_uartStats( char c, const String& line ) {
#if ASYNC_PASSTHROUGH_ENABLED == 1
    // Display AsyncPassthrough statistics
    // AsyncPassthrough::printStatistics();

    // // Offer to clear statistics
    // Jerial.print("Press 'c' to clear statistics, any other key to continue: ");
    // Jerial.flush();

    // unsigned long timer = millis();
    // while (Jerial.available() == 0 && millis() - timer < 3000) {
    //     // Wait for input with timeout
    // }

    // if (Jerial.available() > 0) {
    //     char response = Jerial.read();
    //     Jerial.println(response);
    //     if (response == 'c' || response == 'C') {
    //         AsyncPassthrough::clearStatistics();
    //         Jerial.println("✓ Statistics cleared");
    //     }
    // } else {
    //     Jerial.println("(timeout)");
    // }
#else
    Jerial.println( "AsyncPassthrough is not enabled" );
#endif
    return CMD_DONT_SHOW_MENU;
}


