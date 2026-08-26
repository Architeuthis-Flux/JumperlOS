#include "Menus.h"
#include "ReadingDisplay.h" // resetLastShown() - drop the live-reading pin
#include "RotaryEncoder.h"
#include "SafeString.h"
#include "SingleCharCommands.h"
#include <Arduino.h>

#include "Apps.h"
#include "ArduinoStuff.h"
#include "BitmapEditor.h"
#include "CH446Q.h"
#include "Commands.h"
#include "FatFS.h"
#include "FileParsing.h"
#include "FilesystemStuff.h" // For safe file operations
#include "Graphics.h"
#include "Highlighting.h"
#include "ImagesApp.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "MenuTransitions.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "RotaryEncoder.h"
#include "States.h"
#include "configManager.h"
#include "menuTree.h"
#include "oled.h"
#include "Undo.h"

// External declarations
extern Adafruit_SSD1306 display;

// ============================================================================
// Menus Class Implementation
// ============================================================================

// Static member initialization
Menus& Menus::getInstance() {
    static Menus inst;
    return inst;
}

Menus::Menus( ) {
    // Initialize defaults
}

/**
 * @brief Main service method for menu system
 *
 * This is called each loop iteration and handles:
 * - Click menu detection
 * - Menu rendering when active
 */
ServiceStatus Menus::service( ) {
    lastStatus = ServiceStatus::IDLE;
    // dont allow menus to run if the bitmap editor is active
    //  Check if menu is active
    if ( inClickMenu != 0 ) {
        lastStatus = ServiceStatus::BLOCKING;
        return lastStatus;
    }

    // Check for menu activation (delegated to clickMenu)
    int menuResult = clickMenu( );
    if ( menuResult >= 0 ) {
        lastStatus = ServiceStatus::BLOCKING;
    }

    return lastStatus;
}

// Backward compatibility - create references to singleton members
volatile int& inClickMenu = Menus::getInstance( ).inClickMenu;
int& defconDisplay = Menus::getInstance( ).defconDisplay;
int& selectingRotaryNode = Menus::getInstance( ).selectingRotaryNode;
int& menuState = Menus::getInstance( ).menuState;
int& menuPosition = Menus::getInstance( ).menuPosition;
int& menuScroll = Menus::getInstance( ).menuScroll;
int& menuScrollTarget = Menus::getInstance( ).menuScrollTarget;
int& menuScrollMax = Menus::getInstance( ).menuScrollMax;
int& menuPositionMax = Menus::getInstance( ).menuPositionMax;
int& menuPositionMin = Menus::getInstance( ).menuPositionMin;

// ============================================================================
// Existing Functions
// ============================================================================

int menuRead = 0;
int menuLength = 0;
char menuChars[ 1000 ];

int menuLineIndex = 0;
// String menuLines[150];
int menuLevels[ 150 ];
int stayOnTop[ 150 ];
uint8_t numberOfChoices[ 150 ];
uint8_t actions[ 150 ]; //>n nodes 1 //>b baud 2 //>v voltage 3 //>i integer 7 //>t text 8 //>c connect 9

uint32_t optionSlpitLocations[ 150 ];
int numberOfLevels = 0;
int optionVoltage = 0;

uint8_t selectMultiple[ 10 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int selectMultipleIndex = 0;

char hardCodeOptions[ 10 ][ 10 ] = { "Tx", "Rx", "SDA", "SCL", "5V",
                                     "3V3", "8V", "USB 2", "Print" };
int brightnessOptionMap[] = { 3, 4, 6, 9, 10, 14, 18, 26, 32, 42, 48 };
int menuBrightnessOptionMap[] = { -80, -60, -30, -15, 0, 20, 50, 100, 150, 200 };

struct action {
    int previousMenuPositions[ 10 ];
    int connectOrClear[ 10 ];
    char fromAscii[ 20 ][ 10 ];
    int from[ 20 ];
    int to[ 20 ];
    int previousMenuIndex;
    int connectIndex;
    int optionVoltage;
    int baud;
    int printOrUSB; // 0 print 1 USB
    float analogVoltage;
    int integerValue;   // For integer input (action 7)
    String stringValue; // For text input (action 8)
};

void readMenuFile( int flashOrLocal ) {
    // FatFS.begin();
    // delay(10);
    if ( flashOrLocal == 0 ) {
        writeMenuTree( );
        // while (core2busy == true) {
        //   // Serial.println("waiting for core2 to finish");
        //   }
        core1busy = true;
        File menuFile = safeFileOpen( "/MenuTree.txt", "r", 1000 );
        if ( !menuFile ) {
            delay( 1000 );
            Serial.println( "Failed to open menu file" );
            core1busy = false;
            return;
        }
        menuLength = 0;

        while ( menuFile.available( ) && menuLineIndex < 150 ) {
            menuLines[ menuLineIndex ] = menuFile.readStringUntil( '\n' );
            menuLineIndex++;
        }
        menuLineIndex--;

        menuRead = 1;

        safeFileClose( menuFile, false );
        core1busy = false;
    } else {

        menuLineIndex = 0;

        for ( int i = 0; i < 150; i++ ) {
            // Serial.print(i);
            // Serial.print(": ");
            // Serial.println(menuLines[i]);
            if ( menuLines[ i ] == "end" ) {
                menuLines[ i ] = "";
                break;
            } else {
                // menuLines[i] = String(menuTreeStrings[i]);
                menuLineIndex++;
            }
        }

        // No "end" sentinel found (it's consumed by the first pass — this is a
        // re-read). Without this clamp menuLineIndex lands on 150 and every
        // menu walk goes out of bounds. Shouldn't happen now that menuRead is
        // latched below, but never let a bad table corrupt all menu consumers.
        if ( menuLineIndex >= 150 ) {
            menuLineIndex = 0;
        }

        // Latch so initMenu() never re-runs this path. It is NOT idempotent:
        // it consumes the "end" sentinel, so a second run can't find the end
        // of the table. (This latch was only ever set in the FatFS branch
        // above, so every initMenu() call re-read the local tree.)
        menuRead = 1;
    }
}

int menuParsed = 0;

// Sized 16, not 10: the menu tree has 12 level-0 categories today (Rails,
// Connect, Output, Show, Apps, Calibration, Files, Slots, History,
// DisplayOptions, OLED, RoutingOptions) - count the categories, don't trust
// the array. A 10-slot array let the fill loop below write categoryRanges[10]
// and [11] out of bounds on every menu parse.
int categoryRanges[ 16 ][ 2 ] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } };
int categoryIndex = 0;

void parseMenuFile( void ) {

    for ( int i = 0; i < menuLineIndex + 1;
          i++ ) { // remove comments and empty lines

        if ( menuLines[ i ].startsWith( "/" ) || menuLines[ i ].startsWith( "#" ) ||
             menuLines[ i ].startsWith( "\n" ) || menuLines[ i ].length( ) < 2 ) {

            for ( int j = i; j < menuLineIndex; j++ ) {
                menuLines[ j ] = menuLines[ j + 1 ];
            }

            i--; // so it checks again
            menuLineIndex--;
        }
    }

    for ( int i = 0; i <= menuLineIndex; i++ ) {

        menuLevels[ i ] = menuLines[ i ].lastIndexOf( '-' ) + 1;
        menuLines[ i ].remove( 0, menuLevels[ i ] );
        if ( menuLevels[ i ] > numberOfLevels ) {
            numberOfLevels = menuLevels[ i ];
        }
        if ( menuLines[ i ].startsWith( "$" ) ) { // && menuLines[i].endsWith("$")) {
            stayOnTop[ i ] = 1;
            menuLines[ i ].remove( 0, 1 );
            menuLines[ i ].remove( menuLines[ i ].length( ) - 1, 1 );
        } else {
            stayOnTop[ i ] = 0;
        }

        // Check if line ends with ^ for font selection action
        if ( menuLines[ i ].endsWith( "^" ) ) {
            actions[ i ] = 6;
            menuLines[ i ].remove( menuLines[ i ].length( ) - 1, 1 ); // Remove the ^
        }
        int starIndex = menuLines[ i ].indexOf( "*" );
        int starCount = 0;

        int shift = 0;
        while ( starIndex != -1 ) {

            menuLines[ i ].remove( starIndex, 1 );

            // optionSlpitLocations[i];
            bitSet( optionSlpitLocations[ i ], starIndex + shift );
            shift++;
            starCount++;
            starIndex = menuLines[ i ].indexOf( "*" );
        }

        numberOfChoices[ i ] = starCount / 2;

        // delay(1000);
        // Serial.print(menuLines[i]);
        // Serial.print(" ");
        // Serial.println(numberOfChoices[i]);

        int actionIndex = menuLines[ i ].indexOf( ">" );
        int actionChar = menuLines[ i ].charAt( actionIndex + 1 );
        if ( actionIndex != -1 ) {
            int charsToRemove = 3; // Default: remove ">X " (e.g., ">n ")

            switch ( actionChar ) {
            case 'n':
                actions[ i ] = 1;
                break;
            case 'b':
                actions[ i ] = 2;
                break;
            case 'v':
                actions[ i ] = 3;
                break;
            case 's':
                actions[ i ] = 4;
                break;
            case 'a':
                actions[ i ] = 5;
                break;
            case 'i':
                actions[ i ] = 7; // Integer input: >i(min)(max) - parsed dynamically when selected
                // Need to remove entire >i(min)(max) string
                // Find the last closing parenthesis
                {
                    int firstParen = menuLines[ i ].indexOf( '(', actionIndex );
                    if ( firstParen != -1 ) {
                        int parenCount = 0;
                        int lastParen = firstParen;
                        for ( int k = firstParen; k < menuLines[ i ].length( ); k++ ) {
                            if ( menuLines[ i ].charAt( k ) == '(' )
                                parenCount++;
                            if ( menuLines[ i ].charAt( k ) == ')' ) {
                                parenCount--;
                                lastParen = k;
                                if ( parenCount == 0 )
                                    break;
                            }
                        }
                        charsToRemove = lastParen - actionIndex + 1;
                    }
                }
                break;
            case 't':
                actions[ i ] = 8; // Text input: >t(maxLength)
                // Need to remove entire >t(maxLength) string
                {
                    int firstParen = menuLines[ i ].indexOf( '(', actionIndex );
                    int closeParen = menuLines[ i ].indexOf( ')', firstParen );
                    if ( firstParen != -1 && closeParen != -1 ) {
                        charsToRemove = closeParen - actionIndex + 1;
                    }
                }
                break;
            case 'c':
                actions[ i ] = 9; // Connect action >c(number of selections)
                // Need to remove entire >c(...) string if it has parameters
                {
                    int firstParen = menuLines[ i ].indexOf( '(', actionIndex );
                    int closeParen = menuLines[ i ].indexOf( ')', firstParen );
                    if ( firstParen != -1 && closeParen != -1 ) {
                        charsToRemove = closeParen - actionIndex + 1;
                    }
                }
                break;
            default:
                actions[ i ] = 0;
                break;
            }
            char actionChar2 = menuLines[ i ].charAt( actionIndex + 2 );
            if ( actionChar2 >= '0' && actionChar2 <= '9' ) {
                numberOfChoices[ i ] = menuLines[ i ].charAt( actionIndex + 2 ) - '0';
            } else {
                // numberOfChoices[i] = 1;
            }

            actionIndex = menuLines[ i + 1 ].indexOf( ">" );
            if ( actionIndex != -1 ) {
                selectMultiple[ selectMultipleIndex ] = i;
                selectMultipleIndex++;
            }

            // if (menuLines[i].indexOf("^") != -1) {
            //   menuLines[i].remove(actionIndex, 3);
            //   }

            // Remove the action string from the menu line
            menuLines[ i ].remove( actionIndex, charsToRemove );
        }
    }

    for ( int j = 0; j < menuLineIndex; j++ ) {
        // Serial.println(menuLevels[j]);
        if ( menuLevels[ j ] == 0 ) {
            // Bounds guard: count the categories, don't trust the array size.
            if ( categoryIndex >= 16 ) {
                break;
            }
            categoryRanges[ categoryIndex ][ 0 ] = j;

            if ( categoryIndex > 0 ) {
                categoryRanges[ categoryIndex - 1 ][ 1 ] = j - 1;
            }
            categoryIndex++;
        }
    }
    categoryRanges[ categoryIndex - 1 ][ 1 ] = menuLineIndex;

    int printMenuLinesAtStartup = 0;
    if ( printMenuLinesAtStartup == 1 ) {
        delay( 2000 );

        for ( int j = 0; j < 10; j++ ) {
            Serial.print( categoryRanges[ j ][ 0 ] );
            Serial.print( " " );
            Serial.println( categoryRanges[ j ][ 1 ] );
        }
        Serial.println( "idx level    line            stay     choices   action" );
        for ( int i = 0; i <= menuLineIndex; i++ ) {

            Serial.print( i );
            if ( i < 10 ) {
                Serial.print( " " );
            }
            Serial.print( "   " );
            Serial.print( menuLevels[ i ] );
            Serial.print( "   " );
            for ( int j = 0; j < menuLevels[ i ]; j++ ) {
                Serial.print( " " );
            }
            // Serial.print(" \t");
            int spaces = Serial.print( menuLines[ i ] );
            for ( int j = 0; j < 20 - spaces; j++ ) {
                Serial.print( " " );
            }
            Serial.print( " " );
            Serial.print( stayOnTop[ i ] );
            Serial.print( " \t" );
            Serial.print( numberOfChoices[ i ] );
            Serial.print( " \t" );
            Serial.print( actions[ i ] );
            Serial.print( " \t0b" );
            for ( int j = 0; j < 32; j++ ) {
                if ( j % 8 == 0 ) {
                    Serial.print( " " );
                }
                Serial.print( bitRead( optionSlpitLocations[ i ], j ) );
            }

            Serial.print( "\n\rMenuLineIndex: " );
            Serial.println( menuLineIndex );
        }
        //   for (int i = 0; i < menuLineIndex; i++) {
        // getActionCategory(i);
        //   }

        menuParsed = 1;
    }
}

uint32_t menuColors[ 10 ] = { 0x09000a, 0x0f0004, 0x080800, 0x010f00,
                              0x000a03, 0x00030a, 0x040010, 0x070006 };

void initMenu( void ) {

    // FatFS.begin();
    // delay(1);
    if ( menuRead == 0 ) {
        // Serial.println(menuLines);
        // delay(1);
        readMenuFile( 1 );
        // return 0;
    }
    if ( menuParsed == 0 ) {
        // delay(1);
        parseMenuFile( );
        /// return 0;
    }
}

unsigned long noInputTimer = millis( );
unsigned long exitMenuTime = 9995000;

int subSelection = -1;

int subMenuChoices[ 10 ] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
int maxNumSelections = 0;

int returnToMenuPosition = -1;
int returnToMenuLevel = -1;
int returningFromTimeout = 0;

char submenuBuffer[ 20 ];

char chosenOptions[ 20 ][ 40 ];

int previousMenuSelection[ 10 ] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

// Full LED-state teardown for leaving menu mode mid-action to hand the
// display to something else (apps, File Manager, calibration, history
// scrub). These doMenuAction branches zero inClickMenu themselves before
// running — which used to make clickMenu()'s end-of-session cleanup
// (gated on inClickMenu still being 1) a silent no-op, leaving the logo
// ring enabled (stale ring frozen on the logo) and the depth pads lit
// after the app exited.
static void exitMenuModeForAction( void ) {
    inClickMenu = 0;
    logoRing.enabled = false;
    clearColorOverrides( false, true, false ); // restore depth pads
    menuTransitionCancel( );
}

// Re-assert a swallowed LED show request. Core 2's end-of-frame
// compare-and-swap (`if (showLEDsCore2 == rails) showLEDsCore2 = 0`) can't
// tell a fresh request apart from the one it just serviced: a
// showLEDsCore2 = 2 written while Core 2 is mid-frame (with rails==2
// already captured) gets cleared along with the serviced one, and the
// painted buffer never reaches the LEDs. The bracketed menu painters
// recover via the transition linger's re-requests; the unbracketed screens
// (node picker, value editors) call this in their wait loops instead.
static void menuShowKeepalive( unsigned long& lastRequestMs,
                               unsigned long cadenceMs = 50 ) {
    if ( ledShowIdle( ) && millis( ) - lastRequestMs >= cadenceMs ) {   // T2.2c
        requestLedShow( 2 );
        lastRequestMs = millis( );
    }
}

int Menus::clickMenu( int menuType, int menuOption, int extraOptions ) {
    // Don't allow menus to run if the bitmap editor is active
    if ( BitmapEditor::getInstance( ).active ) {
        return -1;
    }

    if ( menuLineIndex < 2 ) {
        // b.clear();
        b.print( "No menu file", 0x0f0400, 0xFFFFFF, 0, -1, 0 );
        inClickMenu = 0;
        return -1;
    }

    int returnedMenuPosition = -1;
    bool menuSessionRan = false;
    if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) {
        // Check if Highlighting wants to handle this button press (for voltage adjustment)
        if ( Highlighting::getInstance( ).wantsToHandleButtonPress( ) ) {
            // Don't consume the button press - let Highlighting handle it
            return -1;
        }
        // Don't set showLEDsCore2 here - buffer not ready yet
        // It will be set in getMenuSelection() after buffer is prepared

        encoderButtonState = IDLE;
        inClickMenu = 1;
        menuSessionRan = true;
        // The menu owns the terminal for this session and scrolls it freely,
        // which moves the rows a live reading pinned for itself. Drop the OLED
        // dedupe so the next reading repaints once the menu is gone. MUST stay
        // inside this branch: clickMenu() is POLLED from Menus::service()
        // every main-loop pass (see the cleanup note below), and calling this
        // unconditionally re-pinned every live reading at ~1kHz - each reading
        // scrolled two fresh rows instead of repainting in place.
        ReadingDisplay::resetLastShown( );
        logoRing.enabled = true; // ring owns the logo LEDs for this menu session
        logoRing.holdStepLengthMs = 0; // no stale hold-stepping state from last session
        // if (menuRead == 0) {
        //   readMenuFile();
        // }
        // if (menuParsed == 0) {
        //   parseMenuFile();
        // }
        // Serial.setTimeout(1000);
        // Serial.flush();

        // showLoss();
        // while(Serial.available() == 0) ;

        returnedMenuPosition = getMenuSelection( );
        while ( returnedMenuPosition == -1 && Serial.available( ) == 0 && arduinoInReset == 0 ) {
            // delayMicroseconds(5000);
            // Use state-based check in loop (doesn't consume event)
            if ( probing.checkProbeButtonState( ) == 1 ) {
                Serial.println( "Probe button pressed" );
                // Same teardown, same order, as the end-of-session block below:
                // leaving inClickMenu at 1 pins Menus::service() at BLOCKING.
                inClickMenu = 0;
                logoRing.enabled = false;
                clearColorOverrides( false, true, false ); // restore depth pads
                menuTransitionCancel( );
                b.clear( );
                requestLedShow( -1 );
                return -3;
            }
            // oled.showJogo32h();
            returnedMenuPosition = getMenuSelection( );
        }
        if ( returnedMenuPosition == -2 ) {
            // Exit preview mode if active (user cancelled menu)
            SlotManager& mgr = SlotManager::getInstance( );
            if ( mgr.isPreviewMode( ) ) {
                String errorMsg;
                mgr.exitPreview( false, errorMsg ); // Cancel preview
            }

            // This early return bypasses the end-of-session cleanup below,
            // so it must do its own buffer wipe + negative show (clear
            // before drawing wires) or the menu text stays on the
            // breadboard underneath the redrawn nets. Same order as that
            // block: menuTransitionCancel() before b.clear(), or a linger
            // frame can restomp the cleared buffer.
            inClickMenu = 0;
            logoRing.enabled = false;
            clearColorOverrides( false, true, false ); // restore depth pads
            menuTransitionCancel( );
            b.clear( );
            requestLedShow( -1 );

            oled.showJogo32h( );

            return -2;
        }

        // requestLedShow( 1 );

        // Serial.print("returnedMenuPosition: ");
        //  Serial.println(returnedMenuPosition);
        //  Serial.print("subSelection: ");
        //  Serial.println(subSelection);
        //  Serial.print("actions: ");
        //  Serial.println(actions[returnedMenuPosition]);
        //  Serial.print("subMenuChoices: ");
        //  for (int i = 0; i < 8; i++) {
        //    Serial.print(subMenuChoices[i]);
        //    Serial.print(", ");
        //  }
        //  Serial.println();
        //  populateAction();
        //   doMenuAction(returnedMenuPosition);

        // getMenuSelection();
    }

    // Only clean up if a menu session actually ran. clickMenu() is POLLED from
    // Menus::service() every main-loop iteration, so an unconditional
    // clearColorOverrides() here wiped the connector-pad overrides thousands
    // of times a second on the main screen — which erased the encoder
    // hold/reboot sweep's pad writes the instant they landed (caught live via
    // a debugger watchpoint on GPIOcolorOverride0).
    //
    // Gated on a local session flag, NOT on inClickMenu still being 1: action
    // paths (apps / File Manager / history scrub / voltage selectors) zero
    // inClickMenu themselves, which used to skip this cleanup entirely and
    // strand the logo ring + depth pads.
    if ( menuSessionRan ) {
        inClickMenu = 0;
        logoRing.enabled = false;
        clearColorOverrides( false, true, false ); // restore depth pads
        menuTransitionCancel( );

        // Hand the breadboard back to the wires with a CLEAN slate: wipe the
        // menu text out of the pixel buffer and request the show as a
        // NEGATIVE value, which makes Core 2 run clearLEDsExceptRails()
        // before showNets() - a plain positive show draws the wires on top
        // of whatever menu text is still in the buffer.
        b.clear( );
        requestLedShow( -1 );

        // Leave the terminal in a known state when the wheel session ends:
        // clear whatever the menu / launched app left on screen and reprint
        // the serial main menu (printMenu() itself respects dontShowMenu).
        extern int showExtraMenu;
        Serial.print( "\x1b[2J\x1b[H" );
        Serial.flush( );
        singleCharCommands.printMenu( showExtraMenu );
    }

    // oled.showJogo32h();

    return returnedMenuPosition;
}

action currentAction;

int alreadySelected = 0;

void clearAction( void ) {
    for ( int i = 0; i < 10; i++ ) {
        currentAction.previousMenuPositions[ i ] = -1;
        currentAction.connectOrClear[ i ] = -1;
    }
    for ( int i = 0; i < 20; i++ ) {
        currentAction.from[ i ] = -1;
        currentAction.to[ i ] = -1;
    }
    for ( int i = 0; i < 20; i++ ) {
        for ( int j = 0; j < 10; j++ ) {
            currentAction.fromAscii[ i ][ j ] = ' ';
        }
    }
    currentAction.previousMenuIndex = 0;
    currentAction.connectIndex = 0;
    currentAction.optionVoltage = 0;
    currentAction.baud = 0;
    currentAction.printOrUSB = 0;
    currentAction.analogVoltage = 0.0;
}

void populateAction( void ) {
    int actionIndex = 0;
    int connectIndex = 0;
    for ( int i = 0; i < 10; i++ ) {
        if ( previousMenuSelection[ i ] != -1 ) {
            currentAction.previousMenuPositions[ actionIndex ] =
                previousMenuSelection[ i ];
            // currentAction.connectOrClear[connectIndex] = 1;
            actionIndex++;
            // connectIndex++;
        }
    }
    currentAction.previousMenuIndex = actionIndex;
    /// currentAction.connectIndex = connectIndex;
    currentAction.optionVoltage = optionVoltage;
}

void printActionStruct( void ) {

    int spaces = 0;
    Serial.println( "\n\r" );
    Serial.print( "\tMenu Path: \t" );
    for ( int i = 0; i < 10; i++ ) {
        if ( currentAction.previousMenuPositions[ i ] != -1 ) {
            if ( i > 0 ) {
                Serial.print( " > " );
            }

            Serial.print( menuLines[ currentAction.previousMenuPositions[ i ] ] );
        }
    }
    if ( currentAction.connectOrClear[ 0 ] != -1 ) {
        Serial.println( );

        Serial.print( "\tConnect Or Clear: \t" );

        for ( int i = 0; i < 10; i++ ) {
            if ( currentAction.connectOrClear[ i ] != -1 ) {
                Serial.print( currentAction.connectOrClear[ i ] );
                Serial.print( ", " );
            }
        }
    }
    if ( currentAction.from[ 0 ] != -1 ) {
        Serial.println( );
        Serial.print( "\tfrom: \t\t" );
        for ( int i = 0; i < 10; i++ ) {
            if ( currentAction.from[ i ] != -1 ) {

                Serial.print( currentAction.from[ i ] );
                Serial.print( ",\t" );
            }
        }
    }
    if ( currentAction.to[ 0 ] != -1 ) {
        Serial.println( );
        Serial.print( "\tto: \t\t" );
        for ( int i = 0; i < 10; i++ ) {
            if ( currentAction.to[ i ] != -1 ) {
                Serial.print( currentAction.to[ i ] );
                Serial.print( ",\t" );
            }
        }
    }
    if ( currentAction.fromAscii[ 0 ][ 0 ] != ' ' &&
         currentAction.fromAscii[ 0 ][ 0 ] != 0 ) {
        Serial.println( );
        Serial.print( "\tfromAscii: \t" );
        for ( int i = 0; i < 10; i++ ) {
            if ( currentAction.fromAscii[ i ][ 0 ] != ' ' &&
                 currentAction.fromAscii[ i ][ 0 ] != 0 ) {
                for ( int j = 0; j < 10; j++ ) {
                    if ( currentAction.fromAscii[ i ][ j ] != ' ' &&
                         currentAction.fromAscii[ i ][ j ] != 0 ) {
                        Serial.print( currentAction.fromAscii[ i ][ j ] );
                    }
                }
                Serial.print( ",\t" );
            }
        }
    }

    // Serial.println();
    // Serial.print("\tpreviousMenuIndex: ");
    // Serial.println(currentAction.previousMenuIndex);
    if ( currentAction.connectIndex != 0 ) {
        Serial.print( "\n\r\tconnectIndex:  " );
        Serial.print( currentAction.connectIndex );
    }
    if ( currentAction.optionVoltage != 0.0 ) {
        Serial.print( "\n\r\toptionVoltage:  " );
        Serial.print( currentAction.optionVoltage );
    }
    if ( currentAction.analogVoltage != 0.0 ) {
        Serial.print( "\n\r\tanalogVoltage:  " );
        Serial.print( currentAction.analogVoltage );
    }
    Serial.println( "\n\r" );
}

// Accumulated ring hue offset per menu level. Level 0 starts at 0; each child
// level inherits its parent's offset plus the hue of the item that was
// selected to descend into it, so a submenu's colors are anchored to the color
// of the item you picked (each branch gets its own hue lineage). Because the
// lineage keeps accumulating, levels with only 1-2 options still land on a
// different hue per branch instead of always showing the same color.
static int menuRingLevelOffset[ 12 ] = { 0 };

// Show the menu breadcrumb on the connector pad LEDs via the color overrides.
// Pads read top-to-bottom as ADC -> DAC -> GPIO. The pad for the current level
// glows in the live selection's ring hue (so at the top level only ADC is lit,
// tracking the highlighted item), and the pads above it hold the colors of the
// items that were picked to get here. Past 3 levels the window slides so the
// pads always show the colors of the last 3 selections.
static void setMenuDepthLEDs( int menuLevel, int currentHueIdx ) {
    static const logoOverrideNames padPairs[ 3 ][ 2 ] = {
        { ADC_0, ADC_1 },   // topmost pad pair = shallowest shown level
        { DAC_0, DAC_1 },
        { GPIO_0, GPIO_1 }, // bottom pad pair = current level (once depth >= 2)
    };
    int firstLevel = menuLevel - 2;
    if ( firstLevel < 0 )
        firstLevel = 0;
    for ( int p = 0; p < 3; p++ ) {
        int lvl = firstLevel + p;
        uint32_t c = 0;
        if ( lvl <= menuLevel ) {
            // Ancestor pads show the hue of the item chosen at that level —
            // exactly the offset it seeded into the level below it. The
            // current level's pad tracks the live selection hue.
            int idx = ( lvl < menuLevel ) ? menuRingLevelOffset[ lvl + 1 ] : currentHueIdx;
            idx = ( ( idx % LOGO_COLOR_LENGTH ) + LOGO_COLOR_LENGTH ) % LOGO_COLOR_LENGTH;
            c = dimLogoColor( applyRingHueTweaks( logoColorsAll[ PALETTE_RAINBOW ][ idx ] ), 70 );
        }
        setLogoOverride( padPairs[ p ][ 0 ], c );
        setLogoOverride( padPairs[ p ][ 1 ], c );
    }
}

// Drive the reusable logo ring from the current menu cursor. Siblings at the
// active level are the contiguous run around menuPosition bounded by entries of
// a lower level (deeper children are skipped in the count but stay inside the
// run). itemCount/selectedIndex feed the ring layout; the hue offset is
// inherited from the parent's selected item.
// Returns the selected item's ring color so the menu text can be painted in
// the same hue (falls back to the depth palette if the cursor is invalid).
static uint32_t updateMenuLogoRing( int menuPosition, int menuLevel ) {
    if ( menuLevel < 0 )
        menuLevel = 0;
    int maxLevel = (int)( sizeof( menuRingLevelOffset ) / sizeof( menuRingLevelOffset[ 0 ] ) ) - 1;
    if ( menuLevel > maxLevel )
        menuLevel = maxLevel;

    int offset = ( menuLevel == 0 ) ? 0 : menuRingLevelOffset[ menuLevel ];

    // The hold/reboot sweep owns the connector pads while the button is held
    // (each hold-to-back step re-renders the menu line, and repainting the
    // breadcrumb here would stomp the sweep's progress every step). The
    // breadcrumb comes back on the first repaint after release.
    bool padsFree = encoderButtonState != HELD && encoderButtonState != MEDIUM_HELD &&
                    encoderButtonState != LONG_HELD;

    if ( menuPosition < 0 || menuPosition > menuLineIndex ) {
        if ( padsFree ) {
            setMenuDepthLEDs( menuLevel, offset );
        }
        setLogoRing( 0, 0, ( ( offset % LOGO_COLOR_LENGTH ) + LOGO_COLOR_LENGTH ) % LOGO_COLOR_LENGTH );
        return menuColors[ menuLevel > 9 ? 9 : menuLevel ];
    }
    int lo = menuPosition;
    while ( lo > 0 && menuLevels[ lo - 1 ] >= menuLevel )
        lo--;
    int hi = menuPosition;
    while ( hi < menuLineIndex && menuLevels[ hi + 1 ] >= menuLevel )
        hi++;

    int count = 0;
    int sel = 0;
    for ( int i = lo; i <= hi; i++ ) {
        if ( menuLevels[ i ] == menuLevel ) {
            if ( i == menuPosition )
                sel = count;
            count++;
        }
    }

    // Selected item's hue: siblings are spaced evenly across the whole wheel
    // by their actual count (item k of N sits at k/N-th of the circle — must
    // match ringItemColor in LEDs.cpp), on top of the branch offset.
    int selHueStep = ( count > 0 ) ? ( sel * LOGO_COLOR_LENGTH ) / count : 0;

    // Seed the next level's offset from the currently-selected item so a later
    // descent inherits the picked item's hue.
    if ( menuLevel < maxLevel ) {
        menuRingLevelOffset[ menuLevel + 1 ] = offset + selHueStep;
    }

    // Feed the selected item's ring hue to the frame-transition engine so the
    // accent transition types flash each item in its own color.
    int accentIdx = ( ( selHueStep + offset ) % LOGO_COLOR_LENGTH + LOGO_COLOR_LENGTH ) % LOGO_COLOR_LENGTH;
    uint32_t accent = applyRingHueTweaks( logoColorsAll[ PALETTE_RAINBOW ][ accentIdx ] );
    menuTransitionSetAccent( accent );

    // Breadcrumb pads: ancestors hold their picked-item colors, the current
    // level's pad tracks the live selection hue.
    if ( padsFree ) {
        setMenuDepthLEDs( menuLevel, accentIdx );
    }

    setLogoRing( count, sel, ( ( offset % LOGO_COLOR_LENGTH ) + LOGO_COLOR_LENGTH ) % LOGO_COLOR_LENGTH );

    // Breadboard text tracks the same hue, but as its own dimmer color: the
    // raw ring palette is calibrated for the diffused logo and is way too
    // bright for the bare breadboard LEDs. Re-value it down near the old
    // menuColors brightness (their HSV v sat around 8-15), a few ticks
    // brighter — the print path then scales this by the user's
    // menuBrightnessSetting (printGraphicsRow), so the setting still rules.
    // Because the hue comes from the branch offset lineage (parent offset +
    // selected item hue), it keeps evolving at every depth.
    hsvColor textHsv = RgbToHsv( accent );
    textHsv.v = 18;
    return HsvToRaw( textHsv );
}

// Consolidated menu-line render. Paints the selected line (plus the optional
// stay-on-top header) and the reminder onto the breadboard buffer, optionally
// mirrors text to the OLED, and signals Core 2 to flush — keeping both panels in
// sync. Several navigation sites used to hand-roll this b.clear/b.print/
// printMenuReminder/showLEDsCore2 combo and occasionally dropped the OLED update
// or the flush, leaving one display stale. Route them all through here instead.
//   stayOnTopLevel/stayOnTopIndex: getMenuSelection's pinned-header state (-1 = none).
//   oledText: when non-null, mirrored to the OLED; pass nullptr to leave it alone.
//   ledShowMode: value written to showLEDsCore2 (2 = flush the menu text buffer).
static void renderMenuLine( int menuPosition, int menuLevel,
                            int stayOnTopLevel, int stayOnTopIndex,
                            const char* oledText = nullptr, int ledShowMode = 2 ) {
    // Ring update runs first: it computes the selected item's ring hue (and
    // hands it to the transition engine for the accent frame), and the item
    // text below is painted in a dim variant of that same hue — the text
    // continuously matches the logo ring instead of the per-depth palette.
    // It only touches logo/pad state, never the breadboard rows, so it stays
    // outside the beginDraw/arm bracket.
    uint32_t itemColor = updateMenuLogoRing( menuPosition, menuLevel );
    // Scrolling choice rows (number/option lists, numberOfChoices > 0) keep
    // the original depth-palette coloring; the ring hue is only for regular
    // item labels.
    if ( menuPosition >= 0 && menuPosition <= menuLineIndex &&
         numberOfChoices[ menuPosition ] > 0 ) {
        itemColor = menuColors[ menuLevel > 9 ? 9 : menuLevel ];
    }
    // Bracket the repaint for the transition engine: Core 2 holds off showing
    // the breadboard between beginDraw and arm, so a half-painted buffer never
    // hits the LEDs, and arm() snapshots the finished frame as the blend target.
    menuTransitionBeginDraw( );
    b.clear( );
    if ( stayOnTopLevel != -1 && stayOnTopIndex != -1 && menuLevel != stayOnTopLevel ) {
        b.print( menuLines[ stayOnTopIndex ].c_str( ), menuColors[ stayOnTopLevel ],
                 0xFFFFFF, 0, 0,
                 menuLines[ stayOnTopIndex ].length( ) >= 7 || menuLevel == 0 ? 1 : 3 );
        b.printMenuReminder( menuLevel, menuColors[ menuLevel ] );
        b.print( menuLines[ menuPosition ].c_str( ), itemColor, 0xFFFFFF, 0,
                 1, menuLines[ menuPosition ].length( ) >= 7 || menuLevel == 0 ? 1 : 3 );
    } else {
        b.print( menuLines[ menuPosition ].c_str( ), itemColor, 0xFFFFFF, 0,
                 -1, menuLines[ menuPosition ].length( ) >= 7 || menuLevel == 0 ? 1 : 3 );
        b.printMenuReminder( menuLevel, menuColors[ menuLevel ] );
    }
    menuTransitionArm( );
    if ( oledText != nullptr ) {
        oled.clearPrintShow( oledText, 2, true, true, true, -1, -1 );
    }
    requestLedShow( ledShowMode );
}

// (The Menu FX debug TUI drives the real click menu directly — see
// action_menuTransitionTuner() in Debugs.cpp.)

int getMenuSelection( void ) {

    optionVoltage = 0;
    int stayOnTopLevel = -1;
    int stayOnTopIndex = -1;

    int menuPosition = -1;
    int lastMenuPosition = 0;
    int menuLevel = 0;
    int lastMenuLevel = 2;

    int subMenuStartIndex = 0;
    int subMenuEndIndex = menuLineIndex;
    int firstTime = 1;
    int showFonts = 0;
    int lastSubmenuOption = 0;
    int back = 0;
    subSelection = -1;

    // for (int i = 0; i < 10; i++) {
    //   if (selectMultiple[i] != 0) {
    //     Serial.print("Select multiple: ");
    //     Serial.println(selectMultiple[i]);
    //   }
    // }
    // delay(10);

    for ( int i = 0; i < 10; i++ ) {
        previousMenuSelection[ i ] = -1;
        subMenuChoices[ i ] = -1;
    }

    int force = 0;

    // Hold-to-back pacing: the first back-step registers (and redraws) the
    // moment HELD fires; while the button stays physically down, further
    // levels step at most once per HOLD_BACK_REPEAT_MS. Lifting the finger
    // re-arms so a fresh hold backs out immediately again.
    const unsigned long HOLD_BACK_REPEAT_MS = 800;
    bool holdBackArmed = true;
    unsigned long holdBackLastStepMs = 0;
    unsigned long lastPressShowMs = 0;

    clearAction( );

    // requestLedShow( 2 );
    // clearLEDsExceptRails( );
    // Don't set showLEDsCore2 here - buffer not populated with menu text yet
    // It will be set after buffer is updated in the menu loop (firstTime == 1 case)
    // waitCore2( );
    // delay(100);
    if ( returnToMenuPosition != -1 && returnToMenuLevel != -1 ) {
        menuPosition = returnToMenuPosition;
        returnToMenuPosition = -1;

        menuLevel = returnToMenuLevel;
        returnToMenuLevel = -1;
        returningFromTimeout = 1;
        // force = 1;
    }

    noInputTimer = millis( );
    while ( Serial.available( ) == 0 ) {
        jOS.serviceInner( );
        rotaryEncoderButtonStuff( ); // Update button state every iteration

        // Finger lifted -> re-arm the hold-to-back gate so the next hold's
        // first back-step registers immediately. Wait for the state machine to
        // settle to IDLE, then repaint once: the hold sweep clears the pad
        // overrides on release, so this restores the menu depth ladder.
        if ( !isEncoderButtonPhysicallyPressed( ) && encoderButtonState == IDLE ) {
            if ( !holdBackArmed && menuPosition >= 0 ) {
                renderMenuLine( menuPosition, menuLevel, stayOnTopLevel, stayOnTopIndex );
            }
            holdBackArmed = true;
            // Release ends the hold-stepping session — HELD hands the logo
            // back to the reboot sweep again on the next hold.
            logoRing.holdStepLengthMs = 0;
        }

        // Keep the logo refreshing while the button is held so the center LED
        // animates its V (white -> black -> active color) press indicator,
        // including the per-step replays during hold-to-back. Paced to one
        // frame per 16 ms: unthrottled this posted at the loop rate (~1-2 kHz)
        // for as long as the button was down. The V is animated by
        // holdAnimationStuff(), not by this request, so the visual is the same.
        if ( encoderButtonState == PRESSED || logoRing.holdStepLengthMs > 0 ) {
            menuShowKeepalive( lastPressShowMs, 16 );
        }

        if ( Serial.getWriteError( ) ) {

            Serial.clearWriteError( );
            Serial.flush( );
            Serial.println( "Serial error" );
        }
        /// rotaryDivider = 9;
        // delayMicroseconds(1000);
        // 8 raw counts = one physical detent on this encoder. The old value
        // of 4 was 2 menu items per detent - masked for years by the event
        // collapser in rotaryEncoderStuff(), exposed when emission became
        // faithful (one event per committed step).
        rotaryDivider = 8;
        if ( millis( ) - noInputTimer > exitMenuTime ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;

            returnToMenuPosition = menuPosition;
            returnToMenuLevel = menuLevel;
            b.clear( );
            return -2;
        }

        // One probe-button read per pass: getButtonPress() CONSUMES the press
        // whatever its value, so checking "== 2" here used to EAT a remove
        // press before the back-step handler below could see it - the remove
        // button never took you back in the menus (Kevin, 2026-08-18). Read
        // once, dispatch on the local.
        int probePress = ProbeButton::getInstance( ).getButtonPress( );
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED || probePress == 2 ) { //! click

            lastMenuLevel = menuLevel;
            noInputTimer = millis( );

            // chosen[chosenIndex] = menuPosition;
            // chosenType[chosenIndex] = 3;
            // chosenIndex++;

            if ( stayOnTop[ menuPosition ] == 1 ) {
                stayOnTopLevel = menuLevel;
                stayOnTopIndex = menuPosition;
            }

            previousMenuSelection[ menuLevel ] = menuPosition;

            menuLevel++;

            if ( menuLevel > numberOfLevels + 1 ) {
                menuLevel = numberOfLevels;
            }

            for ( int i = menuPosition + 1; i < menuLineIndex; i++ ) {

                if ( menuLevels[ i ] < menuLevel ) {
                    subMenuEndIndex = i;
                    break;
                }
            }

            for ( int i = subMenuEndIndex - 1; i > 0; i-- ) {
                if ( menuLevels[ i ] < menuLevel ) {
                    subMenuStartIndex = i;
                    break;
                }
            }

            // Serial.println();
            // Clear the consumed event and wait for physical button release (debounce).
            // Keep critical services running and bail out after 2s so a stuck or
            // shorted button can't hang Core 0 here forever.
            encoderButtonState = IDLE;
            unsigned long releaseWaitStart = millis( );
            while ( isEncoderButtonPhysicallyPressed( ) ) {
                jOS.serviceInner( );
                if ( millis( ) - releaseWaitStart > 2000 ) {
                    break;
                }
            }
        } else if ( encoderDirectionState == UP || firstTime == 1 || force == 1 ) { // ! up
            // lastMenuLevel > menuLevel) {
            encoderDirectionState = NONE;
            if ( firstTime == 1 ) {
                // Serial.print("Menu position: ");
                // Serial.println(menuPosition);
                // Serial.flush();
                delayMicroseconds( 15000 );
                firstTime = 0;
                // requestLedShow( 2 );
            }
            // currentAction.Category
            resetPosition = true;
            noInputTimer = millis( );
            lastMenuPosition = menuPosition;
            if ( returningFromTimeout == 0 ) {
                menuPosition += 1;
            }
            returningFromTimeout = 0;
            if ( menuPosition > subMenuEndIndex ) {
                menuPosition = subMenuStartIndex;
            }

            int loopCount = 0;
            for ( int i = menuPosition; i <= menuLineIndex; i++ ) {
                // Serial.print(i);
                if ( menuLevels[ i ] == menuLevel ) {
                    // Serial.println("^\n\r");
                    menuPosition = i;
                    // currentAction.Category = getActionCategory(menuPosition);
                    break;
                }
                if ( i == subMenuEndIndex && loopCount == 0 ) {
                    // Serial.println();
                    // Serial.print(i);
                    // Serial.println("\n\n\r");
                    loopCount++;
                    i = subMenuStartIndex - 1;
                } else if ( i == subMenuEndIndex && loopCount == 1 ) {

                    // Serial.println( "!!! " );
                    b.clear( );
                    returnToMenuPosition = -1;
                    returnToMenuLevel = -1;
                    // doMenuAction(menuPosition);

                    // for (int i = 0; i < 8; i++) {
                    //   if (subMenuChoices[i] != -1) {
                    //     Serial.print(menuLines[subMenuChoices[i]]);
                    //     Serial.print(" > ");
                    //   }
                    // }

                    // Serial.println();
                    int keepSelecting = 0;

                    for ( int i = 0; i < 10; i++ ) {
                        if ( selectMultiple[ i ] == menuPosition ) {
                            keepSelecting = 1;
                        }
                        if ( i >= selectMultipleIndex ) {
                            break;
                        }
                    }

                    if ( keepSelecting == 0 ) {
                        delayMicroseconds( 100 );

                        return doMenuAction( );
                    } else {
                        synthesizeEncoderClick( );
                        // menuPosition++;
                        break;
                    }
                    break;
                    // break;
                }
            }

            // Breadboard first: the LED request goes out before the ~12 ms
            // blocking SSD1306 write below, not after it. Nothing in the
            // Serial/OLED block feeds renderMenuLine.
            renderMenuLine( menuPosition, menuLevel, stayOnTopLevel, stayOnTopIndex );

            delayMicroseconds( 100 );
            Serial.print( " " );
            if ( actions[ menuPosition ] == 0 ) {
                Serial.print( "\r                                              \r" );

                // oled.clear();
                for ( int j = 0; j <= menuLevels[ menuPosition ]; j++ ) {
                    Serial.print( ">" );

                    if ( j > 8 ) {
                        break;
                    }
                }
                Serial.flush( );

                String menuLine = menuLines[ menuPosition ];
                menuLine.replace( "~", "±" );
                menuLine.replace( "_", "-" );
                Serial.print( menuLine.c_str( ) );
                Serial.flush( );
                menuLine.replace( "±", "+-" );
                if ( numberOfChoices[ menuPosition ] == 0 ) {
                    oled.clearPrintShow( menuLine.c_str( ), 2, true, true, true, -1, -1 );
                }
            } else if ( actions[ menuPosition ] == 6 ) {
                // Font preview - display at native size (1x) using the specific font
                int fontIndex = oled.setFont( menuLines[ menuPosition ], 0 );
                if ( fontIndex >= 0 ) {
                    // Display the font name at its native size (textSize=1)
                    oled.clearPrintShow( menuLines[ menuPosition ], 2, true, true, true, -1, -1 );
                } else {
                    // Fallback if font not found
                    oled.clearPrintShow( menuLines[ menuPosition ], 2, true, true, true, -1, -1 );
                }
            } else {
                // Action row: still mirror the line to the OLED so it doesn't lag
                // behind the breadboard when you scroll onto an action entry.
                String menuLine = menuLines[ menuPosition ];
                menuLine.replace( "~", "±" );
                menuLine.replace( "_", "-" );
                Serial.println( menuLine.c_str( ) );
                Serial.flush( );
                menuLine.replace( "±", "+-" );
                oled.clearPrintShow( menuLine.c_str( ), 2, true, true, true, -1, -1 );
            }

            /// Serial.print(menuLines[menuPosition]);

            if ( menuLevel != lastMenuLevel ) {
                // Serial.println();
            }
            lastMenuLevel = menuLevel;
            // previousMenuSelection[menuLevel] = menuPosition;
            // Serial.print("Menu position: ");
            // Serial.println(menuPosition);
            // Serial.flush();
            // b.print(menuPosition);

        } else if ( encoderDirectionState == DOWN || ( lastMenuLevel < menuLevel ) ) { //! down
            encoderDirectionState = NONE;
            noInputTimer = millis( );
            // lastMenuLevel = menuLevel;
            lastMenuPosition = menuPosition;
            // if (back == 1) {
            //   back = 0;
            // } else {
            if ( menuPosition == previousMenuSelection[ menuLevel ] ) {
                previousMenuSelection[ menuLevel ] = -1;
            } else {
                menuPosition -= 1;

                if ( menuPosition < subMenuStartIndex ) {
                    menuPosition = subMenuEndIndex;
                }

                int loopCount = 0;

                for ( int i = menuPosition; i >= 0; i-- ) {

                    if ( menuLevels[ i ] == menuLevel ) {

                        menuPosition = i;
                        // currentAction.Category = getActionCategory(menuPosition);
                        break;
                    }
                    if ( i == subMenuStartIndex && loopCount == 0 ) {
                        loopCount++;

                        i = subMenuEndIndex + 1;
                    } else if ( i == subMenuStartIndex && loopCount == 1 ) {

                        Serial.println( " " );

                        b.clear( );
                        returnToMenuPosition = -1;
                        returnToMenuLevel = -1;

                        // for (int i = 0; i < 8; i++) {
                        //   if (chosen[i] != -1) {
                        //     Serial.print(menuLines[chosen[i]]);
                        //     Serial.print(" -> ");
                        //   }
                        // }

                        if ( actions[ menuPosition ] == 3 && subSelection != -1 ) {
                            // Serial.println("get float voltage");
                            getActionFloat( menuPosition );
                        }

                        if ( actions[ menuPosition ] == 7 ) {
                            // Integer input action found
                            // currentAction.fromAscii[subSelection] contains the selected option
                            // text (subSelection is -1 when nothing was picked — don't index)
                            String selectedOptionText =
                                ( subSelection >= 0 && subSelection < 20 )
                                    ? String( currentAction.fromAscii[ subSelection ] )
                                    : String( "" );
                            selectedOptionText.toLowerCase( );

                            // Check if it's "Custom" - only then trigger interactive input
                            if ( selectedOptionText.indexOf( "custom" ) != -1 ) {
                                // Parse integer range from current menu line
                                String menuStr = menuLines[ menuPosition ];
                                int actionIndex = menuStr.indexOf( '>' );

                                if ( actionIndex != -1 ) {
                                    int firstParen = menuStr.indexOf( '(', actionIndex );
                                    int firstClose = menuStr.indexOf( ')', firstParen );
                                    int secondParen = menuStr.indexOf( '(', firstClose );
                                    int secondClose = menuStr.indexOf( ')', secondParen );

                                    int minVal = 0, maxVal = 100;
                                    if ( firstParen != -1 && firstClose != -1 && secondParen != -1 && secondClose != -1 ) {
                                        String minStr = menuStr.substring( firstParen + 1, firstClose );
                                        String maxStr = menuStr.substring( secondParen + 1, secondClose );
                                        minVal = minStr.toInt( );
                                        maxVal = maxStr.toInt( );
                                    }

                                    // Get current value from config if this is for Width/Height
                                    int currentVal = -1;
                                    if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Width" ) != -1 ) {
                                        currentVal = jumperlessConfig.top_oled.width;
                                    } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Height" ) != -1 ) {
                                        currentVal = jumperlessConfig.top_oled.height;
                                    }

                                    currentAction.integerValue = getActionInt( minVal, maxVal, currentVal );
                                }
                            } else {
                                // Preset value selected - extract the numeric value
                                int presetValue = 0;
                                for ( int k = 0; k < selectedOptionText.length( ); k++ ) {
                                    if ( selectedOptionText[ k ] >= '0' && selectedOptionText[ k ] <= '9' ) {
                                        presetValue = presetValue * 10 + ( selectedOptionText[ k ] - '0' );
                                    }
                                }

                                if ( presetValue > 0 ) {
                                    currentAction.integerValue = presetValue;
                                } else {
                                    // Fallback - couldn't parse, use current config value
                                    if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Width" ) != -1 ) {
                                        currentAction.integerValue = jumperlessConfig.top_oled.width;
                                    } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Height" ) != -1 ) {
                                        currentAction.integerValue = jumperlessConfig.top_oled.height;
                                    } else {
                                        currentAction.integerValue = 128; // Safe default
                                    }
                                }
                            }
                        }

                        int keepSelecting = 0;

                        for ( int i = 0; i < 10; i++ ) {
                            if ( selectMultiple[ i ] == menuPosition ) {
                                keepSelecting = 1;
                            }
                            if ( i >= selectMultipleIndex ) {
                                break;
                            }
                        }

                        if ( keepSelecting == 0 ) {

                            return doMenuAction( );
                        } else {
                            synthesizeEncoderClick( );
                            // menuPosition++;
                            break;
                        }
                        break;
                    }
                }
            }
            // }
            //         Serial.print("   \t\t");
            // Serial.println(menuPosition);
            // if (menuLevel == lastMenuLevel) {

            // Serial.print(menuLevel);
            delayMicroseconds( 100 );
            Serial.print( " " );
            if ( actions[ menuPosition ] == 0 ) {
                Serial.print( "\r                                              \r" );
                // oled.clear();

                for ( int j = 0; j <= menuLevels[ menuPosition ]; j++ ) {
                    Serial.print( ">" );
                    // oled.clearPrintShow(">", 2, 5, 8, false, false);
                    if ( j > 8 ) {
                        break;
                    }
                }

                Serial.flush( );

                String menuLine = menuLines[ menuPosition ];
                menuLine.replace( "~", "±" );
                menuLine.replace( "_", "-" );
                Serial.print( menuLine.c_str( ) );
                Serial.flush( );
                menuLine.replace( "±", "+-" );

                // if (actions[menuPosition+1] != 0) {

                if ( numberOfChoices[ menuPosition ] == 0 ) {

                    oled.clearPrintShow( menuLine.c_str( ), 2, true, true, true, -1, -1 );
                }
                //                }
            } else if ( actions[ menuPosition ] == 6 ) {
                // Font preview - display at native size (1x) using the specific font
                int fontIndex = oled.setFont( menuLines[ menuPosition ], 0 );
                if ( fontIndex >= 0 ) {
                    // Display the font name at its native size (textSize=1)
                    oled.clearPrintShow( menuLines[ menuPosition ], 1, true, true, true, -1, -1 );
                } else {
                    // Fallback if font not found
                    oled.clearPrintShow( menuLines[ menuPosition ], 2, true, true, true, -1, -1 );
                }
            } else {
                // Serial.println(menuLines[menuPosition-1]);
                Serial.println( " " );
                // oled.print(" ");
            }

            if ( actions[ menuPosition ] == 1 && subSelection != -1 ) {
                for ( int a = 0; a < 8; a++ ) {
                    subMenuChoices[ a ] = -1;
                }
                int nextOption = 0;

                subMenuChoices[ subSelection ] = selectNodeAction( subSelection );
                maxNumSelections = numberOfChoices[ menuPosition ];
                // Serial.println(maxNumSelections);
                // Serial.println();
                while ( nextOption != -1 && maxNumSelections > 1 ) {
                    // Serial.println(maxNumSelections);
                    // Serial.println();
                    //  Serial.println("fuck");
                    nextOption = selectSubmenuOption( lastMenuPosition, lastMenuLevel );

                    if ( nextOption == -1 ) {
                        // Serial.println("-1");
                        break;
                    }
                    // Serial.println(nextOption);
                    subMenuChoices[ nextOption ] = selectNodeAction( nextOption );
                    maxNumSelections--;
                    /// Serial.println("fuck");
                    // Serial.print("[ ");

                    // for (int a = 0; a < numberOfChoices[menuPosition]; a++) {
                    //   Serial.print(subMenuChoices[a]);
                    //   Serial.print(",");
                    // }
                    // Serial.println(" ] \n\r");
                }
                returnToMenuPosition = -1;
                returnToMenuLevel = -1;

                // menuLevel++;

                // if (keepSelecting == 0) {

                return doMenuAction( );
                // } else {
                //   encoderButtonState = RELEASED;
                //   lastButtonEncoderState = PRESSED;
                //  // menuPosition++;
                //  break;
                // }

            } else if ( actions[ menuPosition ] == 3 && subSelection != -1 ) {

                // Serial.println("get float voltage");
                // Serial.println(subSelection);
                // Serial.println(subSelection);
                // Serial.println(subSelection);

                getActionFloat( menuPosition, subSelection );

                // doMenuAction();
                return doMenuAction( );

            } else if ( actions[ menuPosition ] == 7 ) {
                // Integer input action - handle presets or custom input
                // currentAction.fromAscii[subSelection] contains the selected option
                // text (subSelection is -1 when nothing was picked — don't index)
                String selectedOptionText =
                    ( subSelection >= 0 && subSelection < 20 )
                        ? String( currentAction.fromAscii[ subSelection ] )
                        : String( "" );
                selectedOptionText.toLowerCase( );

                // Check if "Custom" was selected
                if ( selectedOptionText.indexOf( "custom" ) != -1 ) {
                    // Parse range and show interactive input
                    String menuStr = menuLines[ menuPosition ];
                    int actionIndex = menuStr.indexOf( '>' );

                    if ( actionIndex != -1 ) {
                        int firstParen = menuStr.indexOf( '(', actionIndex );
                        int firstClose = menuStr.indexOf( ')', firstParen );
                        int secondParen = menuStr.indexOf( '(', firstClose );
                        int secondClose = menuStr.indexOf( ')', secondParen );

                        int minVal = 0, maxVal = 100;
                        if ( firstParen != -1 && firstClose != -1 && secondParen != -1 && secondClose != -1 ) {
                            String minStr = menuStr.substring( firstParen + 1, firstClose );
                            String maxStr = menuStr.substring( secondParen + 1, secondClose );
                            minVal = minStr.toInt( );
                            maxVal = maxStr.toInt( );
                        }

                        // Get current value from config
                        int currentVal = -1;
                        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Width" ) != -1 ) {
                            currentVal = jumperlessConfig.top_oled.width;
                        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Height" ) != -1 ) {
                            currentVal = jumperlessConfig.top_oled.height;
                        }

                        currentAction.integerValue = getActionInt( minVal, maxVal, currentVal );
                    }
                } else {
                    // Preset value - parse it directly
                    int presetValue = 0;
                    for ( int k = 0; k < selectedOptionText.length( ); k++ ) {
                        if ( selectedOptionText[ k ] >= '0' && selectedOptionText[ k ] <= '9' ) {
                            presetValue = presetValue * 10 + ( selectedOptionText[ k ] - '0' );
                        }
                    }

                    if ( presetValue > 0 ) {
                        currentAction.integerValue = presetValue;
                    } else {
                        currentAction.integerValue = 128; // Safe default
                    }
                }

                return doMenuAction( );

            } else if ( actions[ menuPosition ] == 8 ) {
                // Text input action: -->t(maxLength)
                // Check if user selected "Edit" (trigger input) or "Clear" (skip input)
                // (subSelection is -1 when nothing was picked — don't index)
                String selectedOptionText =
                    ( subSelection >= 0 && subSelection < 20 )
                        ? String( currentAction.fromAscii[ subSelection ] )
                        : String( "" );
                // Serial.print("DEBUG: selectedOptionText BEFORE toLowerCase: '");
                // Serial.print(selectedOptionText);
                // Serial.println("'");
                selectedOptionText.toLowerCase( );
                // Serial.print("DEBUG: selectedOptionText AFTER toLowerCase: '");
                // Serial.print(selectedOptionText);
                // Serial.println("'");
                // Serial.flush();

                if ( selectedOptionText.indexOf( "text" ) != -1 ) {
                    // Parse max length from menu string
                    String menuStr = menuLines[ menuPosition ];
                    int actionIndex = menuStr.indexOf( '>' );

                    int maxLength = 32; // Default
                    if ( actionIndex != -1 ) {
                        int firstParen = menuStr.indexOf( '(', actionIndex );
                        int firstClose = menuStr.indexOf( ')', firstParen );

                        if ( firstParen != -1 && firstClose != -1 ) {
                            String lengthStr = menuStr.substring( firstParen + 1, firstClose );
                            maxLength = lengthStr.toInt( );
                            if ( maxLength <= 0 || maxLength > 128 ) {
                                maxLength = 32; // Clamp to safe default
                            }
                        }
                    }

                    // Get the string from user via rotary encoder
                    currentAction.stringValue = getActionString( maxLength );
                } else if ( selectedOptionText.indexOf( "bitmap" ) != -1 ) {
                    // Get the bitmap from user via rotary encoder
                    encoderButtonState = IDLE;
                    lastButtonEncoderState = IDLE;
                    // Serial.println("getActionBitmap");
                    // Serial.flush();
                    String bitmapFilename = getActionBitmap( );
                    Serial.println( bitmapFilename );
                    Serial.flush( );
                    if ( bitmapFilename.length( ) > 0 ) {
                        currentAction.stringValue = bitmapFilename;
                    } else {
                        currentAction.stringValue = "";
                    }
                } else if ( selectedOptionText.indexOf( "clear" ) != -1 ) {
                    // "Clear" selected - don't trigger text input, just clear in doMenuAction

                    currentAction.stringValue = "";
                }

                return doMenuAction( );

            } else {

                renderMenuLine( menuPosition, menuLevel, stayOnTopLevel, stayOnTopIndex );
                if ( numberOfChoices[ menuPosition ] > 0 ) {
                    // Serial.println("  fuck     !!");
                    subSelection = selectSubmenuOption( menuPosition, menuLevel );

                    if ( subSelection != -1 ) {
                        // menuLevel++;
                        // Serial.print("subselection: ");
                        // Serial.println(subSelection);
                        synthesizeEncoderClick( );
                    }
                    lastMenuPosition = menuPosition;

                    force = 0;
                }
            }

            requestLedShow( 2 );
            lastMenuLevel = menuLevel;
            // previousMenuSelection[menuLevel] = menuPosition;

            // b.print(menuPosition);
        }

        // Loop pacing goes AFTER the dispatch above, not before it: run first
        // and every pass pays it before looking at the button/detent, so an
        // event that was already pending on entry waits out the whole delay.
        delayMicroseconds( 400 );

        delayMicroseconds( 80 );

        // Hold-to-back: the first back-step fires the moment HELD registers
        // (the redraw happens right here in the handler, not on release), then
        // a continued hold steps one more level per HOLD_BACK_REPEAT_MS.
        // MEDIUM_HELD / LONG_HELD are the hold-animation escalations of the
        // same physical hold, so they keep stepping too.
        bool holdBackStep = false;
        if ( encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
             encoderButtonState == LONG_HELD ) {
            if ( holdBackArmed || ( millis( ) - holdBackLastStepMs >= HOLD_BACK_REPEAT_MS ) ) {
                holdBackStep = true;
                holdBackArmed = false;
                holdBackLastStepMs = millis( );
                // Each back-step restarts the hold cycle: the center V replays
                // for the next level (renderLogoRing keeps the ring up through
                // HELD while holdStepLengthMs is set), and resetting
                // buttonHoldStart keeps the MEDIUM/LONG reboot escalation from
                // creeping in while you're still stepping through menus.
                buttonHoldStart = millis( );
                logoRing.holdStepStartMs = millis( );
                logoRing.holdStepLengthMs = HOLD_BACK_REPEAT_MS;
            }
        }

        if ( holdBackStep || probePress == 1 ) {
            noInputTimer = millis( );
            lastMenuLevel = menuLevel;
            // Serial.println("Held");

            // if (stayOnTopLevel == menuLevel) {

            stayOnTopLevel = -1;
            stayOnTopIndex = -1;
            //}
            // NB: do NOT set firstTime here. The firstTime branch of the nav
            // loop does menuPosition += 1 (that's how a fresh menu open walks
            // from -1 onto item 0), so setting it after a back-step advanced
            // the cursor off the restored item and onto its next sibling —
            // popping out of a submenu landed you one entry past the one you
            // entered from. This handler renders and recomputes the submenu
            // bounds itself, so nothing else is needed.

            for ( int i = menuLevel + 1; i < 10; i++ ) {
                previousMenuSelection[ i ] = -1;
            }

            if ( menuLevel > 1 ) {
                menuLevel -= 1;
                // b.clear();
                // return 0;
            } else if ( menuLevel == 1 ) {
                menuLevel = 0;
                // menuPosition = 0;
            } else {
                b.clear( );
                return -2;
            }
            // Serial.print("Menu Level: ");
            // Serial.println(menuLevel);

            if ( menuLevel == 0 ) {
                subMenuStartIndex = 0;
                subMenuEndIndex = menuLineIndex - 1;
            } else {

                subMenuEndIndex = menuLineIndex;

                for ( int i = menuPosition + 1; i <= menuLineIndex; i++ ) {

                    if ( menuLevels[ i ] < menuLevel ) {
                        subMenuEndIndex = i;
                        break;
                    }
                }

                for ( int i = subMenuEndIndex - 1; i > 0; i-- ) {
                    if ( menuLevels[ i ] < menuLevel ) {
                        subMenuStartIndex = i;
                        // Serial.print("\n\rsubMenuStartIndex: ");
                        // Serial.println(subMenuStartIndex);
                        // Serial.print("subMenuEndIndex: ");
                        // Serial.println(subMenuEndIndex);
                        break;
                    }
                }
            }

            // Land on the right entry for this level, then render BOTH panels and
            // flush. The old back path only painted the breadboard buffer (and only
            // when previousMenuSelection != -1) — it never set showLEDsCore2 and
            // never touched the OLED, so after "back" the breadboard could stay dark
            // and the OLED kept the old line. renderMenuLine() fixes both.
            bool twoLine = ( stayOnTopLevel != -1 && stayOnTopIndex != -1 &&
                             menuLevel != stayOnTopLevel );
            if ( previousMenuSelection[ menuLevel ] == -1 ) {
                // No remembered pick at this level (e.g. restored from a
                // timeout straight into a deep level). subMenuStartIndex is
                // the parent/boundary line, not a sibling — land on the first
                // real entry at this level. (The old firstTime=1 workaround used to
                // paper over this by re-scanning, at the cost of advancing the
                // cursor one item past the restored position on every back.)
                menuPosition = subMenuStartIndex;
                for ( int i = subMenuStartIndex; i <= subMenuEndIndex && i <= menuLineIndex; i++ ) {
                    if ( menuLevels[ i ] == menuLevel ) {
                        menuPosition = i;
                        break;
                    }
                }
            } else if ( !twoLine ) {
                menuPosition = previousMenuSelection[ menuLevel ];
            }

            String backLine = menuLines[ menuPosition ];
            backLine.replace( "~", "±" );
            backLine.replace( "_", "-" );
            backLine.replace( "±", "+-" );
            renderMenuLine( menuPosition, menuLevel, stayOnTopLevel, stayOnTopIndex,
                            backLine.c_str( ), 2 );
            noInputTimer = millis( );

            // Deliberately do NOT consume encoderButtonState here: leaving it
            // in HELD (and its MEDIUM/LONG escalations) keeps the hold/reboot
            // animation visible inside menus — renderLogoRing() hands the logo
            // back to the sweep while those states are live, and
            // holdAnimationStuff() needs the state to progress. The
            // HOLD_BACK_REPEAT_MS gate above paces back-steps by time, so the
            // persistent state can't re-trigger early. On release the state
            // machine reports RELEASED with lastButtonEncoderState == HELD,
            // which the click-descend check ignores — no phantom descend.
        }
    }

    return -1;
}

uint32_t nodeSelectionColors[ 10 ] = {
    0x0f0700,
    0x00090f,
    0x0a000f,
    0x050d00,
    0x100500,
    0x000411,
    0x100204,
    0x020f02,
};
uint32_t nodeSelectionColorsHeader[ 10 ] = {
    0x151000,
    0x00153f,
    0x0e003f,
    0x0f2d03,
    0x180d00,
    0x0000af,
    0x1a004f,
    0x061f29,
};

// Safe "does the menu line picked at this level contain this text" lookup.
// previousMenuSelection[] entries are -1 when nothing was picked, and callers
// historically indexed with menuLevel-2 (negative at shallow levels) — both
// walked off menuLines[] / previousMenuSelection[] and read garbage Strings.
static bool pickedLineContains( int level, const char* needle ) {
    if ( level < 0 || level >= 10 ) {
        return false;
    }
    int sel = previousMenuSelection[ level ];
    if ( sel < 0 || sel > menuLineIndex ) {
        return false;
    }
    return menuLines[ sel ].indexOf( needle ) != -1;
}

int selectSubmenuOption( int menuPosition, int menuLevel ) {

    int railMenu = 0;
    // 2 detents per option step at ~4 raw quadrature counts per physical
    // detent — the option carousel consumes encoderDirectionState, whose
    // pacing is exactly rotaryDivider raw counts per UP/DOWN. One-detent
    // stepping (divider 4) made the short option lists feel hair-trigger.
    rotaryDivider = 8;
    delayMicroseconds( 3000 );
    int optionSelected = -1;
    int highlightedOption = 1;
    // Serial.println("\n\r");
    String subMenuStrings[ 8 ];
    int menuOptionLengths[ 8 ];
    int maxMenuOptionLength = 0;
    int maxExists = -1;
    // int showFonts = 0;
    for ( int i = 0; i < 8; i++ ) {
        subMenuStrings[ i ] = "";
    }

    // Serial.println("selectSubmenuOption");
    // delay(5000);
    int shiftStars = -2;
    int lastOption = 0;
    for ( int i = 0; i < 8; i++ ) {
        int optionStart = -1;
        int optionEnd = -1;

        for ( int j = lastOption; j < 32; j++ ) {
            if ( bitRead( optionSlpitLocations[ menuPosition ], j ) == 1 ) {
                if ( optionStart == -1 ) {
                    optionStart = j;
                    shiftStars++;
                } else {
                    optionEnd = j;
                    shiftStars++;
                    lastOption = j + 1;
                    break;
                }
            }
        }

        if ( optionStart != -1 && optionEnd != -1 ) {
            // Serial.print("DEBUG substring: menuPos=");
            // Serial.print(menuPosition);
            // Serial.print(" optionStart=");
            // Serial.print(optionStart);
            // Serial.print(" optionEnd=");
            // Serial.print(optionEnd);
            // Serial.print(" shiftStars=");
            // Serial.print(shiftStars);
            // Serial.print(" menuLine='");
            // Serial.print(menuLines[menuPosition]);
            // Serial.print("'");
            // Serial.println();

            subMenuStrings[ i ] = menuLines[ menuPosition ].substring(
                optionStart - shiftStars, optionEnd - shiftStars - 1 );

            // Serial.print("DEBUG extracted subMenuStrings[");
            // Serial.print(i);
            // Serial.print("]='");
            // Serial.print(subMenuStrings[i]);
            // Serial.println("'");
            // Serial.flush();

            if ( subMenuStrings[ i ].length( ) > maxMenuOptionLength ) {
                menuOptionLengths[ i ] = subMenuStrings[ i ].length( );
                if ( strcasecmp( subMenuStrings[ i ].c_str( ), "max " ) == 0 ) {
                    maxExists = i;
                } else {
                    maxMenuOptionLength = subMenuStrings[ i ].length( );
                }
            }
        }
    }
    // Serial.println(" ");
    // for (int i = 0; i < 8; i++) {
    //   if (subMenuStrings[i] != "") {

    //     Serial.print(subMenuStrings[i]);
    //     Serial.print(" ");
    //   }
    // }
    uint32_t subMenuColors[ 10 ] = {
        0x000f02,
        0x000a03,
        0x00030a,
        0x040010,
        0x100001,
        0x0d0200,
        0x080900,
        0x030e00,
    };

    uint32_t subMenuColorsHeader[ 10 ] = {
        0x001f09,
        0x001508,
        0x00061f,
        0x070020,
        0x200005,
        0x120600,
        0x0f1200,
        0x061200,
    };

    //   uint32_t subMenuColors[10] = {
    //     0x010f00, 0x000a03, 0x00030a, 0x040010,
    //     0x070006, 0x09000a, 0x0f0004, 0x080800,
    // };

    //   uint32_t nodeSelectionColors[10] = {
    //     0x0f0700, 0x00090f, 0x0a000f, 0x050d00,
    //     0x100500, 0x000411, 0x100204, 0x020f02,
    // };
    uint32_t selectColor = subMenuColors[ ( menuLevel + 5 ) % 8 ];
    uint32_t backgroundColor = 0xffffff; // 0x0101000;
    int changed = 1;

    String choiceLine = menuLines[ menuPosition ];
    int brightnessMenu = 0;
    int cut = 0;
    if ( choiceLine.length( ) > 7 ) {
        cut = 1;
    }

    int menuType =
        0; // 0 = cycle (numbers) 1 = show both options 2 = show one option

    if ( choiceLine.length( ) <= 7 && maxMenuOptionLength > 1 ) {
        menuType = 1;
    } else if ( maxMenuOptionLength > 2 && choiceLine.length( ) > 7 ) {
        menuType = 2;
    }
    // if (previousMenuSelection[1] != -1) {
    // Serial.print("previousMenuSelection[1]: ");

    // Serial.print(previousMenuSelection[1]);
    // Serial.print(" ");
    // Serial.println(menuLines[previousMenuSelection[1]]);
    // }

    if ( pickedLineContains( 1, "Load" ) ) {
        // Serial.println("Load");
        menuType = 3;
    }

    if ( pickedLineContains( menuLevel - 2, "Bright" ) && pickedLineContains( menuLevel - 1, "Menu" ) ) {

        brightnessMenu = 1;
    }

    if ( pickedLineContains( menuLevel - 2, "Bright" ) && pickedLineContains( menuLevel - 1, "Rails" ) ) {

        brightnessMenu = 2;
    }

    if ( pickedLineContains( menuLevel - 2, "Bright" ) && pickedLineContains( menuLevel - 1, "Wires" ) ) {

        brightnessMenu = 3;
    }

    if ( pickedLineContains( menuLevel - 2, "Bright" ) && pickedLineContains( menuLevel - 1, "Special" ) ) {

        brightnessMenu = 4;
    }

    // if (menuLines[previousMenuSelection[menuLevel - 1]].indexOf("Font") != -1) {
    //   Serial.println("Font\n\r");
    //   showFonts = 1;
    //   }

    // Serial.print("menuType: ");
    // Serial.println(menuType);
    // Serial.println("selected Submenu Option\n\r");

    encoderButtonState = IDLE;
    int lastBrightness = menuBrightnessSetting;
    int firstTime = 1;
    delayMicroseconds( 1000 );
    while ( optionSelected == -1 ) {
        // rotaryEncoderStuff();
        rotaryEncoderButtonStuff( );
        jOS.serviceInner( );
        delayMicroseconds( 1000 );

        if ( encoderButtonState == HELD || Serial.available( ) > 0 || ProbeButton::getInstance( ).getButtonState( ) == 1 ) {
            /// Serial.println("selectSubmenuOption: HELD");
            b.clear( );
            SlotManager& mgr = SlotManager::getInstance( );
            if ( mgr.isPreviewMode( ) ) {
                String errorMsg;
                mgr.exitPreview( false, errorMsg ); // Cancel preview
            }

            requestLedShow( 1 );
            return -1;
        }
        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) || ProbeButton::getInstance( ).getButtonPress( ) == 2 ) {

            encoderButtonState = IDLE;
            optionSelected = highlightedOption;

            for ( int i = 0; i < 10; i++ ) {
                alreadySelected = 0;
                if ( currentAction.from[ i ] != -1 &&
                     currentAction.from[ i ] == optionSelected ) {
                    alreadySelected = 1;
                    break;
                }
            }
            if ( alreadySelected == 0 ) {
                currentAction.from[ currentAction.connectIndex ] = optionSelected;
            }

            // char menuBuffer[20];
            // Serial.print("Selected: ");
            // Serial.println(subMenuStrings[optionSelected]);
            if ( subMenuStrings[ optionSelected ].indexOf( "V" ) != -1 ) {
                if ( subMenuStrings[ optionSelected ].indexOf( "3" ) != -1 ) {
                    optionVoltage = 3;
                    currentAction.optionVoltage = 3;
                } else if ( subMenuStrings[ optionSelected ].indexOf( "5" ) != -1 ) {
                    optionVoltage = 5;
                    currentAction.optionVoltage = 5;
                } else if ( subMenuStrings[ optionSelected ].indexOf( "8" ) != -1 ) {
                    optionVoltage = 8;
                    currentAction.optionVoltage = 8;
                }

            } else {
                if ( alreadySelected == 0 ) {
                    // Store the option text - use optionSelected as index to match how it's read later
                    subMenuStrings[ optionSelected ].toCharArray(
                        currentAction.fromAscii[ optionSelected ], 10, 0 );
                }
            }

            // currentAction.fromAscii[currentAction.connectIndex][0] = menuBuffer;

            // rotaryDivider = 8;

            return optionSelected;
            changed = 1;
        } else if ( encoderDirectionState == UP ) {
            encoderDirectionState = NONE;
            highlightedOption += 1;
            if ( highlightedOption > numberOfChoices[ menuPosition ] - 1 ) {
                highlightedOption = 0;
            }
            changed = 1;

        } else if ( encoderDirectionState == DOWN || firstTime == 1 ) {
            encoderDirectionState = NONE;
            highlightedOption -= 1;
            if ( highlightedOption < 0 ) {
                highlightedOption = numberOfChoices[ menuPosition ] - 1;
            }
            firstTime = 0;
            changed = 1;
        }
        if ( changed == 1 ) {

            // Bracket this repaint so the configured frame transition (possibly
            // OFF) fires between option rows, same as the main menu lines.
            // Slot preview (menuType 3) is excluded: it shows net colors via the
            // rails==1 path, which the transition engine doesn't own.
            bool fxBracket = ( menuType != 3 );
            if ( fxBracket ) {
                menuTransitionBeginDraw( );
            }

            b.clear( 1 );

            if ( menuType == 0 ) {

                if ( brightnessMenu > 0 ) {
                    // Serial.println();
                    // Serial.println(highlightedOption);
                    // Serial.println();
                    if ( brightnessMenu == 1 ) {
                        switch ( highlightedOption ) {
                        case 0:
                            menuBrightnessSetting = -70;
                            break;
                        case 1:
                            menuBrightnessSetting = -55;
                            break;
                        case 2:
                            menuBrightnessSetting = -45;
                            break;
                        case 3:
                            menuBrightnessSetting = -35;
                            break;
                        case 4:
                            menuBrightnessSetting = -15;
                            break;
                        case 5:
                            menuBrightnessSetting = -5;
                            break;
                        case 6:
                            menuBrightnessSetting = 0;
                            break;
                        case 7:
                            menuBrightnessSetting = 30;
                            break;
                        case 8:
                            menuBrightnessSetting = 60;
                            break;
                        case 9:
                            menuBrightnessSetting = 60;
                            break;
                        default:
                            menuBrightnessSetting = 0;
                            break;
                        }

                        b.clear( );
                        b.print( "B", scaleBrightness( menuColors[ 0 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 0, 0, 3 );
                        b.print( "r", scaleBrightness( menuColors[ 1 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 1, 0, 3 );
                        b.print( "i", scaleBrightness( menuColors[ 2 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 2, 0, 3 );
                        b.print( "g", scaleBrightness( menuColors[ 3 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 3, 0, 3 );
                        b.print( "h", scaleBrightness( menuColors[ 4 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 4, 0, 3 );
                        b.print( "t", scaleBrightness( menuColors[ 5 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 5, 0, 3 );

                        b.printMenuReminder( menuLevel, scaleBrightness( menuColors[ menuLevel ], menuBrightnessSetting ) );

                    } else {
                        uint32_t scaledColor[ 6 ];
                        hsvColor scaledColorHsv[ 6 ];
                        switch ( brightnessMenu ) {
                        case 2:
                            LEDbrightnessRail = (int)( highlightedOption * 1.35 ) + 50;
                            jumperlessConfig.display.rail_brightness = LEDbrightnessRail;

                            // for (int i = 0; i < 6; i++) {
                            //   scaledColorHsv[i] = RgbToHsv(menuColors[i]);
                            //   scaledColorHsv[i].v = LEDbrightnessRail;
                            //   scaledColor[i] = HsvToRaw(scaledColorHsv[i]);
                            // }

                            b.clear( );
                            b.print( "B", scaleBrightness( menuColors[ 0 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 0, 0, 3 );
                            b.print( "r", scaleBrightness( menuColors[ 1 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 1, 0, 3 );
                            b.print( "i", scaleBrightness( menuColors[ 2 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 2, 0, 3 );
                            b.print( "g", scaleBrightness( menuColors[ 3 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 3, 0, 3 );
                            b.print( "h", scaleBrightness( menuColors[ 4 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 4, 0, 3 );
                            b.print( "t", scaleBrightness( menuColors[ 5 ], menuBrightnessOptionMap[ highlightedOption ] ), 0xffffff, 5, 0, 3 );

                            b.printMenuReminder( menuLevel, scaleBrightness( menuColors[ menuLevel ], menuBrightnessSetting ) );

                            break;
                        case 3:
                            LEDbrightness = highlightedOption * 3 + 3;
                            jumperlessConfig.display.led_brightness = LEDbrightness;

                            for ( int i = 0; i < 6; i++ ) {
                                scaledColorHsv[ i ] = RgbToHsv( menuColors[ i ] );
                                scaledColorHsv[ i ].v = LEDbrightness;
                                scaledColor[ i ] = HsvToRaw( scaledColorHsv[ i ] );
                            }

                            b.clear( );
                            b.print( "B", scaledColor[ 0 ], 0xffffff, 0, 0, 3 );
                            b.print( "r", scaledColor[ 1 ], 0xffffff, 1, 0, 3 );
                            b.print( "i", scaledColor[ 2 ], 0xffffff, 2, 0, 3 );
                            b.print( "g", scaledColor[ 3 ], 0xffffff, 3, 0, 3 );
                            b.print( "h", scaledColor[ 4 ], 0xffffff, 4, 0, 3 );
                            b.print( "t", scaledColor[ 5 ], 0xffffff, 5, 0, 3 );
                        case 4:
                            LEDbrightnessSpecial = highlightedOption * 5 + 5;
                            jumperlessConfig.display.special_net_brightness = LEDbrightnessSpecial;

                            for ( int i = 0; i < 6; i++ ) {
                                scaledColorHsv[ i ] = RgbToHsv( menuColors[ i ] );
                                scaledColorHsv[ i ].v = LEDbrightnessSpecial;
                                scaledColor[ i ] = HsvToRaw( scaledColorHsv[ i ] );
                            }

                            b.clear( );
                            b.print( "B", scaledColor[ 0 ], 0xffffff, 0, 0, 3 );
                            b.print( "r", scaledColor[ 1 ], 0xffffff, 1, 0, 3 );
                            b.print( "i", scaledColor[ 2 ], 0xffffff, 2, 0, 3 );
                            b.print( "g", scaledColor[ 3 ], 0xffffff, 3, 0, 3 );
                            b.print( "h", scaledColor[ 4 ], 0xffffff, 4, 0, 3 );
                            b.print( "t", scaledColor[ 5 ], 0xffffff, 5, 0, 3 );
                            break;
                        }
                    }
                    // b.print("Bright" , menuColors[menuLevel-1],
                    //         0xFFFFFF, 0, -1, 3);
                    // b.printMenuReminder(menuLevel, menuColors[menuLevel]);

                    // b.print("n", menuColors[6], 0xffffff, 1, 1, 2);
                    // b.print("e", menuColors[4], 0xffffff, 2, 1, 2);
                    // b.print("s", menuColors[2], 0xffffff, 3, 1, 2);
                    // b.print("s", menuColors[0], 0xffffff, 4, 1, 2);

                    if ( highlightedOption == 0 ) { //!
                        selectColor = subMenuColors[ ( menuLevel + 5 ) % 8 ] & 0x030303;
                    } else if ( highlightedOption == 1 ) {
                        selectColor = subMenuColors[ ( menuLevel + 5 ) % 8 ] & 0x070707;
                    }

                    else {
                        selectColor = subMenuColors[ ( menuLevel + 5 ) % 8 ] *
                                      ( ( ( highlightedOption - 1 ) ) );
                    }
                    menuBrightnessSetting = lastBrightness;
                }

                b.print( subMenuStrings[ highlightedOption ].c_str( ), selectColor,
                         backgroundColor, 3, 1, 0 );

                // Serial.println(selectColor, HEX);
                int start = highlightedOption;
                int loopCount = 0;
                int nudge = 0;
                int moveMax = 0;
                Serial.print( "\r                          \r" );
                for ( int j = 0; j <= menuLevels[ menuPosition ]; j++ ) {
                    Serial.print( ">" );
                    if ( j > 8 ) {

                        break;
                    }
                }
                Serial.print( " " );

                // Serial.print(subMenuStrings[highlightedOption]);

                // if (showFonts == 1) {

                //   Serial.println(highlightedOption);
                //   oled.setFont(highlightedOption);
                //   }

                String menuLine = subMenuStrings[ highlightedOption ];
                menuLine.replace( "~", "±" );
                menuLine.replace( "_", "-" );

                // Add the previous menu level prefix to menuLine if available
                if ( menuLevel > 0 && previousMenuSelection[ menuLevel - 1 ] != -1 ) {
                    String prefix = menuLines[ previousMenuSelection[ menuLevel - 1 ] ];
                    prefix += " ";
                    menuLine = prefix + menuLine;
                }

                Serial.print( menuLine.c_str( ) );
                menuLine.replace( "±", "+-" );

                oled.clearPrintShow( menuLine.c_str( ), 2, true, true, true, 5, 8 );
                Serial.flush( );

                for ( int i = 0; i < 7; i++ ) {

                    if ( i != ( 3 ) ) {
                        if ( i < 3 ) {
                            nudge = -1;
                        } else {
                            nudge = 1;
                        }
                        if ( maxExists == ( i + highlightedOption + 5 ) % 8 ) {

                            // nudge = 5;
                        }

                        b.print( subMenuStrings[ ( i + highlightedOption + 5 ) % 8 ].c_str( ),
                                 subMenuColors[ menuLevel ], 0xFFFFFF, i, 1, nudge, 1 );
                        // Serial.print(subMenuStrings[(i + highlightedOption + 5) % 8]);
                    } else {
                        // Serial.print("\b");
                        // Serial.println((i + highlightedOption + 5) % 8);
                        // Serial.println(subMenuStrings[(i + highlightedOption + 5) % 8]);
                        //  Serial.print("\r                          \r");
                        //  Serial.print(subMenuStrings[(i + highlightedOption + 5) % 8]);
                        //  Serial.flush();
                    }
                }

            } else if ( menuType == 1 ) {

                int menuStartPositions[ 8 ] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                for ( int i = 0; i < 8; i++ ) {
                    menuStartPositions[ i ] = 0;
                }
                int nextStart = 0;
                // selectColor = 0x1a001a;

                Serial.print( "\r                        \r" );
                for ( int j = 0; j <= menuLevels[ menuPosition ]; j++ ) {
                    Serial.print( ">" );
                    if ( j > 8 ) {

                        break;
                    }
                }
                Serial.print( " " );
                // Serial.print(subMenuStrings[highlightedOption]);

                if ( actions[ menuPosition ] == 6 ) {
                    oled.setFont( highlightedOption );
                }

                String menuLine = subMenuStrings[ highlightedOption ];
                menuLine.replace( "~", "±" );
                menuLine.replace( "_", "-" );

                // Add the previous menu level prefix to menuLine if available
                if ( menuLevel > 0 && previousMenuSelection[ menuLevel - 1 ] != -1 ) {
                    String prefix = menuLines[ previousMenuSelection[ menuLevel - 1 ] ];
                    prefix += " ";
                    menuLine = prefix + menuLine;
                }

                Serial.print( menuLine.c_str( ) );
                menuLine.replace( "±", "+-" );
                oled.clearPrintShow( menuLine.c_str( ), 2, true, true, true, 5, 8 );

                for ( int i = 0; i < numberOfChoices[ menuPosition ]; i++ ) {

                    if ( i == highlightedOption ) {
                        b.printRawRow( 0b00001010, ( nextStart * 4 ) + 30, selectColor,
                                       0xffffff );
                        b.printRawRow( 0b00000100, ( nextStart * 4 ) + 31, selectColor,
                                       0xffffff );
                        b.print( subMenuStrings[ i ].c_str( ), selectColor, 0xffffff, nextStart,
                                 1, 1 );
                        //                    Serial.print(subMenuStrings[i]);
                        // Serial.print(" ");
                    } else {
                        b.print( subMenuStrings[ i ].c_str( ), subMenuColors[ menuLevel ],
                                 0xffffff, nextStart, 1, 1 );
                    }

                    nextStart += subMenuStrings[ i ].length( ) + 1;

                    // Serial.println(subMenuStrings[i].length());
                }

            } else if ( menuType == 2 ) {

                // Serial.println(subMenuColors[(highlightedOption + menuLevel) % 8],
                // HEX);
                b.printRawRow( 0b00001010, 30, selectColor, 0xffffff );
                b.printRawRow( 0b00000100, 31, selectColor, 0xffffff );
                b.print( subMenuStrings[ highlightedOption ].c_str( ),
                         subMenuColors[ ( highlightedOption + menuLevel ) % 8 ],
                         backgroundColor, 0, 1, 1 );

                Serial.print( "\r                              \r" );
                oled.clear( );
                for ( int j = 0; j <= menuLevels[ menuPosition ]; j++ ) {
                    Serial.print( ">" );
                    if ( j > 8 ) {

                        break;
                    }
                }
                Serial.print( " " );
                // Serial.print(subMenuStrings[highlightedOption]);

                String menuLine = subMenuStrings[ highlightedOption ];
                menuLine.replace( "~", "±" );
                menuLine.replace( "_", "-" );

                if ( menuLine.startsWith( "Top" ) ) {
                    menuLine.replace( "'", "↑" );
                    menuLine.replace( "Top R", "Top Rail" );
                    brightenedRail = 0;
                } else if ( menuLine.startsWith( "Bot" ) ) {
                    menuLine.replace( ",", "↓" );
                    menuLine.replace( "Bot R", "Bottom Rail" );
                    brightenedRail = 2;
                } else {
                    brightenedRail = -1;
                }

                if ( menuLine.startsWith( "DAC 0" ) ) {
                    DACcolorOverride0 = -2;
                    DACcolorOverride1 = -1;
                } else if ( menuLine.startsWith( "DAC 1" ) ) {
                    DACcolorOverride1 = -2;
                    DACcolorOverride0 = -1;
                } else {
                    DACcolorOverride0 = -1;
                    DACcolorOverride1 = -1;
                }

                // // Add the previous menu level prefix to menuLine if available
                // if (menuLevel > 0 && previousMenuSelection[menuLevel - 1] != -1) {
                //   String prefix = menuLines[previousMenuSelection[menuLevel - 1]];
                //   prefix += " ";
                //   menuLine = prefix + menuLine;
                // }

                Serial.print( menuLine.c_str( ) );
                // Serial.println();
                Serial.flush( );

                menuLine.replace( "±", "+-" );
                oled.clearPrintShow( menuLine.c_str( ), 2, true, true, true, 5, 8 );

            } else if ( menuType == 3 ) { //! Slot Preview
                // Serial.println(subMenuColors[(highlightedOption + menuLevel) % 8],

                // Serial.println(highlightedOption);
                if ( highlightedOption < 0 ) {
                    highlightedOption = 0;
                } else if ( highlightedOption > 7 ) {
                    highlightedOption = 7;
                }

                // lightUpRail();
                // showSavedColors(highlightedOption);
                previewSlotColors( highlightedOption, false );
                // showNets();

                leds.setPixelColor( bbPixelToNodesMapV5[ highlightedOption + 18 ][ 1 ],
                                    nodeSelectionColorsHeader[ highlightedOption ] );

                if ( SlotManager::getInstance( ).slotExists( highlightedOption ) == false || globalState.connections.numBridges <= 1 ) {
                    // Serial.println("slotExists: " + String(SlotManager::getInstance( ).slotExists( highlightedOption )));
                    // Serial.println("numBridges: " + String(globalState.connections.numBridges));
                    // Serial.println("bridges[0][0]: " + String(globalState.connections.bridges[0][0]));
                    // Serial.println("bridges[0][1]: " + String(globalState.connections.bridges[0][1]));
                    // Serial.flush();
                    if ( ( ( globalState.connections.numBridges == 1 ) && ( globalState.connections.bridges[ 0 ][ 0 ] == ROUTABLE_BUFFER_IN || globalState.connections.bridges[ 0 ][ 1 ] == ROUTABLE_BUFFER_IN ) ) || globalState.connections.numBridges == 0 ) {

                        b.print( subMenuStrings[ highlightedOption ].c_str( ),
                                 nodeSelectionColors[ highlightedOption ], 0xFFFFFD, 3, 1, -1, 0 );
                    }
                }

                // Preview mode handled via SlotManager::isPreviewMode() in core2stuff
            }

            if ( fxBracket ) {
                menuTransitionArm( );
            }

            // CRITICAL FIX: Signal Core 2 to display the menu buffer that was written by b.print()
            // Without this, the menu text is written but never shown on the LEDs.
            // Slot preview (menuType 3) is the exception: it needs the previewed slot's
            // NET colors on the breadboard, but core2stuff() skips showNets() whenever
            // rails==2 — so =2 would defeat SlotManager::isPreviewMode() and leave the
            // breadboard showing menu text instead of the slot. Use =1 there so nets render.
            requestLedShow( ( menuType == 3 ) ? 1 : 2 );
            changed = 0;
        }

        // b.print(choiceLine.c_str(), subMenuColors[menuLevel], 0xFFFFFF, 0, 1,
        //         numberOfChoices[menuPosition] > 7 ? 1 : 3);

        // delay(6800);
    }
    // Serial.println(highlightedOption);
    return optionSelected;
}

int yesNoMenu( unsigned long timeout, int startOption ) {
    inClickMenu = 1;

    rotaryDivider = 8; // one option step per physical detent (8 counts)
    // delayMicroseconds(3000);
    int optionSelected = -1;
    // Which option a bare CLICK answers. 0 = No (the historical default and
    // what every pre-existing caller passes); 1 = Yes, for prompts whose
    // wording promises "click = yes".
    int highlightedOption = ( startOption == 1 ) ? 1 : 0;
    int changed = 0;
    uint32_t selectColor = 0x1a001a;
    uint32_t yesColor = 0x001004;
    uint32_t yesColorBright = 0x001f0f;
    uint32_t noColor = 0x100003;
    uint32_t noColorBright = 0x1f0008;
    uint32_t backgroundColor = 0x000002;
    unsigned long startTime = millis( );
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    // Display initial state immediately before entering loop. Mirrors the
    // in-loop redraw below so startOption == 1 really SHOWS Yes selected -
    // a hardcoded "No" here would make the prompt lie about what a click does.
    Serial.print( "\r                      \r" );
    menuTransitionBeginDraw( );
    b.clear( 1 );
    if ( highlightedOption == 1 ) {
        Serial.print( "Yes" );
        b.print( ">", yesColorBright, 0x0, 0, 1, -2 );
        b.print( "Yes", yesColorBright, 0x0, 1, 1, -2 );
        b.print( "No", noColor, 0x0, 5, 1, -1 );
    } else {
        Serial.print( "No" );
        b.print( ">", noColorBright, 0x0, 4, 1, -1 );
        b.print( "Yes", yesColor, 0x0, 1, 1, -2 );
        b.print( "No", noColorBright, 0x0, 5, 1, -1 );
    }
    menuTransitionArm( );
    delay( 100 );
    requestLedShow( 2 );

    while ( optionSelected == -1 ) {
        jOS.serviceInner( );
        rotaryEncoderButtonStuff( ); // Update button state

        if ( millis( ) - startTime > timeout ) {
            inClickMenu = 0;
            return -1;
        }

        if ( Serial.available( ) > 0 ) {
            char input = Serial.read( );
            if ( input == 'y' || input == 'Y' ) {
                highlightedOption = 1;
            } else if ( input == 'n' || input == 'N' ) {
                highlightedOption = 0;
            } else {
                // b.clear();
                inClickMenu = 0;
                return -1;
            }
            inClickMenu = 0;
            return highlightedOption;
        }

        delayMicroseconds( 1000 );
        
        if ( encoderButtonState == HELD ) {
            b.clear( );
            inClickMenu = 0;
            return -1;
        }
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) {

            encoderButtonState = IDLE;
            optionSelected = highlightedOption;
            inClickMenu = 0;
            return optionSelected;
        } else if ( encoderDirectionState == UP ) {
            encoderDirectionState = NONE;
            highlightedOption += 1;
            if ( highlightedOption > 1 ) {
                highlightedOption = 0;
            }
            changed = 1;
        } else if ( encoderDirectionState == DOWN ) {
            encoderDirectionState = NONE;
            highlightedOption -= 1;
            if ( highlightedOption < 0 ) {
                highlightedOption = 1;
            }
            changed = 1;
        }
        if ( changed == 1 ) {
            Serial.print( "\r                      \r" );

            menuTransitionBeginDraw( );
            b.clear( 1 );
            if ( highlightedOption == 1 ) {
                Serial.print( "Yes" );
                b.print( ">", yesColorBright, 0x0, 0, 1, -2 );
                b.print( "Yes", yesColorBright, 0x0, 1, 1, -2 );
                b.print( "No", noColor, 0x0, 5, 1, -1 );
            } else {
                Serial.print( "No" );
                b.print( ">", noColorBright, 0x0, 4, 1, -1 );
                b.print( "Yes", yesColor, 0x0, 1, 1, -2 );
                b.print( "No", noColorBright, 0x0, 5, 1, -1 );
            }
            menuTransitionArm( );

            requestLedShow( 2 );

            changed = 0;
        }
    }
    inClickMenu = 0;
    return optionSelected;
}

int selectNodeAction( int whichSelection ) {
    // This screen paints the breadboard without the transition bracket —
    // abort any in-flight menu transition so its blend frames can't stamp
    // the old menu line over what we draw here.
    menuTransitionCancel( );

    b.clear( );
    requestLedShow( -1 );

    int nodeSelected = -1;
    int currentlySelecting = whichSelection;

    int highlightedNode = currentlySelecting + 13;

    int firstTime = 1;
    int inNanoHeader = 0;

    uint8_t middle = 0b00001110;
    uint32_t middleColor = 0x121215;
    uint8_t hatch = 0b00011111;
    uint32_t overlappingColor = 0xffffff;

    if ( subMenuChoices[ currentlySelecting ] != -1 ) {
        if ( subMenuChoices[ currentlySelecting ] >= NANO_D0 &&
             subMenuChoices[ currentlySelecting ] <= NANO_RESET_0 ) {
            highlightedNode = subMenuChoices[ currentlySelecting ];
            inNanoHeader = 1;
        } else {
            highlightedNode = subMenuChoices[ currentlySelecting ] + 1;
        }
        subMenuChoices[ currentlySelecting ] = -1;
        maxNumSelections++;
    }

    // NOTE: this screen reads raw encoderPosition deltas directly, which
    // rotaryDivider does NOT pace (the divider only paces the
    // encoderDirectionState/UP-DOWN consumer path). The old code still
    // flapped rotaryDivider between 2 and 3 as "acceleration" — useless for
    // this path, and every divider change restarts the encoder PIO state
    // machine mid-scroll. Stepping is paced here instead, by detents.
    int lastDivider = rotaryDivider;
    delayMicroseconds( 300 );

    // Position-based tracking for direct encoder reading
    long lastEncoderPosition = encoderPosition;

    // ── Detent-accumulator stepping with speed tiers ──
    // Raw counts accumulate into detentAccum and each cursor step costs:
    //   speedLevel 0-1 (rest/slow): 2 detents per node — a stray count or
    //                               half-detent nudge never moves the cursor
    //   speedLevel 2-3 (turning):   1 detent per node
    //   speedLevel 4   (flick):     1 detent moves 2 nodes (max speed)
    // Steps committed close together raise the tier one notch each; a pause
    // (or direction flip) drops it back to slow. Two fast steps to reach
    // single-detent stepping, four to reach max — a deliberately gradual ramp.
    const int COUNTS_PER_DETENT = 2;
    const int NODE_SPEED_LEVEL_MAX = 4;
    const unsigned long NODE_ACCEL_STEP_MS = 100;  // step gaps below this accelerate
    const unsigned long NODE_ACCEL_RESET_MS = 350; // pauses above this reset to slow
    int detentAccum = 0;
    int speedLevel = 0;
    unsigned long lastStepTimeMs = millis( );

    unsigned long lastShowRequestMs = 0;

    while ( nodeSelected == -1 && Serial.available( ) == 0 ) {
        delayMicroseconds( 200 );
        // rotaryEncoderStuff( );
        jOS.serviceInner( );
        menuShowKeepalive( lastShowRequestMs );
        int probePress = ProbeButton::getInstance( ).getButtonPress( ); // one read per pass (see getMenuSelection)
        if ( encoderButtonState == HELD || Serial.available( ) > 0 || probePress == 1 ) {
            b.clear( );
            // Flush the cleared buffer back to nets, otherwise the node-selection
            // overlay lingers on the breadboard after cancelling.
            requestLedShow( -1 );
            rotaryDivider = lastDivider;
            return -1;
        }

        // Read encoder position directly for immediate response
        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        lastEncoderPosition = currentEncoderPosition;

        bool needsUpdate = false;
        if ( firstTime == 1 ) {
            needsUpdate = true;
            firstTime = 0;
        }

        if ( encoderDelta != 0 ) {
            // A direction flip discards progress toward the old direction
            // (and any built-up speed) so reversals are always deliberate.
            if ( detentAccum != 0 && ( encoderDelta > 0 ) != ( detentAccum > 0 ) ) {
                detentAccum = 0;
                speedLevel = 0;
            }
            detentAccum += (int)encoderDelta;

            // Speed decays back to the slow (2-detent) tier after a pause.
            if ( millis( ) - lastStepTimeMs > NODE_ACCEL_RESET_MS ) {
                speedLevel = 0;
            }

            int stepCost = COUNTS_PER_DETENT * ( speedLevel <= 1 ? 2 : 1 );
            int nodesPerStep = ( speedLevel >= NODE_SPEED_LEVEL_MAX ) ? 2 : 1;

            while ( detentAccum >= stepCost || detentAccum <= -stepCost ) {
                int dir = ( detentAccum > 0 ) ? 1 : -1;
                detentAccum -= dir * stepCost;

                // Blank the highlight we're moving off of. Only the nano
                // header needs this (the repaint below b.clear()s the
                // breadboard rows but never touches header pixels). The old
                // code did this unguarded — bbPixelToNodesMapV5[node - 70]
                // is an out-of-bounds read for breadboard rows (0-59).
                int mapIdx = highlightedNode - 70;
                if ( inNanoHeader == 1 && mapIdx >= 0 && mapIdx < 30 ) {
                    leds.setPixelColor( bbPixelToNodesMapV5[ mapIdx ][ 1 ], 0x000000 );
                }

                if ( dir < 0 ) {
                    // Moving down (counter-clockwise)
                    highlightedNode -= nodesPerStep;
                    if ( highlightedNode < 0 ) {
                        highlightedNode = NANO_RESET_0;
                        inNanoHeader = 1;
                    }
                    if ( highlightedNode < NANO_D0 && inNanoHeader == 1 ) {
                        highlightedNode = 59;
                        inNanoHeader = 0;
                    }
                } else {
                    // Moving up (clockwise)
                    highlightedNode += nodesPerStep;
                    if ( highlightedNode > 59 && inNanoHeader == 0 ) {
                        highlightedNode = NANO_D0;
                        inNanoHeader = 1;
                    }
                    if ( highlightedNode > NANO_RESET_0 ) {
                        highlightedNode = 0;
                        inNanoHeader = 0;
                        // Leaving the header: blank all header pixels — the
                        // breadboard repaint below doesn't touch them.
                        for ( int i = 0; i < 30; i++ ) {
                            leds.setPixelColor( bbPixelToNodesMapV5[ i ][ 1 ], 0x000000 );
                        }
                    }
                }

                // Steps committed close together raise the speed tier;
                // re-derive the costs so acceleration applies mid-batch.
                unsigned long now = millis( );
                if ( now - lastStepTimeMs < NODE_ACCEL_STEP_MS &&
                     speedLevel < NODE_SPEED_LEVEL_MAX ) {
                    speedLevel++;
                }
                lastStepTimeMs = now;
                stepCost = COUNTS_PER_DETENT * ( speedLevel <= 1 ? 2 : 1 );
                nodesPerStep = ( speedLevel >= NODE_SPEED_LEVEL_MAX ) ? 2 : 1;

                needsUpdate = true;
            }

            if ( needsUpdate ) {
                Serial.print( "\r                      \r" );
                Serial.print( ">>>> " );
                printNodeOrName( highlightedNode, 1 );
                oled.clearPrintShow( definesToChar( highlightedNode, 0 ), 2, true, true );
            }
        }

        // Update LED display if value changed
        if ( needsUpdate ) {
            int overlappingSelection = -1;
            int overlappingConnection = -1;

            b.clear( );
            showNets( );
            requestLedShow( 2 );

            if ( inNanoHeader == 1 ) {
                for ( int a = 0; a < 8; a++ ) {
                    if ( subMenuChoices[ a ] != -1 && subMenuChoices[ a ] >= NANO_D0 ) {
                        leds.setPixelColor( bbPixelToNodesMapV5[ subMenuChoices[ a ] - 70 ][ 1 ],
                                            nodeSelectionColorsHeader[ a ] );

                    } else if ( subMenuChoices[ a ] != -1 && subMenuChoices[ a ] < 60 ) {
                        b.printRawRow( 0b00000100, ( subMenuChoices[ a ] - 1 ), middleColor,
                                       nodeSelectionColors[ a ] );
                    }
                }
                leds.setPixelColor( bbPixelToNodesMapV5[ highlightedNode - 70 ][ 1 ],
                                    nodeSelectionColorsHeader[ currentlySelecting ] );

            } else {
                if ( leds.getPixelColor( ( highlightedNode * 5 ) + 3 ) != 0x000000 ) {
                    overlappingConnection = highlightedNode + 1;
                }
                for ( int a = 0; a < 8; a++ ) {
                    if ( subMenuChoices[ a ] != -1 && subMenuChoices[ a ] < 60 ) {
                        if ( subMenuChoices[ a ] == highlightedNode + 1 &&
                             subMenuChoices[ a ] < 60 ) {
                            hatch = 0b00010101;
                            overlappingColor = nodeSelectionColors[ a ];
                            overlappingSelection = a;
                        }

                        b.printRawRow( 0b00000100, ( subMenuChoices[ a ] - 1 ), middleColor,
                                       nodeSelectionColors[ a ] );
                    }
                }

                if ( overlappingConnection != -1 || overlappingSelection != -1 ) {
                    if ( highlightedNode <= 30 ) {
                        b.printRawRow( middle, ( highlightedNode ),
                                       leds.getPixelColor( ( ( highlightedNode ) * 5 ) + 4 ),
                                       nodeSelectionColors[ currentlySelecting ] );
                    } else {
                        b.printRawRow( middle, ( highlightedNode ),
                                       leds.getPixelColor( ( ( highlightedNode ) * 5 ) + 0 ),
                                       nodeSelectionColors[ currentlySelecting ] );
                    }
                    b.printRawRow( 0b00000100, ( highlightedNode + 1 ),
                                   nodeSelectionColors[ currentlySelecting ], 0x000000 );
                    b.printRawRow( 0b00000100, ( highlightedNode - 1 ),
                                   nodeSelectionColors[ currentlySelecting ], 0x000000 );
                } else {
                    b.printRawRow( 0b00000100, ( highlightedNode ), middleColor,
                                   nodeSelectionColors[ currentlySelecting ] );
                }
            }
            requestLedShow( 2 );
            lastShowRequestMs = millis( );
        }

        // Check for confirmation (short press)
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED || probePress == 2 ) {
            encoderButtonState = IDLE;
            nodeSelected = highlightedNode;

            if ( alreadySelected == 0 ) {
                currentAction.to[ currentAction.connectIndex ] = highlightedNode + 1;
                currentAction.connectIndex++;
            } else {
                for ( int i = 0; i < 10; i++ ) {
                    if ( currentAction.from[ i ] == currentlySelecting ) {
                        if ( nodeSelected > 0 && nodeSelected < 60 ) {
                            currentAction.to[ i ] = highlightedNode + 1;
                        } else {
                            currentAction.to[ i ] = highlightedNode;
                        }
                        break;
                    }
                }
            }
        }
    }

    // Restore rotary divider
    rotaryDivider = lastDivider;

    if ( nodeSelected <= 59 && nodeSelected >= 0 ) {
        return nodeSelected + 1;
    } else {
        return nodeSelected;
    }
}

float getActionFloat( int menuPosition, int rail ) {
    // This screen paints the breadboard without the transition bracket —
    // abort any in-flight menu transition so its blend frames can't stamp
    // the old menu line over what we draw here.
    menuTransitionCancel( );

    float currentChoice = -0.1;
    float snapValues[ 9 ] = { 1.0, 2.0, 3.0, 3.3, 4.0, 5.0, 6.0, 7.0 };

    int snap = 0; //! make this a config variable

    char floatString[ 8 ] = "0.0";
    rotaryDivider = 3;
    // b.clear( 1 );
    int firstTime = 1;
    int snapToValue = 0;

    uint32_t posColor = 0x090600;
    uint32_t negColor = 0x04000f;

    uint32_t threeColor = 0x00060e;
    uint32_t fiveColor = 0x140101;
    uint32_t maxColor = 0x100505;
    uint32_t zeroColor = 0x000e02;

    uint32_t fiveBlended = 0x0e0300;
    uint32_t threeBlended = 0x060f00;
    uint32_t zeroBlended = 0x060801;

    uint32_t numberColor = posColor;

    // Encoder acceleration tracking (using VoltageAdjuster approach)
    long lastEncoderPosition = encoderPosition;
    unsigned long lastChangeTime = millis( );
    float accelerationMultiplier = 1.0;
    int lastDirection = 0;        // -1=down, 0=none, 1=up
    int consecutiveFastCount = 0; // Track consecutive fast movements

    float min = -8.0;
    float max = 8.0;

    float roundedCurrentChoice = 0.0;
    // No need for temp backup - globalState.power should always match hardware
    switch ( rail ) {
    case 0:
        currentChoice = ( ( globalState.power.topRail + globalState.power.bottomRail ) / 2.0 );
        break;
    case 1:
        currentChoice = globalState.power.topRail;
        break;
    case 2:
        currentChoice = globalState.power.bottomRail;
        break;
    }

    // Snapshot the rails BEFORE the slider mutates them. The slider runs
    // a tight encoder/probe loop, applies new voltages on every tick,
    // and bypasses undo recording per-tick (the direct globalState writes
    // below leave prev==voltage so setRailVoltage's recording guard
    // skips it). At loop exit we record ONE undo entry capturing the
    // (initial -> final) jump - so a rail drag becomes a single undo
    // step. railCommitEdit() is a small lambda below the loop.
    //
    // IT ALSO OWNS THE DIRTY MARK, and must (w3-5). The pre-write above is
    // exactly the shape that hides an edit from setRailVoltage: by the time
    // setTopRail() -> setRailVoltage() runs, power.topRail ALREADY holds the
    // new value, so prev == voltage and the setter's guards - undo AND, since
    // w3-5's no-op gate, markDirty - both correctly conclude "nothing changed".
    // Nothing else in this file persists the drag (there is no other
    // markDirty/saveActiveSlot in Menus.cpp, and setTopRail's saveEEPROM arm is
    // dead code), so without the mark here a rail set from the encoder/probe
    // menu is lost on the next slot switch or reboot. Same change detection as
    // the undo record, so an untouched slider still dirties nothing.
    float undoInitialTopRail = globalState.power.topRail;
    float undoInitialBotRail = globalState.power.bottomRail;
    auto railCommitEdit = [&]() {
        if ( rail < 0 || rail > 2 ) return;  // not a rail context
        bool changedTop = ( rail == 0 || rail == 1 ) &&
                          undoInitialTopRail != globalState.power.topRail;
        bool changedBot = ( rail == 0 || rail == 2 ) &&
                          undoInitialBotRail != globalState.power.bottomRail;
        if ( !changedTop && !changedBot ) return;

        // The rails really moved - this is a user edit and must persist.
        globalState.markDirty();
        // Bundle both rail changes into a single transaction so the user gets
        // ONE undo step for "rails to 2.7V" rather than two (one per rail) when
        // rail==0. The label is derived from the recorded ops by undoEndTxn (a
        // top+bottom pair becomes "Rails <v>", a single rail "Top Rail <v>" /
        // "Bot Rail <v>"), so it's identical live and after a reboot - no
        // hand-built label here, which previously drifted from the rebuilt one.
        undoBeginTxn( nullptr, UNDO_SRC_MENU );
        if ( changedTop ) {
            undoRecordDacSet( 2, undoInitialTopRail, globalState.power.topRail );
        }
        if ( changedBot ) {
            undoRecordDacSet( 3, undoInitialBotRail, globalState.power.bottomRail );
        }
        undoEndTxn();
    };

    if ( currentAction.optionVoltage == 5 ) {
        min = 0.0;
        max = 5.0;
    } else if ( currentAction.optionVoltage == 3 ) {
        min = 0.0;
        max = 3.3;
    } else if ( currentAction.optionVoltage == 8 ) {
        min = -8.0;
        max = 8.0;
        
    }
    roundedCurrentChoice = roundf( currentChoice * 10.0f ) / 10.0f;

    unsigned long lastShowRequestMs = 0;

    while ( true ) {
        jOS.serviceInner( );
        menuShowKeepalive( lastShowRequestMs );
        // rotaryEncoderStuff();  // Update encoder and button state

        // Check for cancellation (long press) - check FIRST on every iteration
        if ( encoderButtonState == HELD || ProbeButton::getInstance( ).getButtonState( ) == 1 ) {
            encoderButtonState = IDLE;
            requestLedShow( -1 );
            // Long-press is a "leave the rails wherever the slider was last"
            // exit. The hardware/state still got mutated during the drag,
            // so we still need to record one undo step. NAN tells the
            // action handler NOT to re-apply (a cancel used to drive both
            // rails to the stale/zero analogVoltage and persist it - sweep).
            currentAction.analogVoltage = NAN;
            railCommitEdit();
            return roundedCurrentChoice; // Return current choice without applying
        }

        // Check for confirmation (short press) - check SECOND on every iteration
        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ProbeButton::getInstance( ).getButtonPress( ) == 2 ) {
            roundedCurrentChoice = roundf( currentChoice * 10.0f ) / 10.0f;
            encoderButtonState = IDLE;
            currentAction.analogVoltage = roundedCurrentChoice;

            int keepSelecting = 0;
            for ( int i = 0; i < 10; i++ ) {
                if ( selectMultiple[ i ] == menuPosition ) {
                    keepSelecting = 1;
                }
                if ( i >= selectMultipleIndex ) {
                    break;
                }
            }
            if ( keepSelecting == 1 ) {
                selectNodeAction( );
            }

            railCommitEdit();
            return roundedCurrentChoice;
        }

        // Check for serial cancellation
        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            requestLedShow( -1 );
            currentAction.analogVoltage = NAN;   // cancel = do not re-apply
            railCommitEdit();
            return roundedCurrentChoice;
        }

        // Read probe pads for direct voltage selection
        int probeReading = probing.justReadProbe( true );

        // Check if probe is touching the voltage selection area (rows 31-60)
        if ( probeReading >= 31 && probeReading <= 60 ) {
            // Map probe position to voltage value based on config range
            // Probe rows 31-60 (30 positions) map to min-max voltage
            float voltageRange = max - min;
            float normalizedPosition = ( probeReading - 31 ) / 29.0; // 0.0 to 1.0
            currentChoice = min + ( normalizedPosition * voltageRange );
            

            // Round to 0.1V increments
            // currentChoice = roundf(currentChoice * 10.0f) / 10.0f;

            // Clamp to limits
            if ( currentChoice > max )
                currentChoice = max;
            if ( currentChoice < min )
                currentChoice = min;

            // Clamp to DAC limits
            if ( currentChoice > jumperlessConfig.dacs.limit_max ) {
                currentChoice = jumperlessConfig.dacs.limit_max;
            } else if ( currentChoice < jumperlessConfig.dacs.limit_min ) {
                currentChoice = jumperlessConfig.dacs.limit_min;
            }
roundedCurrentChoice = roundf(currentChoice * 10.0f) / 10.0f;
            // Snap to zero
            // if ( currentChoice > -0.05 && currentChoice < 0.05 ) {
            //     currentChoice = 0.0;
            // }

            // Determine color
            uint32_t numberColor = posColor;
            if ( currentChoice > 0.05 ) {
                numberColor = posColor;
            }
            if ( currentChoice < 5.3 && currentChoice > 4.7 ) {
                numberColor = fiveBlended;
            } else if ( currentChoice < 3.45 && currentChoice > 3.05 ) {
                numberColor = threeBlended;
            } else if ( currentChoice < 0.35 && currentChoice > -0.35 ) {
                numberColor = zeroBlended;
            }
            if ( currentChoice > -0.05 && currentChoice < 0.05 ) {
                numberColor = zeroColor;
            } else if ( currentChoice > 3.25 && currentChoice < 3.35 ) {
                numberColor = threeColor;
            } else if ( currentChoice > 4.95 && currentChoice < 5.05 ) {
                numberColor = fiveColor;
            } else if ( currentChoice > 7.95 && currentChoice < 8.55 ) {
                numberColor = maxColor;
            } else if ( currentChoice < -0.05 ) {
                numberColor = negColor;
            }

            // Format and display
            if ( currentChoice < 0.00 ) {
                snprintf( floatString, 8, "%0.1f V", roundedCurrentChoice );
            } else {
                snprintf( floatString, 8, " %0.1f V", roundedCurrentChoice );
            }

            b.clear( 1 );
            b.print( floatString, numberColor, 0xffffff, 0, 1, 1 );
            Serial.print( "\r                        \r" );
            Serial.print( floatString );
            oled.clearPrintShow( floatString, 2, true, true, true );

            // Update global state immediately
            if ( rail == 0 ) {
                globalState.power.topRail = roundedCurrentChoice;
                globalState.power.bottomRail = roundedCurrentChoice;
            } else if ( rail == 1 ) {
                globalState.power.topRail = roundedCurrentChoice;
            } else if ( rail == 2 ) {
                globalState.power.bottomRail = roundedCurrentChoice;
            }

            // Apply voltage to hardware (only for safe range)
            if ( currentChoice >= 0.0 && currentChoice <= 5.0 ) {
                switch ( rail ) {
                case 0:
                    setTopRail( roundedCurrentChoice, 1, 0 );
                    setBotRail( roundedCurrentChoice, 1, 0 );
                    break;
                case 1:
                    setTopRail( roundedCurrentChoice, 1, 0 );
                    break;
                case 2:
                    setBotRail( roundedCurrentChoice, 1, 0 );
                    break;
                }
            }

            requestLedShow( 2 );

            // Reset encoder-based tracking since we're using probe now
            lastEncoderPosition = encoderPosition;
            accelerationMultiplier = 1.0;
            consecutiveFastCount = 0;
            lastDirection = 0;
            firstTime = 0;
            // Serial.print("currentChoice: ");
            // Serial.print(currentChoice);
            // Serial.println();
            // Serial.flush();
            continue; // Skip encoder processing this iteration
        }

        // Read encoder position directly for immediate response
        long currentEncoderPosition = encoderPosition;
        long encoderDelta = currentEncoderPosition - lastEncoderPosition;

        bool valueChanged = false;

        // Process encoder movement
        if ( encoderDelta != 0 || firstTime == 1 ) {
            // Handle snap delay
            if ( snapToValue > 0 && !firstTime ) {
                snapToValue--;
                lastEncoderPosition = currentEncoderPosition;
                continue;
            }

            // Remember where we were before applying this poll's delta, so the
            // zero-snap below only engages when we ARRIVE at 0 from outside the
            // window (not while resting in it). Otherwise slow steps get snapped
            // back to 0.0 every poll and the value is trapped at zero.
            float choiceBeforeDelta = currentChoice;

            if ( !firstTime ) {
                // Determine current direction
                int currentDirection = ( encoderDelta > 0 ) ? 1 : ( ( encoderDelta < 0 ) ? -1 : 0 );

                // Reset acceleration if direction changed
                if ( currentDirection != 0 && currentDirection != lastDirection ) {
                    accelerationMultiplier = 1.0;
                    consecutiveFastCount = 0;
                    lastDirection = currentDirection;
                }

                // Calculate acceleration based on delta magnitude and timing
                unsigned long currentTime = millis( );
                unsigned long timeSinceLastChange = currentTime - lastChangeTime;
                int deltaMagnitude = abs( encoderDelta );

                // Fast rotation = large delta between polls
                bool isFastRotation = ( deltaMagnitude >= 2 );

                if ( isFastRotation ) {
                    // Fast movement detected - increment consecutive count
                    consecutiveFastCount++;

                    // Only accelerate after 0 consecutive fast movements in same direction
                    if ( consecutiveFastCount >= 0 ) {
                        accelerationMultiplier += 3.5;
                        if ( accelerationMultiplier > 5.0 ) {
                            accelerationMultiplier = 7.0;
                        }
                    }
                } else if ( timeSinceLastChange > 120 ) {
                    // Slow/stopped - reset everything
                    accelerationMultiplier = 1.0;
                    consecutiveFastCount = 0;
                    lastDirection = 0;
                } else {
                    // Medium/slow speed - reset fast count but maintain acceleration
                    consecutiveFastCount = 0;
                }

                lastChangeTime = currentTime;

                // Calculate voltage change based on encoder delta
                // Base change per encoder click, scaled by acceleration
                float deltaMultiplier = 0.01 * accelerationMultiplier;
                float addToValue = encoderDelta * deltaMultiplier;

                // Apply the change (note: subtract because encoder direction)
                currentChoice -= addToValue;
                roundedCurrentChoice = roundf(currentChoice * 10.0f) / 10.0f;
                if ( roundedCurrentChoice == -0.0 ) {
                    roundedCurrentChoice = 0.0;
                }
               // firstTime = 0;
            }

            lastEncoderPosition = currentEncoderPosition;
            // Serial.print( "currentChoice: " );
            // Serial.print( currentChoice );
            // Serial.print( " currentEncoderPosition: " );
            // Serial.print( currentEncoderPosition );
            // Serial.print( " encoderDelta: " );
            // Serial.print( encoderDelta );
            // Serial.println( );
            // Serial.flush( );
            valueChanged = true;

            // Clamp to limits
            if ( currentChoice > max ) {
                currentChoice = max;
            } else if ( currentChoice < min ) {
                currentChoice = min;
            }

            // Clamp to DAC limits
            if ( currentChoice > jumperlessConfig.dacs.limit_max ) {
                currentChoice = jumperlessConfig.dacs.limit_max;
                // Serial.print("currentChoice > jumperlessConfig.dacs.limit_max: ");
                // Serial.print(currentChoice);
                // Serial.println();
                // Serial.flush();
            } else if ( currentChoice < jumperlessConfig.dacs.limit_min ) {
                currentChoice = jumperlessConfig.dacs.limit_min;
                // Serial.print("currentChoice < jumperlessConfig.dacs.limit_min: ");
                // Serial.print(currentChoice);
                // Serial.println();
                // Serial.flush();
            }
            roundedCurrentChoice = roundf(currentChoice * 10.0f) / 10.0f;
            if ( roundedCurrentChoice == -0.0 ) {
                roundedCurrentChoice = 0.0;
            }
            // Determine color based on voltage value (better color logic from original)
            if ( currentChoice > 0.05 ) {
                numberColor = posColor;
            }

            // Blended regions (approaching special values)
            if ( currentChoice < 5.3 && currentChoice > 4.7 ) {
                numberColor = fiveBlended;
            } else if ( currentChoice < 3.45 && currentChoice > 3.05 ) {
                numberColor = threeBlended;
            } else if ( currentChoice < 0.35 && currentChoice > -0.35 ) {
                numberColor = zeroBlended;
            }

            // Exact special values (tight ranges for snap points)
            if ( currentChoice > -0.05 && currentChoice < 0.05 ) {
                // Only hard-snap to exactly 0.0 when we entered the window from
                // outside (or on first draw). While already inside it, let the
                // value accumulate so slow steps can climb back out of zero.
                bool wasInZeroWindow = ( choiceBeforeDelta > -0.05 && choiceBeforeDelta < 0.05 );
                if ( firstTime || !wasInZeroWindow ) {
                    currentChoice = 0.0;
                }
                numberColor = zeroColor;
            } else if ( currentChoice > 3.25 && currentChoice < 3.35 ) {
                numberColor = threeColor;
            } else if ( currentChoice > 4.95 && currentChoice < 5.05 ) {
                numberColor = fiveColor;
            } else if ( currentChoice > 7.95 && currentChoice < 8.55 ) {
                numberColor = maxColor;
            } else if ( currentChoice < -0.05 ) {
                numberColor = negColor;
            }

            // Format display string. Values that round to zero at 0.1 V
            // resolution are shown as 0.0 (never "-0.0"), even when the
            // underlying value is a tiny negative resting inside the zero window.
            float displayChoice = ( currentChoice > -0.05 && currentChoice < 0.05 ) ? 0.0f : roundedCurrentChoice;
            if ( displayChoice < 0.00 ) {
                snprintf( floatString, 8, "%0.1f V", displayChoice );
            } else {
                snprintf( floatString, 8, " %0.1f V", displayChoice );
            }

            // Update LED display
            b.clear( 1 );
            b.print( floatString, numberColor, 0xffffff, 0, 1, 1 );
            requestLedShow( 2 );

            // Update serial
            Serial.print( "\r                        \r" );
            Serial.print( floatString );

            // Update OLED
            oled.clearPrintShow( floatString, 2, true, true, true );

            // Update global state immediately
            if ( rail == 0 ) {
                globalState.power.topRail = roundedCurrentChoice;
                globalState.power.bottomRail = roundedCurrentChoice;
            } else if ( rail == 1 ) {
                globalState.power.topRail = roundedCurrentChoice;
            } else if ( rail == 2 ) {
                globalState.power.bottomRail = roundedCurrentChoice;
            }

            // Apply voltage to hardware (only for safe range)
            if ( firstTime == 0 && currentChoice >= 0.0 && currentChoice <= 5.0 ) {
                switch ( rail ) {
                case 0:
                    setTopRail( roundedCurrentChoice, 1, 0 ); // save=1 to update globalState
                    setBotRail( roundedCurrentChoice, 1, 0 );
                    break;
                case 1:
                    setTopRail( roundedCurrentChoice, 1, 0 );
                    break;
                case 2:
                    setBotRail( roundedCurrentChoice, 1, 0 );
                    break;
                }
            }

            // Check for snap values (using accelerationMultiplier == 1.0 for precision)
            if ( snapToValue == 0 && snap != 0 && accelerationMultiplier == 1.0 ) {
                for ( int i = 0; i < 8; i++ ) {
                    if ( ( fabs( currentChoice ) > snapValues[ i ] - 0.05 && fabs( currentChoice ) < snapValues[ i ] + 0.05 ) ) {
                        snapToValue = 3;
                    }
                }
            }

            // requestLedShow( 2 );
            delayMicroseconds(8000);
            firstTime = 0;
        }
    }

    return roundedCurrentChoice;
}

// EncoderAccelerator class moved to RotaryEncoder.h for shared use

/**
 * @brief Get integer value from user via rotary encoder and breadboard LEDs
 *
 * Similar to getActionFloat but for integer values. Uses rotary encoder to scroll
 * through range with acceleration. Probe touch can jump through range based on position.
 *
 * @param min Minimum allowed value
 * @param max Maximum allowed value
 * @param currentValue Starting value (optional, defaults to middle of range)
 * @return Selected integer value
 */
int getActionInt( int minVal, int maxVal, int currentValue ) {
    // This screen paints the breadboard without the transition bracket —
    // abort any in-flight menu transition so its blend frames can't stamp
    // the old menu line over what we draw here.
    menuTransitionCancel( );

    // Initialize to middle of range if no current value provided
    if ( currentValue == -1 ) {
        currentValue = ( minVal + maxVal ) / 2;
    }

    // Clamp to range
    if ( currentValue < minVal )
        currentValue = minVal;
    if ( currentValue > maxVal )
        currentValue = maxVal;

    int range = maxVal - minVal;
    int originalValue = currentValue;
    char intString[ 16 ];

    // Set rotary divider for good responsiveness
    int lastDivider = rotaryDivider;
    rotaryDivider = 3;

    b.clear( 1 );

    // Color mapping based on position in range
    uint32_t lowColor = 0x001010;  // Cyan for low values
    uint32_t midColor = 0x101000;  // Yellow for mid values
    uint32_t highColor = 0x100010; // Magenta for high values
    uint32_t currentColor = midColor;

    // Position-based tracking for direct encoder reading
    long lastEncoderPosition = encoderPosition;

    // Use generalized accelerator
    EncoderAccelerator accelerator;

    // Fractional accumulation for ultra-precise control
    float fractionalValue = (float)currentValue;
    int lastDisplayedValue = currentValue;

    bool firstUpdate = true;

    // Reset button state to wait for NEW press
    Menus::getInstance( ).inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    // Display initial value
    float position = (float)( currentValue - minVal ) / (float)range;
    if ( position < 0.33f ) {
        currentColor = lowColor;
    } else if ( position < 0.67f ) {
        currentColor = midColor;
    } else {
        currentColor = highColor;
    }

    snprintf( intString, 16, "%d", currentValue );
    b.clear( 1 );
    b.print( intString, currentColor, 0xffffff, 0, 1, 1 );
    Serial.print( "\r                        \r" );
    Serial.print( intString );
    oled.clearPrintShow( intString, 2, true, true, true );
    requestLedShow( 2 );

    unsigned long lastShowRequestMs = millis( );

    while ( true ) {
        delayMicroseconds( 200 );
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        menuShowKeepalive( lastShowRequestMs );

        // Check for cancellation (long press)
        int probePress = ProbeButton::getInstance( ).getButtonPress( ); // one read per pass (see getMenuSelection)
        if ( encoderButtonState == HELD || probePress == 1 ) {
            rotaryDivider = lastDivider;
            encoderButtonState = IDLE;
            b.clear( );
            Menus::getInstance( ).inClickMenu = 0;
            return originalValue; // Return original on cancel
        }

        // Check for confirmation (short press)
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED || probePress == 2 ) {
            encoderButtonState = IDLE;
            rotaryDivider = lastDivider;
            b.clear( );
            Menus::getInstance( ).inClickMenu = 0;
            return currentValue;
        }

        // Handle serial input for cancellation
        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            rotaryDivider = lastDivider;
            b.clear( );
            Menus::getInstance( ).inClickMenu = 0;
            return originalValue;
        }

        // Read encoder position directly
        long currentEncoderPosition = encoderPosition;
        long encoderDelta = currentEncoderPosition - lastEncoderPosition;

        bool valueChanged = false;

        if ( encoderDelta != 0 || firstUpdate ) {
            if ( !firstUpdate ) {
                // Get accelerated delta from helper class
                float deltaFloat = accelerator.getAcceleratedDelta( encoderDelta );

                // Apply to fractional value (note: negative because encoder direction)
                fractionalValue -= deltaFloat;

                // Clamp fractional value to range
                if ( fractionalValue > (float)maxVal )
                    fractionalValue = (float)maxVal;
                if ( fractionalValue < (float)minVal )
                    fractionalValue = (float)minVal;

                // Convert to integer for display
                currentValue = (int)roundf( fractionalValue );
            }

            lastEncoderPosition = currentEncoderPosition;
            valueChanged = true;
            firstUpdate = false;
        }

        if ( valueChanged ) {
            // Clamp to range (already done to fractionalValue above)
            if ( currentValue > maxVal )
                currentValue = maxVal;
            if ( currentValue < minVal )
                currentValue = minVal;

            // Only update display if the integer value actually changed
            if ( currentValue != lastDisplayedValue ) {
                lastDisplayedValue = currentValue;

                // Calculate color based on position in range
                float position = (float)( currentValue - minVal ) / (float)range;
                if ( position < 0.33f ) {
                    currentColor = lowColor;
                } else if ( position < 0.67f ) {
                    currentColor = midColor;
                } else {
                    currentColor = highColor;
                }

                // Display on breadboard and serial
                snprintf( intString, 16, "%d", currentValue );
                b.clear( 1 );
                b.print( intString, currentColor, 0xffffff, 0, 1, 1 );

                Serial.print( "\r                        \r" );
                Serial.print( intString );

                // Display on OLED
                oled.clearPrintShow( intString, 2, true, true, true );

                requestLedShow( 2 );
            }
        }
    }

    // Should never reach here
    rotaryDivider = lastDivider;
    Menus::getInstance( ).inClickMenu = 0;
    return originalValue;
}

/**
 * @brief Helper function to get display name for special characters
 */
static const char* getCharDisplayName( char c ) {
    if ( c == '\b' || c == 0x08 )
        return "<BS>";
    if ( c == '\t' )
        return "<TAB>";
    if ( c == '\n' )
        return "<ENTER>";
    return nullptr;
}

/**
 * @brief Helper function to display string on breadboard with scrolling
 *
 * @param inputString The string to display
 * @param cursorPos Current cursor position
 * @param currentChar Optional current character being selected (highlighted)
 * @param highlightColor Color for current character
 * @param dimColor Color for entered characters
 */
static void displayStringOnBreadboard( const char* inputString, int cursorPos,
                                       char currentChar = 0,
                                       uint32_t highlightColor = 0x0f0f0f,
                                       uint32_t dimColor = 0x050505 ) {
    b.clear( );

    // Calculate scroll offset - show 16 char window
    int scrollOffset = 0;
    if ( cursorPos >= 8 ) {
        scrollOffset = cursorPos - 7; // Keep cursor near middle-right
        if ( scrollOffset > cursorPos - 15 && cursorPos >= 15 ) {
            scrollOffset = cursorPos - 15; // Don't scroll past the start
        }
    }

    // Show entered characters in scrolling window
    for ( int i = scrollOffset; i < cursorPos && i < scrollOffset + 16; i++ ) {
        int displayPos = i - scrollOffset;
        int displayX = displayPos % 8;
        int displayY = displayPos / 8;

        char charStr[ 2 ] = { inputString[ i ], '\0' };
        b.print( charStr, dimColor, 0x000000, displayX, -1, displayY );
    }

    // Show current character if provided
    if ( currentChar != 0 ) {
        int cursorDisplayPos = cursorPos - scrollOffset;
        int displayX = cursorDisplayPos % 8;
        int displayY = cursorDisplayPos / 8;

        if ( cursorDisplayPos >= 0 && cursorDisplayPos < 16 ) {
            const char* specialName = getCharDisplayName( currentChar );
            if ( specialName ) {
                // Show compact special char name
                char compactName[ 5 ];
                if ( currentChar == '\b' || currentChar == 0x08 ) {
                    strcpy( compactName, "BS" );
                } else if ( currentChar == '\t' ) {
                    strcpy( compactName, "TAB" );
                } else if ( currentChar == '\n' ) {
                    strcpy( compactName, "\n" );
                } else {
                    strcpy( compactName, "?" );
                }
                b.print( compactName, highlightColor, 0xffffff, displayX, -1, displayY );
            } else {
                char displayStr[ 2 ] = { currentChar, '\0' };
                b.print( displayStr, highlightColor, 0xffffff, displayX, -1, displayY );
            }
        }
    }
}

/**
 * @brief Get text string from user via rotary encoder character selection
 *
 * Uses rotary encoder to select characters one by one. Displays current character
 * on breadboard LEDs and full string on OLED.
 *
 * Features:
 * - Rotate encoder to select characters
 * - Short press to confirm character and move to next
 * - Double-click to delete last character (backspace)
 * - Long press to finish and return string
 * - Type directly from Serial (backspace deletes, ESC exits)
 * - Special characters: <BS>, <TAB>, <ENTER> for control chars
 *
 * @param maxLength Maximum string length (default 32)
 * @return Entered string (empty on cancel/ESC)
 */
String getActionString( int maxLength ) {
    // This screen paints the breadboard without the transition bracket —
    // abort any in-flight menu transition so its blend frames can't stamp
    // the old menu line over what we draw here.
    menuTransitionCancel( );

    // Character set for text input (printable ASCII + special control chars)
    // Special chars use placeholder indices that map to actual control codes
    const char* characterSet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:',.<>?/\\\"\x08\t\n ";
    int charSetLength = strlen( characterSet );

    // Use fixed-size buffer (max 128 chars + null terminator to avoid VLA)
    const int MAX_STRING_LENGTH = 129;
    char inputString[ MAX_STRING_LENGTH ];
    memset( inputString, 0, MAX_STRING_LENGTH );

    // Clamp maxLength to safe maximum
    if ( maxLength <= 0 || maxLength >= MAX_STRING_LENGTH ) {
        maxLength = 32;
    }

    int cursorPos = 0;
    float charIndexFloat = 0.0f; // Fractional character index for smooth acceleration
    int charIndex = 0;           // Integer character index for display

    // Save and set rotary divider for good responsiveness
    int lastDivider = rotaryDivider;
    rotaryDivider = 4;

    b.clear( );
    bool firstUpdate = true;

    // Color for current character highlight
    uint32_t highlightColor = 0x0f0f0f;
    uint32_t dimColor = 0x030303;
    uint32_t cursorColor = 0x050505;

    // Position-based tracking
    long lastEncoderPosition = encoderPosition;

    // Use generalized accelerator for character selection
    EncoderAccelerator accelerator;

    accelerator = EncoderAccelerator::Medium( );

    // State management - match pattern from getActionInt
    Menus::getInstance( ).inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    // Enable interactive mode for live character echoing
    // Serial.write(0x0E);  // Turn ON interactive mode
    // Serial.flush();

    // Display initial cursor (fixed size buffer for display)
    char oledDisplay[ 140 ];
    snprintf( oledDisplay, sizeof( oledDisplay ), "    " );
    oled.clearPrintShow( oledDisplay, 2, true, true, true );
    Serial.print( "\r                          \r" );
    Serial.println( "Type directly or use encoder. ESC to cancel, Ctrl+Enter to finish." );
    requestLedShow( 2 );

    unsigned long lastShowRequestMs = millis( );

    while ( true ) {
        // delayMicroseconds(300);
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        menuShowKeepalive( lastShowRequestMs );
        // Handle serial input for direct typing
        if ( Serial.available( ) > 0 ) {
            char c = Serial.read( );

            // ESC = cancel and exit
            if ( c == 0x1B ) { // ESC
                rotaryDivider = lastDivider;
                b.clear( );
                // Serial.write(0x0F);  // Turn OFF interactive mode
                // Serial.flush();
                Serial.println( "\n\rCanceled" );
                Menus::getInstance( ).inClickMenu = 0;
                return String( "" );
            }

            // Backspace = delete last character
            if ( c == '\b' || c == 0x7F ) {
                if ( cursorPos > 0 ) {
                    // Get the character we're deleting to maintain charIndex near it
                    char deletedChar = inputString[ cursorPos - 1 ];

                    cursorPos--;
                    inputString[ cursorPos ] = '\0';

                    // Try to find the deleted char in the character set to position there
                    for ( int i = 0; i < charSetLength; i++ ) {
                        if ( characterSet[ i ] == deletedChar ) {
                            charIndex = i;
                            charIndexFloat = (float)i;
                            break;
                        }
                    }

                    accelerator.reset( );
                    firstUpdate = true;

                    // Update display with scrolling and live echo
                    snprintf( oledDisplay, sizeof( oledDisplay ), "%s_", inputString );
                    oled.clearPrintShow( oledDisplay, 2, true, true, true );

                    // Echo the backspace visually
                    Serial.print( "\r" );
                    Serial.print( inputString );
                    Serial.print( "                        \r" );

                    displayStringOnBreadboard( inputString, cursorPos, 0, highlightColor, cursorColor );
                    requestLedShow( 2 );
                }
                continue;
            }

            // Ctrl+Enter = finish and return
            if ( c == '\n' || c == '\r' ) {
                if ( Serial.peek( ) == '\n' || Serial.peek( ) == '\r' ) {
                    Serial.read( ); // Consume the other newline char
                }
                // Finish and return
                inputString[ cursorPos ] = '\0';
                rotaryDivider = lastDivider;
                b.clear( );
                // Serial.write(0x0F);  // Turn OFF interactive mode
                // Serial.flush();
                // Serial.println();
                Menus::getInstance( ).inClickMenu = 0;
                return String( inputString );
            }

            // Regular character - add to string
            if ( cursorPos < maxLength && ( isprint( c ) || c == '\t' ) ) {
                inputString[ cursorPos ] = c;
                cursorPos++;
                inputString[ cursorPos ] = '\0';

                if ( cursorPos >= maxLength ) {
                    // Max length reached
                    rotaryDivider = lastDivider;
                    b.clear( );
                    // Serial.write(0x0F);  // Turn OFF interactive mode
                    // Serial.flush();
                    // Serial.println();
                    Menus::getInstance( ).inClickMenu = 0;
                    return String( inputString );
                }

                // Keep charIndex at the character we just typed for next position
                // This way if you type 'a' then 'b', the encoder starts at 'b' for next char
                for ( int i = 0; i < charSetLength; i++ ) {
                    if ( characterSet[ i ] == c ) {
                        charIndex = i;
                        charIndexFloat = (float)i;
                        break;
                    }
                }

                accelerator.reset( );
                firstUpdate = true;

                // Update display with scrolling - show live character
                snprintf( oledDisplay, sizeof( oledDisplay ), "%s", inputString );
                oled.clearPrintShow( oledDisplay, 2, true, true, true );

                // Live echo with proper cursor positioning
                Serial.print( "\r" );
                Serial.print( inputString );
                Serial.print( "                        \r" );

                displayStringOnBreadboard( inputString, cursorPos, 0, highlightColor, cursorColor );
                requestLedShow( 2 );
            }
            continue;
        }

        // Check for finish (long press)
        int probePress = ProbeButton::getInstance( ).getButtonPress( ); // one read per pass (see getMenuSelection)
        if ( encoderButtonState == HELD || probePress == 1 ) {
            // Finish and return current string
            inputString[ cursorPos ] = '\0';
            rotaryDivider = lastDivider;
            encoderButtonState = IDLE;
            b.clear( );
            // Serial.write(0x0F);  // Turn OFF interactive mode
            // Serial.flush();
            Serial.println( );
            Menus::getInstance( ).inClickMenu = 0;
            return String( inputString );
        }

        // (A commented-out DOUBLECLICKED backspace branch lived here. Removed
        //  2026-08-22 with the encoder double-click itself — the wheel is
        //  turn/click/hold only. Backspace in this editor is the serial
        //  backspace key; there is no wheel gesture for it.)
        // rotaryEncoderStuff();

        // Check for character confirmation (short press)
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED || probePress == 2 ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;

            // Confirm current character and move to next position
            inputString[ cursorPos ] = characterSet[ charIndex ];
            cursorPos++;

            if ( cursorPos >= maxLength ) {
                // Max length reached, return
                inputString[ maxLength ] = '\0';
                rotaryDivider = lastDivider;
                b.clear( );
                // Serial.write(0x0F);  // Turn OFF interactive mode
                // Serial.flush();
                Serial.println( );
                Menus::getInstance( ).inClickMenu = 0;
                return String( inputString );
            }

            // Keep charIndex at the character we just selected for smart positioning
            // Don't reset to 0 - stay near the last selected character
            // charIndex and charIndexFloat already set, just reset acceleration
            accelerator.reset( ); // Reset acceleration for next character
            firstUpdate = true;
            continue;
        }

        // Read encoder for character selection
        long currentEncoderPosition = encoderPosition;
        long encoderDelta = currentEncoderPosition - lastEncoderPosition;

        if ( encoderDelta != 0 || firstUpdate ) {
            if ( !firstUpdate ) {
                // Get accelerated delta for smooth, fast character browsing
                // REVERSE direction for more intuitive feel
                float deltaFloat = accelerator.getAcceleratedDelta( -encoderDelta );

                // Apply to fractional character index
                charIndexFloat += deltaFloat;

                // Wrap around character set (use modulo for smooth wrapping)
                while ( charIndexFloat < 0.0f )
                    charIndexFloat += charSetLength;
                while ( charIndexFloat >= charSetLength )
                    charIndexFloat -= charSetLength;

                // Convert to integer for display
                charIndex = (int)roundf( charIndexFloat );

                // Ensure within bounds after rounding
                if ( charIndex < 0 )
                    charIndex = 0;
                if ( charIndex >= charSetLength )
                    charIndex = charSetLength - 1;
            }

            // Update display - show entered string + current char with scrolling
            char currentChar = characterSet[ charIndex ];
            displayStringOnBreadboard( inputString, cursorPos, currentChar, highlightColor, cursorColor );

            // Display full string on OLED with current character and cursor
            char tempString[ MAX_STRING_LENGTH ];
            strncpy( tempString, inputString, MAX_STRING_LENGTH - 1 );

            // For display, show special char names
            const char* specialName = getCharDisplayName( currentChar );
            if ( specialName ) {
                // Show the entered string + special char name + cursor
                snprintf( oledDisplay, sizeof( oledDisplay ), "%s%s", inputString, specialName );
            } else {
                tempString[ cursorPos ] = currentChar;
                tempString[ cursorPos + 1 ] = '\0';
                snprintf( oledDisplay, sizeof( oledDisplay ), "%s", tempString );
            }
            oled.clearPrintShow( oledDisplay, 2, true, true, true );

            // Display on serial with special char handling
            Serial.print( "\r" );
            if ( specialName ) {
                Serial.print( inputString );
                Serial.print( specialName );
            } else {
                Serial.print( tempString );
            }
            Serial.print( " " );
            Serial.print( "                        \r" );

            requestLedShow( 2 );
            firstUpdate = false;
            lastEncoderPosition = currentEncoderPosition;
        }
    }

    // Should never reach here
    rotaryDivider = lastDivider;
    // Serial.write(0x0F);  // Turn OFF interactive mode
    // Serial.flush();
    Menus::getInstance( ).inClickMenu = 0;
    return String( "" );
}

/**
 * @brief Get bitmap filename from user via interactive image selector
 *
 * Launches an interactive image browser that displays images from /images folder.
 * User can scroll through images using rotary encoder and select one.
 *
 * Features:
 * - Rotary encoder to browse through images
 * - Live preview of each image on OLED
 * - Image info overlay (filename, counter)
 * - Short click to select image
 * - Long press to cancel
 *
 * @return Selected image filename (empty on cancel)
 */
String getActionBitmap( ) {
    // This screen paints the breadboard without the transition bracket —
    // abort any in-flight menu transition so its blend frames can't stamp
    // the old menu line over what we draw here.
    menuTransitionCancel( );

    // Call the interactive image selector from ImagesApp
    return selectImageFromMenu( );
}

//>n nodes 1 //>b baud 2 //>v voltage 3 //>i integer 7 //>t text 8

// subSelection

actionCategories getActionCategory( void ) {

    if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf( "Slots" ) !=
         -1 ) {
        return SLOTSACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Rails" ) != -1 ) {
        return RAILSACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Show" ) != -1 ) {
        return SHOWACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Output" ) != -1 ) {
        return OUTPUTACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Arduino" ) != -1 ) {
        return ARDUINOACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Probe" ) != -1 ) {
        return PROBEACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Connect" ) != -1 ) {
        return CONNECTACTION;
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Display" ) != -1 ) {
        return DISPLAYACTION;

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Apps" ) != -1 ) {
        return APPSACTION;

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Routing" ) != -1 ) {
        return ROUTINGACTION;

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "OLED" ) != -1 ) {
        return OLEDACTION;

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Calib" ) != -1 ) {
        return CALIBRATIONACTION;

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "History" ) != -1 ) {
        return HISTORYACTION;

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Files" ) != -1 ) {
        return APPSACTION;  // Files = file manager (python_scripts, run .py / load slot / images)
    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Python" ) != -1 ) {
        return APPSACTION;  // Python = same as Files (click-menu file browser)

    } else if ( menuLines[ currentAction.previousMenuPositions[ 0 ] ].indexOf(
                    "Parts" ) != -1 ) {
        // Parts = the picker app row, a childless TOP-LEVEL entry
        // (menuTree.h). Selecting it walks off the end of its empty submenu
        // range and lands in doMenuAction, exactly the way Files and History
        // do. The APPSACTION arm's appNameIdx then falls back to
        // previousMenuPositions[0] for single-level entries, so this yields
        // runApp(-1, "Parts"), which name-matches the apps[] row
        // { "Parts", 25, 1, partsAppLauncher }. This string and that row
        // are ONE unit - rename either and the top-level row goes dead
        // silently, because the menu line's own text IS runApp's name arg.
        //
        // Order-safe: none of the keywords tested above this one - Slots,
        // Rails, Show, Output, Arduino, Probe, Connect, Display, Apps,
        // Routing, OLED, Calib, History, Files, Python - is a substring of
        // "Parts", so no earlier arm can claim the row. (The row was
        // "Projects" through wave 2 and "Guides" through the first ambient
        // slice.)
        return APPSACTION;

    } else {
        return NOCATEGORY;
    }

    return NOCATEGORY;
}

int doMenuAction( int menuPosition, int selection ) {
    // This screen paints the breadboard without the transition bracket —
    // abort any in-flight menu transition so its blend frames can't stamp
    // the old menu line over what we draw here.
    menuTransitionCancel( );


    populateAction( );

    // Strip \31 characters from menuLines referenced in action struct
    // This ensures proper matching while keeping OLED display correct
    for ( int i = 0; i < currentAction.previousMenuIndex; i++ ) {
        int menuIdx = currentAction.previousMenuPositions[ i ];
        if ( menuIdx != -1 ) {
            String cleaned = menuLines[ menuIdx ];
            // cleaned.replace( "\31", "" );
            menuLines[ menuIdx ] = cleaned;
        }
    }

    // printActionStruct( );
    // clearLEDsExceptRails( );
    requestLedShow( -1 );

    actionCategories currentCategory = getActionCategory( );

    if ( currentCategory == SHOWACTION ) { //! Show

        // Serial.print( "Show Action\n\r" );
        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Voltage" ) !=
             -1 ) {

            // printActionStruct();

            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( ADC0, currentAction.to[ i ] );
                        break;
                    case 1:

                        addBridgeToState( ADC1, currentAction.to[ i ] );
                        break;
                        // break;
                    case 2:

                        addBridgeToState( ADC2, currentAction.to[ i ] );
                        break;
                    case 3:
                        addBridgeToState( ADC3, currentAction.to[ i ] );
                        break;
                    case 4:
                        addBridgeToState( ADC4, currentAction.to[ i ] );
                        break;

                    default:
                        break;
                    }

                    // break;
                }
            }

            refreshConnections( );

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "Current" ) != -1 ) {

            // printActionStruct();

            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( ISENSE_PLUS, currentAction.to[ i ] );
                        break;
                    case 1:

                        addBridgeToState( ISENSE_MINUS, currentAction.to[ i ] );
                        break;
                        // break;

                    default:
                        break;
                    }

                    // break;
                }
            }
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Digital" ) != -1 ) {
            if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "GPIO" ) != -1 ) {
                for ( int i = 0; i < 10; i++ ) {
                    if ( currentAction.from[ i ] != -1 ) {
                        switch ( currentAction.from[ i ] ) {
                        case 0:
                            addBridgeToState( RP_GPIO_1, currentAction.to[ i ] );
                            gpioState[ 0 ] = 2;
                            break;
                        case 1:
                            addBridgeToState( RP_GPIO_2, currentAction.to[ i ] );
                            gpioState[ 1 ] = 2;
                            break;
                        case 2:
                            addBridgeToState( RP_GPIO_3, currentAction.to[ i ] );
                            gpioState[ 2 ] = 2;
                            break;
                        case 3:
                            addBridgeToState( RP_GPIO_4, currentAction.to[ i ] );
                            gpioState[ 3 ] = 2;
                            break;
                        case 4:
                            addBridgeToState( RP_GPIO_5, currentAction.to[ i ] );
                            gpioState[ 4 ] = 2;
                            break;
                        case 5:
                            addBridgeToState( RP_GPIO_6, currentAction.to[ i ] );
                            gpioState[ 5 ] = 2;
                            break;
                        case 6:
                            addBridgeToState( RP_GPIO_7, currentAction.to[ i ] );
                            gpioState[ 6 ] = 2;
                            break;
                        case 7:
                            addBridgeToState( RP_GPIO_8, currentAction.to[ i ] );
                            gpioState[ 7 ] = 2;
                            break;
                        default:
                            break;
                        }
                        updateGPIOConfigFromState( );
                    }
                }
            }
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "UART" ) != -1 ) {
            // Dispatch on from[i] (the picked option index), NOT on
            // indexOf("Tx") against the option line - that line is "Tx Rx"
            // so the string test always matched Tx and Rx was unreachable.
            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 && currentAction.to[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( RP_UART_TX, currentAction.to[ i ] );
                        break;
                    case 1:
                        addBridgeToState( RP_UART_RX, currentAction.to[ i ] );
                        break;
                    default:
                        break;
                    }
                }
            }
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "I2C" ) != -1 ) {
            // Same from[i] dispatch fix as UART ("SDA SCL" always matched
            // SDA). I2C1 is routed on GPIO 26/27 (breadboard GPIO 7/8, the
            // same pair the OLED's crossbar connection uses).
            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 && currentAction.to[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( RP_GPIO_26, currentAction.to[ i ] ); // SDA
                        break;
                    case 1:
                        addBridgeToState( RP_GPIO_27, currentAction.to[ i ] ); // SCL
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        // digitalWrite(RESETPIN, HIGH);

        // delayMicroseconds(200);

        // digitalWrite(RESETPIN, LOW);

        // showSavedColors(netSlot);
        // sendPaths();
        // sendAllPathsCore2 = 1;
        // chooseShownReadings();

        refreshConnections( );

        slotChanged = 0;

        return 10;
        // loadingFile = 1;

    } else if ( currentCategory == RAILSACTION ) { //! Rails

        //  Serial.print( "Rails Action\n\r" );
        // A cancelled slider leaves analogVoltage NAN: the drag already moved
        // the rails and railCommitEdit() recorded it, so there is nothing to
        // apply. Re-applying drove BOTH rails to 0.00V and persisted that over
        // the user's saved voltage (sweep finding, high).
        if ( isnan( currentAction.analogVoltage ) ) {
            requestLedShow( -1 );
            currentAction.analogVoltage = 0.0f;   // leave the struct sane
        } else {
        requestLedShow( 1 );
        waitCore2( );

        switch ( currentAction.from[ 0 ] ) {
        case 0: {
            setTopRail( currentAction.analogVoltage, 1, 0 );
            delayMicroseconds( 100 );
            setBotRail( currentAction.analogVoltage, 1, 0 );
            // oled.clearPrintShow("Both Rails \n\rset to ", 1, true, false, false);
            String voltageString = "Both Rails set to\n\r" + String( currentAction.analogVoltage ) + " V";
            oled.clearPrintShow( voltageString, 2, true, true, true );
            break;
        }
        case 1: {
            // delay(100);
            setTopRail( currentAction.analogVoltage, 1, 0 );
            String voltageString = "Top Rail set to\n\r" + String( currentAction.analogVoltage ) + " V";
            oled.clearPrintShow( voltageString, 2, true, true, true );
            break;
        }
        case 2: {
            // delay(100);
            setBotRail( currentAction.analogVoltage, 1, 0 );
            String voltageString = "Bottom Rail set to\n\r" + String( currentAction.analogVoltage ) + " V";
            oled.clearPrintShow( voltageString, 2, true, true, true );
            break;
        }
        default: {
            break;
        }
        }
        requestLedShow( -1 );
        }   // end of the "slider was confirmed" apply block

        // NO markDirty() HERE, DELIBERATELY (w3-5 fix round 2).
        //
        // The old comment here said "State is marked dirty by setRailVoltage()",
        // which stopped being true when the setter gained its no-op gate. Fix
        // round 1 replaced it with an unconditional mark, and that was wrong in
        // the other direction: this branch is reached from
        // `getActionFloat(); return doMenuAction();` (:1490-1495), which runs
        // doMenuAction() after EVERY exit from the slider - confirm, long-press
        // cancel and serial cancel alike. Short-pressing without turning
        // anything would therefore have dirtied the state and made the next
        // idle flush rewrite the slot file: one more no-change rewrite, which
        // is the exact class of bug this task exists to remove.
        //
        // Both cases are already covered without a mark here:
        //   * the slider PRE-WROTE globalState.power, so the setters above see
        //     prev == voltage and stay quiet - and railCommitEdit() (:2941) has
        //     already marked, but only when the rails actually moved. It is the
        //     single owner of that decision and it runs on all three exits.
        //   * a future Rails menu entry that reaches RAILSACTION WITHOUT the
        //     slider (a preset voltage, actions[] != 3) does no pre-write, so
        //     setTopRail/setBotRail arrive with prev != voltage and the setter's
        //     own mark fires normally.
        // The pre-write is the only thing that can hide a change from the
        // setter, and the only pre-writer is the slider. So: nothing to do here.
        // (No configChanged either - voltages live in state, not config.)

    } else if ( currentCategory == SLOTSACTION ) { //! Slots

        // Serial.print( "Slots Action\n\r" );

        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Save" ) !=
             -1 ) {

            if ( currentAction.from[ 0 ] >= 0 && currentAction.from[ 0 ] < NUM_SLOTS ) {
                // Save the ACTIVE STATE to the chosen destination. This used
                // to be saveCurrentSlotToSlot(netSlot, dest), which LOADED
                // netSlot first and then saved it - a reload that was always
                // redundant (the active state IS the source) and that fails
                // outright with netSlot == -2. saveSlot(dest) also moves the
                // context to dest, number and path together.
                String errorMsg;
                if ( !SlotManager::getInstance( ).saveSlot( currentAction.from[ 0 ], errorMsg ) ) {
                    Serial.println( "Error saving to slot " +
                                    String( currentAction.from[ 0 ] ) + ": " + errorMsg );
                }
                netSlot = currentAction.from[ 0 ];
            }

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "Load" ) != -1 ) {
            if ( currentAction.from[ 0 ] >= 0 && currentAction.from[ 0 ] < NUM_SLOTS ) {
                // saveCurrentSlotToSlot(netSlot, currentAction.from[0]);

                netSlot = currentAction.from[ 0 ];
                String errorMsg;
                SlotManager::getInstance( ).exitPreview( true, errorMsg );

                SlotManager::getInstance( ).setActiveSlot( netSlot );
                // slotChanged = 1;
                refreshConnections( -1 );
                //  chooseShownReadings();
                // printAllChangedNetColorFiles( );
            }
            // netSlot = currentAction.from[0];
            return currentAction.from[ 0 ];
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "Clear" ) != -1 ) {
            // createSlots(currentAction.from[0], 0);

            String errorMsg;
            SlotManager::getInstance( ).deleteSlot( currentAction.from[ 0 ], errorMsg );

            //  refreshConnections();

            //  sendAllPathsCore2 = 1;
            //  chooseShownReadings();
            return 10;
        }

    } else if ( currentCategory == OUTPUTACTION ) { //! Output

        // Serial.print( "Output Action\n\r" );
        printActionStruct( );
        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "GPIO" ) !=
             -1 ) {

            printActionStruct( );

            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( RP_GPIO_1, currentAction.to[ i ] );
                        break;
                    case 1:

                        addBridgeToState( RP_GPIO_2, currentAction.to[ i ] );
                        break;

                    case 2:

                        addBridgeToState( RP_GPIO_3, currentAction.to[ i ] );
                        break;
                    case 3:

                        addBridgeToState( RP_GPIO_4, currentAction.to[ i ] );
                        break;

                    case 4:

                        addBridgeToState( RP_GPIO_5, currentAction.to[ i ] );
                        break;

                    case 5:

                        addBridgeToState( RP_GPIO_6, currentAction.to[ i ] );
                        break;

                    case 6:

                        addBridgeToState( RP_GPIO_7, currentAction.to[ i ] );
                        break;

                    case 7:

                        addBridgeToState( RP_GPIO_8, currentAction.to[ i ] );
                        break;

                    default:
                        break;
                    }

                    // break;
                }

                // break;
            }
            // Without this the bridges only land in state and nothing
            // routes them until some other action refreshes.
            refreshConnections( );

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "Voltage" ) != -1 ) {

            printActionStruct( );

            // Did the loop below actually PRE-WRITE a power field? Only then is
            // there an edit the setters cannot see (w3-5 fix round 2). This
            // branch is reached from `selectNodeAction(); ... doMenuAction();`
            // (:1444/:1477) with the return value unchecked, and
            // selectNodeAction() returns -1 on cancel (:2673) - so a cancelled
            // node pick lands here with every from[]/to[] still -1, the loop
            // body never runs, and an unconditional mark (which is what fix
            // round 1 left here) would dirty the state for a visit that changed
            // nothing at all.
            bool wroteAPowerField = false;

            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 && currentAction.to[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( DAC0, currentAction.to[ i ] );
                        // setDac0_5Vvoltage(currentAction.analogVoltage);
                        globalState.power.dac0 = currentAction.analogVoltage;
                        wroteAPowerField = true;
                        break;
                    case 1:

                        addBridgeToState( DAC1, currentAction.to[ i ] );
                        globalState.power.dac1 = currentAction.analogVoltage;
                        wroteAPowerField = true;
                        // setDac1_8Vvoltage(currentAction.analogVoltage);
                        break;
                        // break;

                    case 2:
                        addBridgeToState( TOP_RAIL, currentAction.to[ i ] );
                        globalState.power.topRail = currentAction.analogVoltage;
                        wroteAPowerField = true;
                        break;
                    case 3:
                        addBridgeToState( BOTTOM_RAIL, currentAction.to[ i ] );
                        globalState.power.bottomRail = currentAction.analogVoltage;
                        wroteAPowerField = true;
                        break;

                    default:
                        break;
                    }
                }
            }
            refreshConnections( );
            setRailsAndDACs( );
            // THIRD instance of the pre-write shape (w3-5 sweep, not in the
            // review's list): the cases above assign globalState.power.dac0 /
            // dac1 / topRail / bottomRail DIRECTLY, so setRailsAndDACs() - which
            // re-reads those same fields - hands every setter prev == voltage
            // and marks nothing. Before this, the dirty flag arrived incidentally
            // from the addBridgeToState() next to each assignment, which is both
            // fragile (it is persisting a VOLTAGE on the strength of a BRIDGE
            // add) and lossy when that add is refused (isConnectionAllowed, or
            // MAX_BRIDGES). Mark it for the reason it is actually true - the
            // user set a voltage - and ONLY when one was actually set.
            if ( wroteAPowerField ) {
                globalState.markDirty();
            }

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "UART" ) != -1 ) {
            for ( int i = 0; i < 10; i++ ) {
                if ( currentAction.from[ i ] != -1 && currentAction.to[ i ] != -1 ) {
                    switch ( currentAction.from[ i ] ) {
                    case 0:
                        addBridgeToState( RP_UART_TX, currentAction.to[ i ] );
                        break;
                    case 1:

                        addBridgeToState( RP_UART_RX, currentAction.to[ i ] );
                        break;
                        // break;
                    default:
                        break;
                    }

                    // break;
                }
            }
            // Same as GPIO above - route the new bridges now.
            refreshConnections( );
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Limits" ) != -1 ) {
            if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Min Max" ) != -1 ) {
                // Serial.print( "Min Max\n\r" );
                if ( currentAction.from[ 0 ] == 0 ) {
                    jumperlessConfig.dacs.limit_min = 0.0;
                } else if ( currentAction.from[ 0 ] == 1 ) {
                    jumperlessConfig.dacs.limit_min = 0.0;
                } else if ( currentAction.from[ 0 ] == 2 ) {
                    jumperlessConfig.dacs.limit_min = -5.0;
                } else if ( currentAction.from[ 0 ] == 3 ) {
                    jumperlessConfig.dacs.limit_min = -8.0;
                }

                if ( currentAction.from[ 0 ] == 0 ) {
                    jumperlessConfig.dacs.limit_max = 3.3;
                } else if ( currentAction.from[ 0 ] == 1 ) {
                    jumperlessConfig.dacs.limit_max = 5.0;
                } else if ( currentAction.from[ 0 ] == 2 ) {
                    jumperlessConfig.dacs.limit_max = 5.0;
                } else if ( currentAction.from[ 0 ] == 3 ) {
                    jumperlessConfig.dacs.limit_max = 8.0;
                }

                configChanged = true;
            }
        }

    } else if ( currentCategory == HISTORYACTION ) { //! History

        exitMenuModeForAction( );
        runHistoryScrubMenu( );
        refreshConnections( -1, 0 );
        return 10;

    } else if ( currentCategory == APPSACTION ) {

        // Serial.print( "Apps Action\n\r" ); //! Apps Action

        // Use last valid menu position for app name (Files/Python are single-level; Apps subitems use [1])
        int appNameIdx = ( currentAction.previousMenuIndex > 1 && currentAction.previousMenuPositions[ 1 ] >= 0 )
                             ? currentAction.previousMenuPositions[ 1 ]
                             : currentAction.previousMenuPositions[ 0 ];

        if ( appNameIdx >= 0 && menuLines[ appNameIdx ].indexOf( "Games" ) != -1 ) {
            doomOn = 1;
            // Serial.println( "\n\n\n\rGames\n\r" );
        }
        // digitalWrite(RESETPIN, HIGH);

        // delayMicroseconds(200);

        // digitalWrite(RESETPIN, LOW);

        // showSavedColors(netSlot);
        // sendPaths();
        // sendAllPathsCore2 = 1;
        // chooseShownReadings();

        // slotChanged = 0;
        exitMenuModeForAction( );

        runApp( -1, (char*)menuLines[ appNameIdx ].c_str( ) );
        // requestLedShow( -1 );
        refreshConnections( -1, 0 );

        return 10;

    } else if ( currentCategory == CALIBRATIONACTION ) {

        // Serial.print( "Calibration Action\n\r" ); //! Calibration Action

        exitMenuModeForAction( );

        // Translate menu names to app names
        String menuItem = menuLines[ currentAction.previousMenuPositions[ 1 ] ];
        String appName;

        if ( menuItem.indexOf( "Pads" ) != -1 ) {
            appName = "Probe  Calib";
        } else if ( menuItem.indexOf( "Thresh" ) != -1 ) {
            appName = "Switch Calib";
        } else if ( menuItem.indexOf( "DACs" ) != -1 ) {
            appName = "Calib  DACs";
        } else if ( menuItem.indexOf( "Full" ) != -1 ) {
            appName = "Full   Test";
        } else if ( menuItem.indexOf( "Cable" ) != -1 ) {
            appName = "Probe  Cable";
        } else if ( menuItem.indexOf( "Xbar" ) != -1 ) {
            appName = "Xbar   Test";
        } else if ( menuItem.indexOf( "Tip" ) != -1 ) {
            appName = "Tip    Voltage";
        } else if ( menuItem.indexOf( "PSRAM" ) != -1 ) {
            appName = "PSRAM  Test";
        } else {
            // Fallback: try the menu item directly
            appName = menuItem;
        }

        runApp( -1, (char*)appName.c_str( ) );
        refreshConnections( -1, 0 );

        return 10;

    } else if ( currentCategory == ARDUINOACTION ) {

        //    Serial.print( "Arduino Action\n\r" );

    } else if ( currentCategory == PROBEACTION ) {

        //   Serial.print( "Probe Action\n\r" );

    } else if ( currentCategory == CONNECTACTION ) {
        // Connect/Disconnect using encoder or probe

        // Get the action type from the submenu selection
        // Check for "Remove" first since it's more specific
        // "Add" = connect mode (setOrClear = 1)
        // "Remove" = disconnect mode (setOrClear = 0)
        int setOrClear = 1; // Default to connect mode

        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Remove" ) != -1 && currentAction.from[ 0 ] == 1 ) {
            setOrClear = 0; // Clear mode
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Add" ) != -1 && currentAction.from[ 0 ] == 0 ) {
            setOrClear = 1; // Connect mode
        }

        // Exit menu mode before entering probe mode. Full teardown (not just
        // inClickMenu = 0): probeMode owns the logo for its connect/clear
        // colors, so the menu's logoRing + color overrides must be dropped
        // here or the ring stays painted over the probe's blue/red logo.
        exitMenuModeForAction( );

        // Enter probe mode with encoder support
        // This works exactly like clicking the probe button, but with encoder
        // cursor support. fromClickMenu = true: a wheel click with no cursor
        // showing re-shows the cursor here, instead of the probe-button-entry
        // behavior of bailing out to the click menu.
        probing.probeMode( setOrClear, -1, true );

        // Refresh display after exiting probe mode (negative = clear the
        // probe-mode leftovers out of the buffer before drawing wires)
        b.clear( );
        requestLedShow( -1 );
        oled.showJogo32h( );

    } else if ( currentCategory == ROUTINGACTION ) { //! Routing Options

        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Stack" ) !=
             -1 ) {
            if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Rails" ) !=
                 -1 ) {
                jumperlessConfig.routing.stack_rails = currentAction.from[ 0 ];

                if ( currentAction.fromAscii[ 0 ][ 0 ] == 'M' || currentAction.fromAscii[ 0 ][ 0 ] == 'm' ) {
                    jumperlessConfig.routing.stack_rails = 7;
                }

            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Paths" ) != -1 ) {

                jumperlessConfig.routing.stack_paths = currentAction.from[ 0 ];

                if ( currentAction.fromAscii[ 0 ][ 0 ] == 'M' || currentAction.fromAscii[ 0 ][ 0 ] == 'm' ) {
                    // pathPriority = 2;
                    // pathDuplicates = 5;
                    jumperlessConfig.routing.stack_paths = 5;
                }

            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "DACs" ) != -1 ) {
                jumperlessConfig.routing.stack_dacs = currentAction.from[ 0 ];

                if ( currentAction.fromAscii[ 0 ][ 0 ] == 'M' || currentAction.fromAscii[ 0 ][ 0 ] == 'm' ) {
                    // dacPriority = 2;
                    // dacDuplicates = 4;
                    jumperlessConfig.routing.stack_dacs = 4;
                } else {
                    // dacPriority = 1;
                    jumperlessConfig.routing.stack_dacs = 1;
                }
            }
        }
        Serial.print( "\n\rDuplicate Rails: " );
        Serial.println( jumperlessConfig.routing.stack_rails );

        Serial.print( "Duplicate DACs: " );
        Serial.print( jumperlessConfig.routing.stack_dacs );
        // Serial.print("\n\rDAC Priority: ");
        // Serial.print(jumperlessConfig.routing.dac_priority);
        Serial.print( "Duplicate Paths: " );
        Serial.println( jumperlessConfig.routing.stack_paths );

        // Serial.print("\n\n\r");

        // Serial.print("Routing Action\n\r");
        refreshConnections( -1, 1, 0 );
        configChanged = true;
        // saveDuplicateSettings(0);

    } else if ( currentCategory == DISPLAYACTION ) { //! Display Options

        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Jumpers" ) !=
             -1 ) {
            if ( currentAction.from[ 0 ] == 0 ) {
                jumperlessConfig.display.lines_wires = 1;
            } else {
                jumperlessConfig.display.lines_wires = 0;
            }
            debugFlagSet( 12 );

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "Bright" ) != -1 ) {

            if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Rails" ) != -1 ) {
                // LEDbrightnessRail = currentAction.from[0] * 10 + 30;
            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Wires" ) != -1 ) {
                // LEDbrightness = currentAction.from[0] * 5 + 5;
            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Special" ) != -1 ) {
                // LEDbrightnessSpecial = currentAction.from[0] * 5 + 5;
            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Menu" ) != -1 ) {
                menuBrightnessSetting = menuBrightnessOptionMap[ currentAction.from[ 0 ] ];
            }

            saveLEDbrightness( 0 );
            showNets( );
            requestLedShow( 2 );
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "DEFCON" ) != -1 ) {

            if ( currentAction.from[ 0 ] == 0 ) {
                strcpy( defconString, "Jumper less V5" );
                defconDisplay = 0;
            } else if ( currentAction.from[ 0 ] == 1 ) {
                defconDisplay = -1;
            } else if ( currentAction.from[ 0 ] == 2 ) {
                strcpy( defconString, " Fuck    You   " );
                // defconString[0] = "  Fuck   You";
                defconDisplay = 0;
            }

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf(
                        "Colors" ) != -1 ) {
            if ( currentAction.from[ 0 ] == 0 ) {
                netColorMode = 0;
            } else {
                netColorMode = 1;
            }
            debugFlagSet( 13 );
        }
        // Serial.print( "Display Action\n\r" );

    } else if ( currentCategory == OLEDACTION ) { //! OLED Options

        // Handle menu-level actions that need special processing
        // Note: Integer input (action 7) is now handled in getMenuSelection()
        // following the same pattern as getActionFloat (action 3)

        // Apply integer input value to config based on menu context.
        // previousMenuPositions[2] only names a row on a 3-deep-or-more path
        // (OLED > Display Size > Width). Reached from a depth-1 row
        // (OLED > Connect) it is -1, and clearAction() only runs once per menu
        // session so it can also hold a STALE index from an earlier Width /
        // Height visit — populateAction() rewrites [0..previousMenuIndex-1]
        // and never clears the tail. The depth test has to gate the WHOLE
        // chain: guarding only the first arm just routes menuLines[-1] into
        // the next one.
        if ( currentAction.previousMenuIndex > 2 &&
             currentAction.previousMenuPositions[ 2 ] >= 0 ) {
            if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Width" ) != -1 ) {
                jumperlessConfig.top_oled.width = currentAction.integerValue;
                oled.displayWidth = currentAction.integerValue;
                configChanged = true;
                // Reinitialize display with new dimensions - init() now properly handles this
                oled.init( );
                // saveConfig();
            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Height" ) != -1 ) {
                jumperlessConfig.top_oled.height = currentAction.integerValue;
                oled.displayHeight = currentAction.integerValue;
                configChanged = true;
                // Reinitialize display with new dimensions - init() now properly handles this
                oled.init( );
                //  saveConfig();
            } else if ( menuLines[ currentAction.previousMenuPositions[ 2 ] ].indexOf( "Rotation" ) != -1 ) {
                jumperlessConfig.top_oled.rotation = currentAction.integerValue;
                configChanged = true;
                // Apply rotation immediately without full reinit
                getDisplay( ).setRotation( jumperlessConfig.top_oled.rotation );
                // saveConfig();
            }
        }

        // Apply text input value to config based on menu context (action 8)
        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "StartUp" ) != -1 &&
             menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Messge" ) != -1 ) {
            // Check if user selected "Edit" or "Clear"
            String selectedOption = String( currentAction.fromAscii[ 0 ] );
            selectedOption.toLowerCase( );

            if ( selectedOption.indexOf( "text" ) != -1 ) {
                // User wants to edit - the text input already ran, so save the value
                strncpy( jumperlessConfig.top_oled.startup_message, currentAction.stringValue.c_str( ), 32 );
                jumperlessConfig.top_oled.startup_message[ 32 ] = '\0'; // Ensure null termination
                configChanged = true;
                //    saveConfig();

                // Show confirmation on OLED and serial
                oled.clear( );
                oled.setTextSize( 1 );
                oled.clearPrintShow( "Startup Message\nSaved:", 1, true, true, true );
                oled.setTextSize( 2 );
                delay( 1000 );
                oled.clearPrintShow( jumperlessConfig.top_oled.startup_message, 2, true, true, true );

                Serial.print( "\n\rStartup message saved: " );
                Serial.println( jumperlessConfig.top_oled.startup_message );
                // delay(1500); // Show confirmation briefly
            } else if ( selectedOption.indexOf( "bitmap" ) != -1 ) {
                // User selected a bitmap image
                if ( currentAction.stringValue.length( ) > 0 ) {
                    // Store the bitmap filename in startup_message
                    strncpy( jumperlessConfig.top_oled.startup_message, currentAction.stringValue.c_str( ), 32 );
                    jumperlessConfig.top_oled.startup_message[ 32 ] = '\0'; // Ensure null termination
                    configChanged = true;

                    // Show confirmation on OLED by displaying the selected image
                    oled.clear( );
                    // oled.setTextSize(1);
                    ///
                    // oled.clearPrintShow("Startup Image\nSelected:", 1, true, true, true);
                    // delay(1000);

                    // Display the selected bitmap as preview
                    String fullPath = currentAction.stringValue;
                    if ( loadAndDisplayImage( currentAction.stringValue.c_str( ) ) ) {
                        // delay(1500); // Show preview
                    } else {
                        Serial.println( "Error loading image" );
                        Serial.println( currentAction.stringValue );
                        Serial.println( fullPath );
                        Serial.flush( );
                        oled.clearPrintShow( "Error loading\nimage", 1, true, true, true );
                        // delay(1500);
                    }

                    Serial.print( "\n\rStartup bitmap saved: " );
                    Serial.println( jumperlessConfig.top_oled.startup_message );
                    //     saveConfig();
                } else {
                    // User canceled selection
                    Serial.println( "\n\rBitmap selection canceled" );
                }
            } else if ( selectedOption.indexOf( "clear" ) != -1 ) {
                // User wants to clear - set to empty string
                strcpy( jumperlessConfig.top_oled.startup_message, "" );
                configChanged = true;
                //   saveConfig();
                //
                // Show confirmation with logo
                oled.clear( );
                // oled.setTextSize(1);
                oled.clearPrintShow( "Startup Message\nCleared", 1, true, true, true );
                // delay(1000);
                // oled.dumpFrameBuffer();

                // Show the logo that will appear on startup
                oled.init( );

                Serial.println( "\n\rStartup message cleared - will show logo on boot" );
                // delay(1500);
            }
        }

        // LEDbrightness = (brightnessOptionMap[currentAction.from[0]]);
        // LEDbrightnessSpecial = (specialBrightnessOptionMap[currentAction.from[0]]);
        if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Connect" ) != -1 &&
             menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "On Boot" ) != -1 ) {
            if ( currentAction.from[ 0 ] == 0 ) {
                jumperlessConfig.top_oled.lock_connection = 1;
                globalState.config.oledLockConnection = 1;
                jumperlessConfig.top_oled.connect_on_boot = 1;
                oled.init( );
            } else if ( currentAction.from[ 0 ] == 1 ) {
                jumperlessConfig.top_oled.connect_on_boot = 0;
                jumperlessConfig.top_oled.lock_connection = 0;
                globalState.config.oledLockConnection = 0;
                oled.disconnect( );
            }
            // oled.init();
            oled.clear( );
            oled.setTextSize( 1 );
            oled.clearPrintShow( "Connect \nOn Boot: ", 1, true, false, false, 0, 0 );
            oled.setTextSize( 2 );
            // oled.setCursor(0, 0);
            if ( jumperlessConfig.top_oled.connect_on_boot == 1 ) {
                oled.print( "On" );
            } else {
                oled.print( "Off" );
            }
            oled.show( );
            oled.setTextSize( 1 );
            configChanged = true;

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Lock" ) != -1 &&
                    menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Connect" ) == -1 ) {
            if ( currentAction.from[ 0 ] == 0 ) {
                jumperlessConfig.top_oled.lock_connection = 1;
                globalState.config.oledLockConnection = 1;
            } else if ( currentAction.from[ 0 ] == 1 ) {
                jumperlessConfig.top_oled.lock_connection = 0;
                globalState.config.oledLockConnection = 0;
            }
            oled.clear( );
            oled.setTextSize( 1 );
            oled.print( "Lock: " );
            oled.setTextSize( 2 );
            if ( jumperlessConfig.top_oled.lock_connection == 1 ) {
                oled.print( "On" );
            } else {
                oled.print( "Off" );
            }
            oled.show( );
            oled.setTextSize( 1 );
            configChanged = true;
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Connect" ) != -1 ) {
            if ( oled.checkConnection( ) == 0 ) {
                jumperlessConfig.top_oled.enabled = 1;
                requestLedShow( 1 );
                oled.init( );
                oled.clearPrintShow( "OLED Connected", 2, true, true, true );
                delay( 300 );
                // oled.clearPrintShow("Disconnecting OLED", 2, true, true, true);
                // delay( 300 );
                oled.clear( );
                oled.showJogo32h( );
                // oled.disconnect( );
                // jumperlessConfig.top_oled.enabled = 0;
            }
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Font" ) != -1 ) {
            // Use the font name directly from menu
            String fontMenuName = menuLines[ currentAction.previousMenuPositions[ 2 ] ];

            // Set the font by name (this will search fontList)
            int fontIndex = oled.setFont( fontMenuName, 0 );

            // Use parseFont() to get the correct FontFamily enum value
            // This ensures consistency with config system (single source of truth)
            int configFontValue = parseFont( fontMenuName.c_str( ) );

            // Update config with FontFamily enum value
            jumperlessConfig.top_oled.font = configFontValue;
            configChanged = true;
            // saveConfig();

            Serial.print( "Font set to: " );
            Serial.print( fontMenuName );
            Serial.print( " (config value " );
            Serial.print( configFontValue );
            Serial.println( ")" );

        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Show" ) != -1 &&
                    menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "in Term" ) != -1 ) {
            if ( jumperlessConfig.top_oled.show_in_terminal == 1 ) {
                jumperlessConfig.top_oled.show_in_terminal = 0;
            } else {
                jumperlessConfig.top_oled.show_in_terminal = 1;
            }
            configChanged = true;
        } else if ( menuLines[ currentAction.previousMenuPositions[ 1 ] ].indexOf( "Pins" ) != -1 ) {
            // Handle OLED connection type selection:
            // 0 = GPIO 7/8 (via crossbar using GPIO 26/27) - uses I2C1 (Wire1)
            // 1 = RP6/RP7 (hardwired GPIO 6/7) - uses I2C1 (Wire1)
            // 2 = Internal I2C0 (hardwired GPIO 4/5) - uses I2C0 (Wire)
            int newConnectionType = currentAction.from[ 0 ];

            // Print feedback BEFORE disconnecting (while OLED still works)
            switch ( newConnectionType ) {
            case 0:
                Serial.println( "\n\rOLED pins set to GPIO 7/8 (crossbar, pins 26/27)" );
                break;
            case 1:
                Serial.println( "\n\rOLED pins set to RP6/RP7 (hardwired, pins 6/7)" );
                break;
            case 2:
                Serial.println( "\n\rOLED pins set to Internal I2C0 (hardwired, pins 4/5)" );
                break;
            }

            // Disconnect, update pins, save, and reinit in one shot.
            applyOledConnectionType( newConnectionType, /*reinitDisplay=*/true, /*persist=*/true );
            configChanged = true;
        }

    } else if ( currentCategory == NOCATEGORY ) {

        // Serial.print( "No Category\n\r" );
    }

    return 1;
}

String categoryNames[] = { "Show", "Rails", "Slots", "Output", "Arduino",
                           "Probe", "Apps", "Routing", "NoCategory" };

actionCategories getActionCategory( int menuPosition ) {

    for ( int i = 0; i < categoryIndex; i++ ) {
        if ( menuPosition >= categoryRanges[ i ][ 0 ] &&
             menuPosition < categoryRanges[ i ][ 1 ] ) {
            Serial.print( "Category: " );
            Serial.println( categoryNames[ i ] );
            Serial.println( "\n\r" );
            return (actionCategories)i; // this assumes the enum is in the right order
        }
    }

    return NOCATEGORY;
}

// char printMainMenu(int extraOptions) {

//   Serial.print("\n\n\r\t\tMenu\n\r");
//   // Serial.print("Slot ");
//   // Serial.print(netSlot);
//   Serial.print("\n\r");
//   Serial.print("\tm = show this menu\n\r");
//   Serial.print("\tn = show netlist\n\r");
//   Serial.print("\ts = show node files by slot\n\r");
//   Serial.print("\to = load node files by slot\n\r");
//   Serial.print("\tf = load node file to current slot\n\r");
//   // Serial.print("\tr = rotary encoder mode -");
//   //   rotaryEncoderMode == 1 ? Serial.print(" ON (z/x to cycle)\n\r")
//   //                          : Serial.print(" off\n\r");
//   // Serial.print("\t\b\bz/x = cycle slots - current slot ");
//   // Serial.print(netSlot);
//   Serial.print("\n\r");
//   Serial.print("\te = show extra menu options\n\r");

//   if (extraOptions == 1) {
//     Serial.print("\tb = show bridge array\n\r");
//     Serial.print("\tp = probe connections\n\r");
//     Serial.print("\tw = waveGen\n\r");
//     Serial.print("\tv = toggle show current/voltage\n\r");
//     // Serial.print("\tu = set baud rate for USB-Serial\n\r");
//     Serial.print("\tl = LED brightness / test\n\r");
//     Serial.print("\td = toggle debug flags\n\r");
//     }
//   // Serial.print("\tc = clear nodes with probe\n\r");
//   Serial.print("\n\n\r");
//   return ' ';
//   }

char LEDbrightnessMenu( void ) {

    char input = ' ';
    Serial.print( "\n\r\t\tLED Brightness Menu \t\n\n\r" );
    Serial.print( "\n\r\tl = LED brightness        =   " );
    Serial.print( LEDbrightness );
    Serial.print( "\n\r\tr = rail brightness       =   " );
    Serial.print( LEDbrightnessRail );
    Serial.print( "\n\r\ts = special brightness    =   " );
    Serial.print( LEDbrightnessSpecial );
    Serial.print( "\n\r\tc = click menu brightness =   " );
    Serial.print( menuBrightnessSetting );
    Serial.print( "\n\r\tt = all types\t" );
    Serial.println( );
    Serial.print( "\n\r\td = reset to defaults" );
    Serial.println( );
    Serial.print( "\n\r\tn = color name test" );
    Serial.print( "\n\r\tb = bounce logo" );
    Serial.print( "\n\r\tc = random colors" );
    Serial.print( "\n\r\t? = is this?" );
    Serial.println( );
    Serial.print( "\n\r\tm = return to main menu\n\n\r" );
    // Serial.print(leds.getBrightness());
    if ( LEDbrightness > 50 || LEDbrightnessRail > 50 ||
         LEDbrightnessSpecial > 70 ) {
        // Serial.print("\tBrightness settings above ~50 will cause significant
        // heating, it's not recommended\n\r");
        /// delay(10);
    }

    while ( Serial.available( ) == 0 ) {
        // delayMicroseconds(10);
    }

    input = Serial.read( );

    if ( input == 'm' ) {
        saveLEDbrightness( 0 );

        return ' ';
    } else if ( input == 'n' ) {
        int hue = colorPicker( );
        Serial.print( "Hue: " );
        Serial.println( hue );
        Serial.print( "Color: " );
        hsvColor hsv = { (uint8_t)hue, 255, LEDbrightness };
        Serial.printf( "0x%06X\n\r", HsvToRaw( hsv ) );
        return ' ';
    } else if ( input == 'd' ) {
        saveLEDbrightness( 1 );

        return ' ';
    } else if ( input == 'l' ) {
        Serial.print( "\n\r\t+ = increase\n\r\t- = decrease\n\r\tm = exit\n\n\r" );
        Serial.flush( );
        while ( input == 'l' ) {

            while ( Serial.available( ) == 0 )
                ;
            char input2 = Serial.read( );
            if ( input2 == '+' ) {
                LEDbrightness += 1;

                if ( LEDbrightness > 200 ) {

                    LEDbrightness = 200;
                }
                Serial.print( "\r                            \r" );
                Serial.print( "LED brightness:  " );
                Serial.print( LEDbrightness );
                Serial.print( "   " );
                // Serial.print("\n\r");
                Serial.flush( );

                requestLedShow( 2 );
            } else if ( input2 == '-' ) {
                LEDbrightness -= 1;

                if ( LEDbrightness < 2 ) {
                    LEDbrightness = 1;
                }
                Serial.print( "\r                            \r" );
                Serial.print( "LED brightness:  " );
                Serial.print( LEDbrightness );
                Serial.print( "   " );
                // Serial.print("\n\r");
                Serial.flush( );

                requestLedShow( 2 );
            } else if ( input2 == 'x' || input2 == ' ' || input2 == 'm' ) {
                input = ' ';
            } else {
            }
            // showNets();

            // for (int i = 8; i <= numberOfNets; i++) {
            //   lightUpNet(i, -1, 1, LEDbrightness, 0);
            // }
            requestLedShow( 1 );

            if ( Serial.available( ) == 0 ) {
                Serial.print( "\r                            \r" );
                Serial.print( "LED brightness:  " );
                Serial.print( LEDbrightness );
                Serial.print( "   " );
                // Serial.print("\n\r");
                Serial.flush( );
                if ( LEDbrightness > 50 ) {
                    // Serial.print("Brightness settings above ~50 will cause
                    // significant heating, it's not recommended\n\r");
                }
            }
        }
    } else if ( input == 'r' ) {
        Serial.print( "\n\r\t+ = increase\n\r\t- = decrease\n\r\tm = exit\n\n\r" );
        while ( input == 'r' ) {

            while ( Serial.available( ) == 0 )
                ;
            char input2 = Serial.read( );
            if ( input2 == '+' || input2 == '=' ) {

                LEDbrightnessRail += 1;

                if ( LEDbrightnessRail > 200 ) {

                    LEDbrightnessRail = 200;
                }
                Serial.print( "\r                            \r" );
                Serial.print( "Rail brightness:  " );
                Serial.print( LEDbrightnessRail );
                Serial.print( "   " );
                // Serial.print("\n\r");
                Serial.flush( );

                requestLedShow( 2 );
            } else if ( input2 == '-' || input2 == '_' ) {

                LEDbrightnessRail -= 1;

                if ( LEDbrightnessRail < 2 ) {
                    LEDbrightnessRail = 1;
                }
                Serial.print( "\r                            \r" );
                Serial.print( "Rail brightness:  " );
                Serial.print( LEDbrightnessRail );
                Serial.print( "   " );
                // Serial.print("\n\r");
                Serial.flush( );

                requestLedShow( 2 );
            } else if ( input2 == 'x' || input2 == ' ' || input2 == 'm' ) {
                input = ' ';
                saveLEDbrightness( 0 );
                return ' ';
            } else {
            }
            lightUpRail( -1, -1, 1, LEDbrightnessRail );

            if ( Serial.available( ) == 0 ) {
                Serial.print( "\r                            \r" );
                Serial.print( "Rail brightness:  " );
                Serial.print( LEDbrightnessRail );
                // Serial.print("\n\r");
                Serial.flush( );
                if ( LEDbrightnessRail > 50 ) {
                    // Serial.println("Brightness settings above ~50 will cause
                    // significant heating, it's not recommended\n\n\r");
                }
            }
        }

        // Serial.print(input);
        // Serial.print("\n\r");
    } else if ( input == 'h' ) {
        Serial.print( "\n\r\t+ = increase\n\r\t- = decrease\n\r\tm = exit\n\n\r" );
        b.clear( );
        b.print( "B", menuColors[ 0 ], 0xffffff, 0, 0, 1 );
        b.print( "r", menuColors[ 1 ], 0xffffff, 1, 0, 1 );
        b.print( "i", menuColors[ 2 ], 0xffffff, 2, 0, 1 );
        b.print( "g", menuColors[ 3 ], 0xffffff, 3, 0, 1 );
        b.print( "h", menuColors[ 4 ], 0xffffff, 4, 0, 1 );
        b.print( "t", menuColors[ 5 ], 0xffffff, 5, 0, 1 );

        b.print( "n", menuColors[ 6 ], 0xffffff, 1, 1, 2 );
        b.print( "e", menuColors[ 4 ], 0xffffff, 2, 1, 2 );
        b.print( "s", menuColors[ 2 ], 0xffffff, 3, 1, 2 );
        b.print( "s", menuColors[ 0 ], 0xffffff, 4, 1, 2 );

        requestLedShow( 2 );
        while ( input == 'h' ) {

            while ( Serial.available( ) == 0 )
                ;
            char input2 = Serial.read( );
            if ( input2 == '+' ) {
                menuBrightnessSetting += 5;
                if ( menuBrightnessSetting > 150 ) {
                    menuBrightnessSetting = 150;
                }

                b.clear( );

                b.print( "B", menuColors[ 0 ], 0xffffff, 0, 0, 1 );
                b.print( "r", menuColors[ 1 ], 0xffffff, 1, 0, 1 );
                b.print( "i", menuColors[ 2 ], 0xffffff, 2, 0, 1 );
                b.print( "g", menuColors[ 3 ], 0xffffff, 3, 0, 1 );
                b.print( "h", menuColors[ 4 ], 0xffffff, 4, 0, 1 );
                b.print( "t", menuColors[ 5 ], 0xffffff, 5, 0, 1 );

                b.print( "n", menuColors[ 6 ], 0xffffff, 1, 1, 2 );
                b.print( "e", menuColors[ 4 ], 0xffffff, 2, 1, 2 );
                b.print( "s", menuColors[ 2 ], 0xffffff, 3, 1, 2 );
                b.print( "s", menuColors[ 0 ], 0xffffff, 4, 1, 2 );

                requestLedShow( 2 );
            } else if ( input2 == '-' ) {

                menuBrightnessSetting -= 5;
                if ( menuBrightnessSetting < -100 ) {
                    menuBrightnessSetting = -100;
                }
                b.clear( );
                b.print( "B", menuColors[ 0 ], 0xffffff, 0, 0, 1 );
                b.print( "r", menuColors[ 1 ], 0xffffff, 1, 0, 1 );
                b.print( "i", menuColors[ 2 ], 0xffffff, 2, 0, 1 );
                b.print( "g", menuColors[ 3 ], 0xffffff, 3, 0, 1 );
                b.print( "h", menuColors[ 4 ], 0xffffff, 4, 0, 1 );
                b.print( "t", menuColors[ 5 ], 0xffffff, 5, 0, 1 );

                b.print( "n", menuColors[ 6 ], 0xffffff, 1, 1, 2 );
                b.print( "e", menuColors[ 4 ], 0xffffff, 2, 1, 2 );
                b.print( "s", menuColors[ 2 ], 0xffffff, 3, 1, 2 );
                b.print( "s", menuColors[ 0 ], 0xffffff, 4, 1, 2 );

                requestLedShow( 2 );
            } else if ( input2 == 'x' ) {
                input = ' ';
            } else {
            }
            lightUpRail( -1, -1, 1, LEDbrightnessRail );

            if ( Serial.available( ) == 0 ) {

                Serial.print( "Click menu brightness:  " );
                Serial.print( menuBrightnessSetting );
                Serial.print( "\n\r" );
            }
        }

        // Serial.print(input);
        Serial.print( "\n\r" );
    } else if ( input == 's' ) {
        // Serial.print("\n\r\t+ = increase\n\r\t- = decrease\n\r\tx =
        // exit\n\n\r");
        while ( input == 's' ) {

            while ( Serial.available( ) == 0 )
                ;
            char input2 = Serial.read( );
            if ( input2 == '+' ) {

                LEDbrightnessSpecial += 1;

                if ( LEDbrightnessSpecial > 200 ) {

                    LEDbrightnessSpecial = 200;
                }

                // requestLedShow( 2 );
            } else if ( input2 == '-' ) {

                LEDbrightnessSpecial -= 1;

                if ( LEDbrightnessSpecial < 2 ) {
                    LEDbrightnessSpecial = 1;
                }

                // requestLedShow( 2 );
            } else if ( input2 == 'x' || input2 == ' ' || input2 == 'm' ) {
                input = ' ';
                saveLEDbrightness( 0 );
                return ' ';
            } else {
            }

            for ( int i = 0; i < 8; i++ ) {
                lightUpNet( i, -1, 1, LEDbrightnessSpecial, 0 );
            }
            requestLedShow( 1 );
            if ( Serial.available( ) == 0 ) {

                Serial.print( "Special brightness:  " );
                Serial.print( LEDbrightnessSpecial );
                Serial.print( "\n\r" );
                if ( LEDbrightnessSpecial > 70 ) {
                    // Serial.print("Brightness settings above ~70 for special nets will
                    // cause significant heating, it's not recommended\n\n\r ");
                }
            }
        }

        // Serial.print(input);
        Serial.print( "\n\r" );
    } else if ( input == 't' ) {

        Serial.print( "\n\r\t+ = increase\n\r\t- = decrease\n\r\tm = exit\n\n\r" );
        while ( input == 't' ) {

            while ( Serial.available( ) == 0 )
                ;
            char input2 = Serial.read( );
            if ( input2 == '+' ) {

                LEDbrightness += 1;
                LEDbrightnessRail += 1;
                LEDbrightnessSpecial += 1;

                if ( LEDbrightness > 200 ) {

                    LEDbrightness = 200;
                }
                if ( LEDbrightnessRail > 200 ) {

                    LEDbrightnessRail = 200;
                }
                if ( LEDbrightnessSpecial > 200 ) {

                    LEDbrightnessSpecial = 200;
                }

                requestLedShow( 1 );
            } else if ( input2 == '-' ) {

                LEDbrightness -= 1;
                LEDbrightnessRail -= 1;
                LEDbrightnessSpecial -= 1;

                if ( LEDbrightness < 2 ) {
                    LEDbrightness = 1;
                }
                if ( LEDbrightnessRail < 2 ) {
                    LEDbrightnessRail = 1;
                }
                if ( LEDbrightnessSpecial < 2 ) {
                    LEDbrightnessSpecial = 1;
                }

                requestLedShow( 1 );
            } else if ( input2 == 'x' || input2 == ' ' || input2 == 'm' ||
                        input2 == 'l' ) {
                input = ' ';
                saveLEDbrightness( 0 );
                return ' ';
            } else {
            }

            for ( int i = 6; i <= numberOfNets; i++ ) {
                lightUpNet( i, -1, 1, LEDbrightness, 0 );
            }

            lightUpRail( -1, -1, 1, LEDbrightnessRail );
            for ( int i = 0; i < 6; i++ ) {
                lightUpNet( i, -1, 1, LEDbrightnessSpecial, 0 );
            }
            requestLedShow( 1 );

            if ( Serial.available( ) == 0 ) {

                Serial.print( "LED brightness:      " );
                Serial.print( LEDbrightness );
                Serial.print( "\n\r" );
                Serial.print( "Rail brightness:     " );
                Serial.print( LEDbrightnessRail );
                Serial.print( "\n\r" );
                Serial.print( "Special brightness:  " );
                Serial.print( LEDbrightnessSpecial );
                Serial.print( "    " );
                Serial.flush( );
                if ( LEDbrightness > 50 || LEDbrightnessRail > 50 ||
                     LEDbrightnessSpecial > 70 ) {
                    // Serial.print("Brightness settings above ~50 will cause
                    // significant heating, it's not recommended\n\n\r ");
                }
            }
        }
    } else if ( input == 'b' ) {
        Serial.print( "\n\rPress any key to exit\n\n\r" );
        leds.clear( );
        holdCore1Frames( ); // park core 1 while we own the LED strip
        while ( Serial.available( ) == 0 ) {
            // startupColorsV5();
            drawAnimatedImage( 0 );
            delay( 80 );
            drawAnimatedImage( 1 );
            //  delay(100);

            // clearLEDsExceptRails();
            // requestLedShow( 1 );

            // delay(2000);
            // rainbowBounce(3);
        }
        releaseCore1Frames( );
        // showNets();
        // lightUpRail(-1, -1, 1);
        requestLedShow( -1 );

        input = '!'; // this tells the main fuction to reset the leds
    } else if ( input == 'c' ) {
        Serial.print( "\n\rPress any key to exit\n\n\r" );
        holdCore1Frames( ); // park core 1 while we own the LED strip
        while ( Serial.available( ) == 0 ) {

            randomColors( );
            leds.show( );
            delayMicroseconds( random( 500, 80000 ) );
            requestLedShow( -3 );
        }
        releaseCore1Frames( );
        requestLedShow( -1 );
        // delay(100);
        input = '!';
    } else if ( input == 'p' ) {
        for ( int i = 0; i < LED_COUNT; i++ ) {
            uint32_t color = leds.getPixelColor( i );
            rgbColor currentPixel = unpackRgb( color );
            char padZero = '0';

            // String colorString =
            //     ("0x") + (currentPixel.r > 15 ? : '0') + String(currentPixel.r,
            //     16)
            //     + (currentPixel.g > 15 ? : '0') + String(currentPixel.g, 16) +
            //     (currentPixel.b > 15 ? : '0') + String(currentPixel.b, 16);
            Serial.print( "0x" );
            currentPixel.r > 15 ?: Serial.print( padZero );
            Serial.print( currentPixel.r, 16 );
            currentPixel.g > 15 ?: Serial.print( padZero );
            Serial.print( currentPixel.g, 16 );
            currentPixel.b > 15 ?: Serial.print( padZero );
            Serial.print( currentPixel.b, 16 );
            Serial.print( ", " );

            if ( i % 8 == 0 && i > 0 ) {
                Serial.println( );
            }

            // Serial.println(colorString);
        }

        return ' ';
    } else if ( input == '?' ) {
        showLoss( );
        while ( Serial.available( ) == 0 ) {
        }
        requestLedShow( -1 );
        return ' ';
    } else {
        saveLEDbrightness( 0 );
        // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
        requestLedShow( 1 ); // Trigger LED update on core 2

        return input;
    }
    return input;
}

void showLoss( void ) {
    b.clear( );
    requestLedShow( -3 );
    uint32_t guyColor = 0x0a0a1a;
    uint32_t hairColor = 0x1a0902;
    uint32_t nurseColor = 0x1a0207;
    uint32_t doctorColor = 0x1b0a1e;
    uint32_t patientColor = 0x070a0f;

    b.printRawRow( 0b00001111, 5, guyColor, hairColor );

    b.printRawRow( 0b00001111, 20, guyColor, hairColor );
    b.printRawRow( 0b00001111, 25, nurseColor, 0xffffff );

    b.printRawRow( 0b00001111, 34, guyColor, hairColor );
    b.printRawRow( 0b00011111, 38, doctorColor, 0xffffff );

    b.printRawRow( 0b00001111, 49, guyColor, hairColor );
    b.printRawRow( 0b00000010, 51, patientColor, 0xffffff );
    b.printRawRow( 0b00000010, 52, patientColor, 0xffffff );
    b.printRawRow( 0b00000010, 53, patientColor, 0xffffff );
    b.printRawRow( 0b00000010, 54, patientColor, 0xffffff );
    b.printRawRow( 0b00000010, 55, patientColor, 0xffffff );
    b.printRawRow( 0b00000010, 56, patientColor, 0xffffff );
}
/*
0Show
1  Voltage
2    0 1 2
3      Nodes
1  Current
2    Nodes
1  Digital
2    5V
3      0 1 2 3
4        Nodes
2    3.3V
3      0 1 2 3
4        Nodes
2    UART
3      Tx Rx
4        Nodes
5          2nd USBPrint
6            Baud
2    I2C
3      SDA SCL
4        Nodes
5          2nd USBPrint
6            Baud
1  Options
2    Middle Out
3      Range
2    Bottom Up
3      Range
2    Bright
3      Range
2    Color
3      Range
0Rails
1  Both
2    VoltageSet
1  Top
2    VoltageSet
1  Bottom
2    VoltageSet
0Slots
1  Save
2    Which?
1  Load
2    Which?
1  Clear
2    Which?
0Output
1  Volage
4        5V or ±8V?
3      VoltageSet
4        Nodes
1  Digital
2    5V
3      0 1 2 3
4        Nodes
2    3.3V
3      0 1 2 3
4        Nodes
1  UART
2    Tx Rx
3      Nodes
4        2nd USBPrint
5          Baud
1  Buffer
2    In Out
3      Nodes
0Arduino
1  UART
2    Tx Rx
3      Nodes
4        2nd USBPrint
5          Baud
1  Reset
0Probe
1  Connect
1  Clear
1  Calibration



// */

// void drawMainMenu(void) {
//     clearDisplay();
//     drawString("Jumperless", 0, 0, 2, WHITE);
//     drawLine(0, 20, 128, 20, WHITE);

//     const char* options[] = {
//         "Nets",
//         "Probing",
//         "Settings",
//         "Apps",
//         "Config"
//     };
//     const int numOptions = 5;
//     menuPositionMax = numOptions - 1;
//     menuPositionMin = 0;

//     // Draw menu options
//     for (int i = 0; i < numOptions; i++) {
//         int y = 24 + (i * 16);
//         if (menuPosition == i) {
//             drawString(">", 0, y, 2, WHITE);
//             drawString(options[i], 20, y, 2, WHITE);
//         } else {
//             drawString(options[i], 20, y, 2, DARKGREY);
//         }
//     }

//     // Handle menu selection
//     if (menuConfirm) {
//         menuConfirm = 0;
//         switch (menuPosition) {
//             case 0:
//                 menuState = MENU_NETS;
//                 break;
//             case 1:
//                 menuState = MENU_PROBING;
//                 break;
//             case 2:
//                 menuState = MENU_SETTINGS;
//                 break;
//             case 3:
//                 menuState = MENU_APPS;
//                 break;
//             case 4:
//                 menuState = MENU_CONFIG;
//                 break;
//         }
//         menuPosition = 0;
//     }
// }

// void drawConfigMenu(void) {
//     clearDisplay();
//     drawString("Configuration", 0, 0, 1, WHITE);
//     drawLine(0, 10, 128, 10, WHITE);

//     // Menu options
//     const char* options[] = {
//         "Print Config",
//         "Load Config",
//         "Reset to Default",
//         "Back"
//     };
//     const int numOptions = 4;
//     menuPositionMax = numOptions - 1;
//     menuPositionMin = 0;

//     // Draw menu options
//     for (int i = 0; i < numOptions; i++) {
//         int y = 16 + (i * 12);
//         if (menuPosition == i) {
//             drawString(">", 0, y, 1, WHITE);
//             drawString(options[i], 10, y, 1, WHITE);
//         } else {
//             drawString(options[i], 10, y, 1, DARKGREY);
//         }
//     }

//     // Handle menu selection
//     if (menuConfirm) {
//         menuConfirm = 0;
//         switch (menuPosition) {
//             case 0: // Print Config
//                 printConfigToSerial();
//                 break;
//             case 1: // Load Config
//                 readConfigFromSerial();
//                 break;
//             case 2: // Reset to Default
//                 resetConfigToDefaults();
//                 saveConfig();
//                 Serial.println("Configuration reset to defaults");
//                 break;
//             case 3: // Back
//                 menuState = MENU_MAIN;
//                 menuPosition = 0;
//                 break;
//         }
//     }
// }

// void drawMenu(void) {
//     switch (menuState) {
//         case MENU_MAIN:
//             drawMainMenu();
//             break;
//         case MENU_NETS:
//             drawNetsMenu();
//             break;
//         case MENU_PROBING:
//             drawProbingMenu();
//             break;
//         case MENU_SETTINGS:
//             drawSettingsMenu();
//             break;
//         case MENU_APPS:
//             drawAppsMenu();
//             break;
//         case MENU_CONFIG:
//             drawConfigMenu();
//             break;
//     }
// }

volatile bool g_historyScrubActive = false;

// =============================================================================
// History scrub menu
//
// Live per-transaction scrub over the undo ring driven by the clickwheel.
// Each detent applies one txn (forward or backward) immediately via
// undoScrubTo() so the breadboard mirrors the past state in real time
// (CH446Q crosspoints + LEDs are refreshed inside undoUndo/undoRedo).
//
// Controls:
//   - Encoder turn CW  -> step backward in time (one txn older)
//   - Encoder turn CCW -> step forward (newer / into redo land)
//   - Encoder short click OR probe connect -> commit (cursor stays put)
//   - Encoder long press OR probe disconnect -> cancel (rewind to entry)
//
// Per-txn (not waypoint) per user preference. PSRAM bandwidth handles
// ~10 revert/apply per second easily; OLED label paint dominates.
// =============================================================================
void runHistoryScrubMenu( void ) {
    int entryPos = undoPosition( );
    int total = undoTotalTxns( );
    if ( total == 0 ) {
        oled.clearPrintShow( "history\nempty", 2, true, true, true );
        uint32_t t0 = millis( );
        while ( millis( ) - t0 < 700 ) {
            jOS.serviceInner( );
            delay( 5 );
        }
        return;
    }

    // Route the clickwheel through the shared menu encoder path
    // (encoderDirectionState UP/DOWN) so this screen gets the same
    // phase-independent detent hysteresis as the top-level menu instead of
    // re-deriving steps from the raw count. rotaryDivider sets sensitivity:
    // ~12 raw counts per step ≈ 3 detents on the V5 encoder (which yields
    // ~4 quad counts per detent). Restored on exit in the cleanup below.
    int lastDivider = rotaryDivider;
    rotaryDivider = 12;

    Menus::getInstance( ).inClickMenu = 1;
    g_historyScrubActive = true;     // tell Core 2 to keep painting nets
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;    // drop any step left over from the opening click
    encoderDirectionConsumed = true;

    int scrub = entryPos;

    // Repaint OLED ONLY - we deliberately do NOT paint text onto the
    // breadboard LEDs here, so the user sees the actual reverted
    // connections (driven by undoScrubTo -> refreshConnections inside
    // undoUndo/undoRedo) exactly the way the hold-disconnect gesture
    // shows them on the main screen.
    //
    // Same two-tier typography as the undo toast (large position
    // counter on top, smaller label below) so the scrub UI matches the
    // single-step toast visually.
    auto repaint = [ & ]( ) {
        const char* lbl = undoLabelAt( 0 );
        if ( !lbl || !lbl[ 0 ] ) lbl = "<start>";
        char buf[ 80 ];
        snprintf( buf, sizeof( buf ), "%d/%d\n%s", -scrub, total, lbl );
        oled.clearPrintShowSmall( buf );
    };
    repaint( );

    while ( true ) {
        delayMicroseconds( 200 );
        rotaryEncoderStuff( );
        jOS.serviceInner( );

        // Keep the yellow undo indicator lit for the duration of the
        // scrub session - re-extend the window each loop.
        undoFlashLogo( 250 );

        int probePress = ProbeButton::getInstance( ).getButtonPress( ); // one read per pass (see getMenuSelection)
        if ( encoderButtonState == HELD
             || probePress == disconnectPress ) {
            undoScrubTo( entryPos );
            break;
        }

        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED )
             || probePress == connectPress ) {
            encoderButtonState = IDLE;
            break;
        }

        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            undoScrubTo( entryPos );
            break;
        }

        // Consume one logical detent step from the shared menu path.
        // UP = step NEWER (toward redo), DOWN = step OLDER (toward undo) —
        // matches the previous CW = older mapping.
        int step = 0;
        if ( encoderDirectionState == UP ) {
            step = +1;
            encoderDirectionState = NONE;
        } else if ( encoderDirectionState == DOWN ) {
            step = -1;
            encoderDirectionState = NONE;
        }
        if ( step != 0 ) {
            int next = scrub + step;
            if ( next > 0 ) next = 0;
            if ( next < -total ) next = -total;
            if ( next != scrub ) {
                scrub = next;
                undoScrubTo( scrub );
                repaint( );
            }
        }
    }

    g_historyScrubActive = false;
    rotaryDivider = lastDivider;
    Menus::getInstance( ).inClickMenu = 0;
    // Repaint the final reverted/replayed state on the breadboard so the
    // user sees the connections (not the menu's previous text overlay).
    // doMenuAction's caller also calls refreshConnections after we return.
    refreshConnections( -1, 1, 0 );
}
