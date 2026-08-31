#include "Highlighting.h"
#include "Adafruit_USBD_CDC.h"
#include "AsyncPassthrough.h"
#include "JumperlOS.h"
#include "Graphics.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "States.h"
#include "NetManager.h"
#include "NetVoltageScan.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "ArduinoStuff.h"
#include "Probing.h"
#include "MeasureMode.h"  // For MEASUREMODE_TAKES_PRECEDENCE macro
#include "RotaryEncoder.h"
#include "config.h"
#include "hardware/i2c.h"
#include "hardware/structs/io_bank0.h"
#include "oled.h"
#include "PersistentStuff.h"
#include "Commands.h"
#include "FileParsing.h"
#include "Undo.h"
#include "Menus.h"
#include "ReadingDisplay.h"
#include "PartLabels.h"      // partLabels - the part-highlight overlay channel
#include "PartsApp.h"        // partsShowPartCard - the scroll's part card
#include "PartPlacement.h"   // partPinNode - semantic labels for part-pin nets
#include <Arduino.h>
#include <cmath>

// ---------------------------------------------------------------------------
// Semantic readout names (Kevin's ruling, 2026-08-25: show meaning, not
// numbers). A placed part's pin on the net wins ("SSD1306 SDA"); else a GPIO
// whose RP2350 FUNCSEL really is I2C reports its bus role straight from the
// pin function register (even GPIO = SDA, odd = SCL - the silicon's fixed
// map). Returns false when neither applies - the caller keeps its numeric
// name. userGpioIdx is the gpioDef index (-1 when not a GPIO readout).
// (netHasNode is PartLabels' netContainsNode shape - that one is file-local.)
static bool netHasNode(int netNum, int node) {
    if (netNum <= 0 || netNum >= MAX_NETS) return false;
    if (globalState.connections.nets[netNum].number != netNum) return false;
    for (int n = 0; n < MAX_NODES && globalState.connections.nets[netNum].nodes[n] != 0; n++) {
        if (globalState.connections.nets[netNum].nodes[n] == node) return true;
    }
    return false;
}
static bool netSemanticName(int netNum, int userGpioIdx, char* out, size_t outLen) {
    // Special-function nets (GND, rails, DACs) keep their authoritative
    // names - probing GND must say GND, not the first placed part's power
    // pin (sweep finding). Same predicate partsReassertNetNames uses.
    if (netNum > 0 && netNum < MAX_NETS &&
        globalState.connections.nets[netNum].number == netNum &&
        globalState.connections.nets[netNum].specialFunction <= 0) {
        for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
            const PartDefinition& p = globalState.parts.parts[i];
            if (!p.placed) continue;
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                int node = partPinNode(p, p.pins[j]);
                if (node < 1 || node > 60) continue;
                if (!netHasNode(netNum, node)) continue;
                // Truncate the part NAME, never the pin suffix - in a 16-byte
                // caller buffer "SSD1306_2 SDA" must not lose the "SDA"
                // (sweep finding).
                const char* pinName = p.pins[j].name;
                int pinLen = (int)strlen(pinName);
                int nameBudget = (int)outLen - 1 - pinLen - 1;   // NUL + space
                if (nameBudget < 1) nameBudget = 1;
                snprintf(out, outLen, "%.*s %s", nameBudget, p.name, pinName);
                return true;
            }
        }
    }
    if (userGpioIdx >= 0) {
        int rpPin = gpioDef[userGpioIdx][0];
        if (gpio_get_function((uint)rpPin) == GPIO_FUNC_I2C) {
            snprintf(out, outLen, "GPIO %d %s", userGpioIdx + 1,
                     (rpPin & 1) ? "SCL" : "SDA");
            return true;
        }
    }
    return false;
}

// ============================================================================
// Highlighting Class Implementation
// ============================================================================

// Static member initialization
Highlighting& Highlighting::getInstance() {
    static Highlighting inst;
    return inst;
}

Highlighting::Highlighting() {
    // Initialize colors
    highlightedOriginalColor = {0, 0, 0};
    brightenedOriginalColor = {0, 0, 0};
    warningOriginalColor = {0, 0, 0};
}

/**
 * @brief Main service method for highlighting system
 * 
 * This is called each loop iteration and handles:
 * - Encoder-based net highlighting (CRITICAL - must run every loop for smooth UX)
 * - Probe reading integration
 * - Warning timeouts (rate-limited)
 * - Reading change detection (rate-limited)
 * - Measure mode continuous voltage display (when switch in measure position)
 * 
 * OPTIMIZATION: encoder highlighting runs every loop for instant response,
 * but timeout checks and change detection are rate-limited to reduce overhead.
 */
ServiceStatus Highlighting::service() {
    lastStatus = ServiceStatus::IDLE;
    //return lastStatus;

    // ============================================================================
    // CRITICAL PATH: Button press check - MUST happen before encoder highlighting
    // ============================================================================
    // Check for encoder button press on persistent nodes (rails, DACs) FIRST
    // This prevents encoder movements from changing highlighting before voltage adjustment
    if (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) {
        if (handleEncoderButtonPress()) {
            // Button press was handled (voltage adjustment, etc.)
            // CRITICAL: Consume ALL encoder state so nothing else processes it
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            encoderDirectionState = NONE;  // Also consume any pending direction changes
            lastStatus = ServiceStatus::BLOCKING;
            return lastStatus;
        }
        // If not handled, button press will be processed by other systems (menus, etc.)
    }
    // ============================================================================
    // CRITICAL PATH: Encoder highlighting - MUST run every loop for smooth UX
    // ============================================================================
    int encoderNetHighlighted = encoderNetHighlight();
    
    // NOTE: Measure mode handling has been moved to the MeasureMode service
    // (see MeasureMode.h/cpp) for cleaner separation of concerns
    
    // ============================================================================
    // MEASURE MODE PRECEDENCE CHECK
    // ============================================================================
    // When switch is in measure mode (position 0) and MeasureMode service is active,
    // skip probe highlighting to avoid display conflicts
#if MEASUREMODE_TAKES_PRECEDENCE
    if (switchPosition == 0 && measureModeService.isMeasurementActive()) {
        // MeasureMode is handling display - skip probe highlighting entirely
        return lastStatus;
    }
#endif
    

    // Get probe reading from Probing service (cheap - cached value)
    int probeReading = probing.getLastProbeReading();
    
    // Handle probe-based highlighting (only if probe is touching something)
    if (probeReading > 0) {
        if (highlightNets(probeReading) > 0) {
            firstConnection = probeReading;
            lastStatus = ServiceStatus::BUSY;
        }
    }
    
    // ============================================================================
    // NON-CRITICAL PATH: Timeout checks and change detection
    // Rate-limit these to 25Hz (every 40ms) - no need to check every loop
    // ============================================================================
    static unsigned long lastPeriodicCheckTime = 0;
    unsigned long now = millis();
    if (now - lastPeriodicCheckTime >= 40) {  // 40ms = 25Hz
        lastPeriodicCheckTime = now;
        
        // Handle warning timeouts
        warnNetTimeout(1);
        
        // Check for reading changes
        checkForReadingChanges();
    }
    
    // Track changes for LED updates
    if (lastHighlightedNet != highlightedNet ||
        lastBrightenedNet != brightenedNet ||
        lastWarningNet != warningNet) {
        lastHighlightedNet = highlightedNet;
        lastBrightenedNet = brightenedNet;
        lastWarningNet = warningNet;
        lastStatus = ServiceStatus::BUSY;
    }
    
    return lastStatus;
}

// Backward compatibility - create references to singleton members
rgbColor& highlightedOriginalColor = Highlighting::getInstance().highlightedOriginalColor;
rgbColor& brightenedOriginalColor = Highlighting::getInstance().brightenedOriginalColor;
rgbColor& warningOriginalColor = Highlighting::getInstance().warningOriginalColor;
int& firstConnection = Highlighting::getInstance().firstConnection;
int& showReadingRow = Highlighting::getInstance().showReadingRow;
int& showReadingNet = Highlighting::getInstance().showReadingNet;
int& highlightedRow = Highlighting::getInstance().highlightedRow;
int& lastNodeHighlighted = Highlighting::getInstance().lastNodeHighlighted;
int& highlightedNet = Highlighting::getInstance().highlightedNet;
int& probeConnectHighlight = Highlighting::getInstance().probeConnectHighlight;
int& brightenedNode = Highlighting::getInstance().brightenedNode;
int& brightenedNet = Highlighting::getInstance().brightenedNet;
int& brightenedRail = Highlighting::getInstance().brightenedRail;
int& brightenedAmount = Highlighting::getInstance().brightenedAmount;
int& brightenedNodeAmount = Highlighting::getInstance().brightenedNodeAmount;
int& brightenedNetAmount = Highlighting::getInstance().brightenedNetAmount;
int& warningRow = Highlighting::getInstance().warningRow;
int& warningNet = Highlighting::getInstance().warningNet;
unsigned long& warningTimeout = Highlighting::getInstance().warningTimeout;
unsigned long& warningTimer = Highlighting::getInstance().warningTimer;
unsigned long& highlightTimer = Highlighting::getInstance().highlightTimer;

// ============================================================================
// Existing Functions (now class methods)
// ============================================================================

// ---------------------------------------------------------------------------
// Live-reading change-detection guards.
//
// Every "print only when it changed" latch used by highlightNets() and
// checkForReadingChanges() lives here instead of in function-local statics, so
// resetReadingState() can force the next paint. The case that needs it: a
// Python script exits with the SAME net still selected and UNCHANGED values -
// with the guards untouched, nothing would ever repaint after the handback.
//
// Reset ONLY from resetReadingState(). Never from clearHighlighting(): that
// runs every command cycle (main loop top + printMenu) and resetting here would
// force a repaint each time. Float sentinels are an impossible reading, not
// 0.0 (a live 0.00 V equalled the old sentinel and skipped the first paint) and
// not NaN (fabs(x - NaN) > t is always false, which would skip forever).
// ---------------------------------------------------------------------------
namespace {
constexpr float kNoReading = -1000.0f;

struct LiveReadingGuards {
    // highlightNets(): I Sense pair
    bool          sensePrintInitialized   = false;
    int           lastSenseNetPrinted     = -1;
    float         lastSenseCurrentPrinted = kNoReading;
    float         lastSenseVoltagePrinted = kNoReading;
    unsigned long lastSenseUpdatePrinted  = 0;
    // highlightNets(): UART live view (append-only diff)
    char   prevTx[64] = {0};
    size_t prevTxLen  = 0;
    char   prevRx[64] = {0};
    size_t prevRxLen  = 0;
    // highlightNets(): plain-net "Net N / row X" reprint guard
    int lastPrintedRowNode = -1;
    // checkForReadingChanges()
    float         prevAdcReading      = kNoReading;
    int           prevGpioInputState  = -1;
    int           prevGpioOutputState = -1;
    float         prevDacVoltage      = kNoReading;
    float         prevRailVoltage     = kNoReading;
    int           lastMeasuredNet     = -1;
    unsigned long lastUpdateTime      = 0;
    char   prevUartTx[64] = {0};
    size_t prevUartTxLen  = 0;
    char   prevUartRx[64] = {0};
    size_t prevUartRxLen  = 0;
    float  prevShownCurrent = kNoReading;
    // checkForReadingChanges(): I Sense pair
    float lastCurrentPrinted = kNoReading;
    float lastVoltagePrinted = kNoReading;
    // checkForReadingChanges(): estimated-current / scan trio
    float lastEstCurrentPrinted = kNoReading;
    int   lastEstNetPrinted     = -1;
    char  lastScanVPrinted[16]  = "";

    void reset( ) { *this = LiveReadingGuards( ); }
};
LiveReadingGuards g_readingGuards;
} // namespace

void Highlighting::clearHighlighting( int updateLEDs) {

    // netColors[highlightedNet] = highlightedOriginalColor;
    // netColors[brightenedNet] = brightenedOriginalColor;
    // netColors[warningNet] = warningOriginalColor;
    // Serial.println("clearHighlighting");
    // Serial.flush();
    for ( int i = 4; i < numberOfRowAnimations; i++ ) {
        rowAnimations[ i ].row = -1;
        rowAnimations[ i ].net = -1;
    }
    probeConnectHighlight = -1;
    highlightedNet = -1;
    brightenedNet = -1;
    warningNet = -1;
    warningRow = -1;
    brightenedRail = -1;
    brightenedNode = -1;
    highlightedRow = -1;

    firstConnection = -1;

    // Re-tapping the same net after the highlight cleared must reprint.
    lastPrintedNet = -1;
    // Whatever is on the panel is no longer ours to dedupe against (a menu,
    // toast or script may paint over it), so force the next reading to draw.
    ReadingDisplay::resetLastShown( );
    // The PARTSEL trace belongs to the selection that just ended: blank it so
    // it can't hover over whatever reading comes next. (Content only - the
    // pin keeps its shape, and the next part stop rewrites the row in place.)
    ReadingDisplay::clearLiveStatusLine( );

    // Reset timers
    highlightTimer = 0;
    warningTimeout = 0;
    warningTimer = 0;
   // leds.clear( );

    // The part-highlight overlay rides the same lifecycle: no highlight, no
    // role-color dots (PartLabels re-shows them the next time something on
    // the part is highlighted or tapped).
    partLabels.clearPartHighlight( );

    // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
    // Use negative value to force clearBeforeSend, ensuring old highlights are fully cleared
    if (updateLEDs != 0) {
    requestLedShow( updateLEDs );  // Trigger full clear + LED update on core 2
    }
}

// Drop the latches that keep a reading on the panel: the live updater's
// target net and the reprint guards. The highlight TIMEOUT deliberately
// leaves these alone - a reading staying visible after the LEDs fade is the
// feature - so this is the separate, explicit "someone else owns the display
// now" reset, used when handing back from a Python script.
void Highlighting::resetReadingState( ) {
    showReadingNet = -1;
    showReadingRow = -1;
    lastPrintedNet = -1;
    g_readingGuards.reset( );   // force the next paint even for an unchanged value
    ReadingDisplay::resetLastShown( );
}

int lastReturnNode = -1;
int scrolledRow = -1;

// Part focus for the mode-1 scroll: a placed part is a first-class stop
// (Kevin's spec, 2026-08-27). Landing on any of its rows highlights the
// WHOLE part (role-color dots + the OLED card with pin assignments and
// cached test data); the next detents walk its pins - every pin of this
// half's span before anything else - then the row scan resumes past it.
// Reachable with zero wires: the stop check is part geometry, not paths[].
static int8_t scrollPartIdx = -1;   // -1 = not in part focus
static int8_t scrollPartPin = -1;   // -1 = the whole part

// Every part-removal path calls this (Highlighting.h): the raw indices
// above and PartLabels' highlight slot go stale the moment the parts table
// compacts - the in-loop guard can only see out-of-range or unplaced, not
// a DIFFERENT part shifted into the same slot (audit, 2026-08-27).
extern "C" void highlightingInvalidatePartFocus(void) {
    scrollPartIdx = -1;
    scrollPartPin = -1;
    partLabels.clearPartHighlight();
}

// The part card (partsShowPartCard) paints the OLED without going through
// ReadingDisplay, so the live-reading pipeline's dedupe key AND the one-shot
// reprint guards (lastPrintedNet / lastPrintedRowNode) still describe a
// panel that is no longer there. Without this reset, scrolling back onto a
// wired pin whose reading text hadn't changed early-returned before the
// OLED write and left the previous pin's card up (Kevin's bench, 08:38 -
// "scroll in from one direction sometimes doesn't update").
//
// EXACTLY these three latches, not resetReadingState(): the full reset also
// wipes the live-value dead zones (prevRailVoltage & co), and with a STALE
// highlight still armed (tap Top Rail, then inspect-tap an unwired part
// leg - brightenNet(unwired) leaves highlightedNet parked on the rail) the
// forced first repaint stomped the card ~80ms after it painted (adversarial
// review, 09:12). The one-shot paints in highlightNets gate only on the
// two guards below, so this narrow set is sufficient for the scroll fix.
extern "C" void highlightingNoteExternalOledPaint(void) {
    Highlighting::getInstance().lastPrintedNet = -1;
    g_readingGuards.lastPrintedRowNode = -1;
    ReadingDisplay::resetLastShown();
}

// First placed part with a pin on this row (its pin index in *pinIdx).
static int scrollPartOnRow( int row, int* pinIdx ) {
    if ( row < 1 || row > 60 ) return -1;
    for ( int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++ ) {
        const PartDefinition& p = globalState.parts.parts[ i ];
        if ( !p.placed ) continue;
        for ( int j = 0; j < p.numPins && j < MAX_PART_PINS; j++ ) {
            if ( partPinNode( p, p.pins[ j ] ) == row ) {
                if ( pinIdx != nullptr ) *pinIdx = j;
                return i;
            }
        }
    }
    return -1;
}

extern unsigned long persistentHighlightTimeout;   // defined below (line ~870)

// The part's next pin in travel order within the current half: the lowest
// pin row above `fromRow` (up) / highest below (down). `inclusive` admits
// fromRow itself - the whole-part -> first-pin step. Returns the pin index
// or -1 when the span is exhausted.
static int scrollPartNextPin( const PartDefinition& p, int fromRow, bool up, bool inclusive ) {
    bool topHalf = ( fromRow <= 30 );
    int best = -1, bestRow = up ? 61 : 0;
    for ( int j = 0; j < p.numPins && j < MAX_PART_PINS; j++ ) {
        int r = partPinNode( p, p.pins[ j ] );
        if ( r < 1 || r > 60 || ( r <= 30 ) != topHalf ) continue;
        bool beyond = up ? ( r > fromRow || ( inclusive && r == fromRow ) )
                         : ( r < fromRow || ( inclusive && r == fromRow ) );
        if ( !beyond ) continue;
        if ( up ? ( r < bestRow ) : ( r > bestRow ) ) {
            bestRow = r;
            best = j;
        }
    }
    return best;
}

int Highlighting::encoderNetHighlight( int print, int mode, int divider ) {

    int lastDivider = rotaryDivider;
    rotaryDivider = divider;
    int returnNode = -1;

    // Serial.print (" highlightedNet: ");
    // Serial.println(highlightedNet);

    // Serial.print (" brightenedRail: ");
    // Serial.println(brightenedRail);

    // Serial.print (" brightenedNode: ");
    // Serial.println(brightenedNode);

    // Serial.flush();
    // if (inClickMenu == 1)
    //   return -1;
    // rotaryEncoderStuff();
    if ( mode == 0 ) {
        if ( encoderDirectionState == UP ) {
            // Serial.println(encoderPosition);
            encoderDirectionState = NONE;
            if ( highlightedNet < 0 ) {
                highlightedNet = -1;
                brightenedNet = -1;
                currentHighlightedNode = 0;
            }
            currentHighlightedNode++;
            if ( highlightedNet >= 0 && highlightedNet < numberOfNets && ( currentHighlightedNode >= MAX_NODES || globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ] <= 0 ) ) {
                currentHighlightedNode = 0;
                highlightedNet++;
                if ( highlightedNet > numberOfNets - 1 ) {
                    highlightedNet = -2;
                    brightenedNet = -2;
                    currentHighlightedNode = 0;
                }
                brightenedNet = highlightedNet;
                if ( highlightedNet >= 0 && highlightedNet < numberOfNets ) {
                    brightenedNode = globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ];
                    if ( highlightedNet != 0 && globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ] != 0 ) {
                        returnNode = globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ];
                    }
                } else {
                    brightenedNode = -1;
                }
                highlightNets( 0, highlightedNet, print );
                // Serial.print("highlightedNet: ");
                // Serial.println(highlightedNet);
                // Serial.flush();
            }
            if ( highlightedNet > numberOfNets - 1 ) {
                highlightedNet = -2;
                brightenedNet = -2;
                currentHighlightedNode = 0;
            }

            brightenedNet = highlightedNet;

            if ( highlightedNet >= 0 && highlightedNet < numberOfNets ) {
                brightenedNode = globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ];
                if ( highlightedNet != 0 && globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ] != 0 ) {
                    returnNode = globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ];
                }
            } else {

                brightenedNode = -1;
            }
            // Serial.print("returnNode: ");
            // Serial.println(returnNode);
            // Serial.flush();
            highlightNets( 0, highlightedNet, print );
            // Serial.print("highlightedNet: ");
            // Serial.println(highlightedNet);
            // Serial.flush();
            // assignNetColors();
            // assignNetColors();

        } else if ( encoderDirectionState == DOWN ) {
            // Serial.println(encoderPosition);
            encoderDirectionState = NONE;
            if ( highlightedNet == 0 ) {

                highlightedNet = numberOfNets - 1;
                brightenedNet = numberOfNets - 1;
            }

            currentHighlightedNode--;

            if ( currentHighlightedNode < 0 ) {
                highlightedNet--;
                if ( highlightedNet < 0 ) {
                    highlightedNet = numberOfNets - 1;
                    brightenedNet = numberOfNets - 1;
                    currentHighlightedNode = 0;
                }
                currentHighlightedNode = MAX_NODES - 1;
                // Every iteration must leave currentHighlightedNode inside
                // [0, MAX_NODES-1] - it is the index the loop test and the
                // reads below it dereference - and the walk needs a bound of
                // its own, since an all-empty net table would cycle forever.
                int rescanLimit = numberOfNets * MAX_NODES;
                while ( rescanLimit-- > 0 && highlightedNet >= 0 && highlightedNet < numberOfNets && globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ] <= 0 ) {
                    currentHighlightedNode--;
                    if ( currentHighlightedNode < 0 ) {
                        highlightedNet--;
                        if ( highlightedNet < 0 ) {
                            highlightedNet = numberOfNets - 1;
                            brightenedNet = numberOfNets - 1;
                            currentHighlightedNode = 0;
                        } else {
                            currentHighlightedNode = MAX_NODES - 1;
                        }
                    }
                }
            }
            brightenedNet = highlightedNet;
            if ( highlightedNet >= 0 && highlightedNet < numberOfNets ) {
                brightenedNode = globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ];
                if ( highlightedNet != 0 && globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ] != 0 ) {
                    returnNode = globalState.connections.nets[ highlightedNet ].nodes[ currentHighlightedNode ];
                }
            } else {
                brightenedNode = -1;
            }
            // Serial.print("returnNode: ");
            // Serial.println(returnNode);
            // Serial.flush();
            highlightNets( 0, highlightedNet, print );
            // Serial.print("highlightedNet: ");
            // Serial.println(highlightedNet);
            // Serial.flush();
            // assignNetColors();
        }
        if ( returnNode != lastNodeHighlighted ) {
            // b.clear();
            // b.printRawRow(0b00000100, lastNodeHighlighted-2, 0x000000, 0x000000);
            // b.printRawRow(0b00000100, lastNodeHighlighted, 0x0000000, 0x000000);

            // b.printRawRow(0b00000100, returnNode-2, 0x0f0f00, 0x000000);
            // b.printRawRow(0b00000100, returnNode, 0x0f0f00, 0x000000);

            lastNodeHighlighted = returnNode;
            // requestLedShow( 2 );
        }
        // rotaryDivider = lastDivider;
        return returnNode;
    } else if ( mode == 1 ) {
        // Initialize scrolledRow if needed (start with breadboard row 1)
        // Valid scroll targets: 1..60 (breadboard), TOP_RAIL, BOTTOM_RAIL, NANO_D0..NANO_RESET_1
        if ( scrolledRow == -1 || !(
                 ( scrolledRow >= 1 && scrolledRow <= 60 ) ||
                 ( scrolledRow == GND ) || ( scrolledRow == TOP_RAIL ) || ( scrolledRow == BOTTOM_RAIL ) ||
                 ( scrolledRow >= NANO_D0 && scrolledRow <= NANO_RESET_1 ) ) ) {
            scrolledRow = 1;
        }
        // Serial.print("                      \rscrolledRow = ");
        // Serial.print(scrolledRow);

        // Helper function to increment scrolledRow across breadboard + rails + nano ranges
        auto incrementRow = []( int& row ) {
            if ( row >= 1 && row < 60 ) {
                row++;
            } else if ( row == 60 ) {
                row = GND; // jump from breadboard to GND
            } else if ( row == GND ) {
                row = TOP_RAIL; // then to rails
            } else if ( row == TOP_RAIL ) {
                row = BOTTOM_RAIL;
            } else if ( row == BOTTOM_RAIL ) {
                row = NANO_D0; // jump from rails to nano header
            } else if ( row >= NANO_D0 && row < NANO_RESET_1 ) {
                row++;
            } else if ( row == NANO_RESET_1 ) {
                row = 1; // wrap around to start
            }
        };

        // Helper function to decrement scrolledRow across breadboard + rails + nano ranges
        auto decrementRow = []( int& row ) {
            if ( row > 1 && row <= 60 ) {
                row--;
            } else if ( row == 1 ) {
                row = NANO_RESET_1; // wrap around to end
            } else if ( row > NANO_D0 && row <= NANO_RESET_1 ) {
                row--;
            } else if ( row == NANO_D0 ) {
                row = BOTTOM_RAIL; // jump from nano to rails
            } else if ( row == BOTTOM_RAIL ) {
                row = TOP_RAIL;
            } else if ( row == TOP_RAIL ) {
                row = GND;
            } else if ( row == GND ) {
                row = 60; // jump from GND to breadboard
            }
        };

        // Land the part focus. pj = -1 is the WHOLE part, pj >= 0 one pin.
        // The part is the important thing (Kevin's spec): the card - name /
        // type / cached test data / pin-row assignments - owns the panel in
        // BOTH states, and highlightedNet stays parked so the live net
        // updater can never repaint "Net N" over it. A focused wired pin
        // still lights its net + row gradient (brightenedNet/Node feed the
        // LEDs directly); an unwired pin's row lights through the overlay.
        auto focusPart = [&]( int pi, int pj ) {
            const PartDefinition& p = globalState.parts.parts[ pi ];
            partLabels.setPartHighlight( pi, pj, persistentHighlightTimeout );
            highlightedNet = -1;
            // a fresh stamp, or a STALE timer from the previous net stop
            // fires the 1.8s clear mid-card (clearHighlighting tears the
            // part focus down with it)
            highlightTimer = millis( );
            int net = -1;
            int row = scrolledRow;
            bool specialNet = false;
            if ( pj < 0 ) {
                brightenedNet = -1;
                brightenedNode = -1;
            } else {
                row = partPinNode( p, p.pins[ pj ] );
                for ( int i = 0; i < numberOfPaths; i++ ) {
                    if ( globalState.connections.paths[ i ].node1 == row ||
                         globalState.connections.paths[ i ].node2 == row ) {
                        net = globalState.connections.paths[ i ].net;
                        break;
                    }
                }
                brightenedNet = ( net > 0 ) ? net : -1;
                brightenedNode = ( net > 0 ) ? row : -1;
                brightenedAmount = 80;
                // A pin wired to anything beyond the breadboard - a GPIO,
                // ADC, DAC, rail, nano pin - hands the panel to the live
                // net display: it knows how to show that thing (GPIO
                // HIGH/LOW with click-to-toggle feedback, ADC volts...)
                // and now says the pin's name too (showGpioPinReading).
                if ( net > 0 && net < MAX_NETS &&
                     globalState.connections.nets[ net ].number == net ) {
                    if ( net <= 5 ) specialNet = true;
                    for ( int n = 0; n < MAX_NODES && !specialNet; n++ ) {
                        int nd = globalState.connections.nets[ net ].nodes[ n ];
                        if ( nd == 0 ) break;
                        if ( nd > 60 ) specialNet = true;
                    }
                }
            }
            requestLedShow( -1 );
            if ( print ) {
                // PARTSEL rides the pinned status row and repaints in place -
                // the old raw "\r\n...\n" print scrolled the terminal on
                // every detent AND knocked the reading pin loose, so one pass
                // through a part left a screenful of stale selections behind.
                // Emitted BEFORE the card/reading below: the status paint
                // blanks the reading row, and the reading that belongs to
                // THIS stop repaints right after (a whole-part or unwired-pin
                // stop has none, and its row correctly stays blank).
                char status[ 128 ];
                size_t sl = (size_t)snprintf( status, sizeof( status ), "PARTSEL part=%s row=%d", p.name, row );
                if ( sl >= sizeof( status ) ) sl = sizeof( status ) - 1;
                if ( pj < 0 ) {
                    for ( int j = 0; j < p.numPins && j < MAX_PART_PINS && sl < sizeof( status ) - 1; j++ ) {
                        sl += (size_t)snprintf( status + sl, sizeof( status ) - sl, "%s%s", j ? " " : " pins=", p.pins[ j ].name );
                        if ( sl >= sizeof( status ) ) sl = sizeof( status ) - 1;
                    }
                } else if ( net > 0 ) {
                    snprintf( status + sl, sizeof( status ) - sl, " pin=%s net=%d", p.pins[ pj ].name, net );
                } else {
                    snprintf( status + sl, sizeof( status ) - sl, " pin=%s net=none", p.pins[ pj ].name );
                }
                ReadingDisplay::emitLiveStatusLine( status );
            }
            if ( specialNet ) {
                highlightNets( 0, net, print );   // the live card, part-named
            } else {
                partsShowPartCard( p, pj );
            }
        };

        // A stale part focus (part removed / table rewritten) resets cleanly.
        if ( scrollPartIdx >= 0 &&
             ( scrollPartIdx >= globalState.parts.numParts ||
               !globalState.parts.parts[ scrollPartIdx ].placed ) ) {
            scrollPartIdx = -1;
            scrollPartPin = -1;
            partLabels.clearPartHighlight( );
        }

        if ( encoderDirectionState == UP ) {
            encoderDirectionState = NONE;

            // Part focus first: every pin of this part (this half's span)
            // before anything else. The whole-part landing hands the first
            // detent to the entry pin (inclusive), then upward through the
            // span; exhausted, the row scan resumes past it.
            if ( scrollPartIdx >= 0 ) {
                const PartDefinition& p = globalState.parts.parts[ scrollPartIdx ];
                int pj = scrollPartNextPin( p, scrolledRow, true, scrollPartPin < 0 );
                if ( pj >= 0 ) {
                    scrollPartPin = (int8_t)pj;
                    scrolledRow = partPinNode( p, p.pins[ pj ] );
                    focusPart( scrollPartIdx, pj );
                    returnNode = scrolledRow;
                } else {
                    // leaving the part: its paint goes with it (bench,
                    // 2026-08-27 - the dots/row wash lingered 15s after
                    // scrolling on)
                    scrollPartIdx = -1;
                    scrollPartPin = -1;
                    partLabels.clearPartHighlight( );
                }
            }

            if ( returnNode == -1 ) {
                incrementRow( scrolledRow );

                // Find the next stop: a placed part's row (checked BEFORE the
                // wiring test, so a part with no wires is reachable), else a
                // row with connections, starting from current scrolledRow
                int originalRow = scrolledRow;
                do {
                    int pj = -1;
                    int pi = scrollPartOnRow( scrolledRow, &pj );
                    if ( pi >= 0 ) {
                        scrollPartIdx = (int8_t)pi;
                        scrollPartPin = -1;   // the whole-part landing
                        focusPart( pi, -1 );
                        returnNode = scrolledRow;
                        break;
                    }

                    bool foundConnection = false;
                    int connectedNet = -1;

                    // Check if current scrolledRow has any connections
                    for ( int i = 0; i < numberOfPaths; i++ ) {
                        if ( globalState.connections.paths[ i ].node1 == scrolledRow || globalState.connections.paths[ i ].node2 == scrolledRow ) {
                            foundConnection = true;
                            connectedNet = globalState.connections.paths[ i ].net;
                            break;
                        }
                    }
                    // Allow highlighting rails and GND even without explicit paths
                    if ( !foundConnection ) {
                        if ( scrolledRow == GND ) {
                            foundConnection = true;
                            connectedNet = 1;
                        } else if ( scrolledRow == TOP_RAIL ) {
                            foundConnection = true;
                            connectedNet = 2;
                        } else if ( scrolledRow == BOTTOM_RAIL ) {
                            foundConnection = true;
                            connectedNet = 3;
                        }
                    }

                    if ( foundConnection ) {
                        partLabels.clearPartHighlight( );   // a net stop never wears part paint
                        highlightedNet = connectedNet;
                        brightenedNet = connectedNet;
                        brightenedNode = scrolledRow;
                        brightenedAmount = 80; // Set brightness for highlighting
                        requestLedShow( -1 );
                        highlightNets( 0, highlightedNet, print );
                        returnNode = scrolledRow;
                        break;
                    } else {
                        incrementRow( scrolledRow );
                    }
                } while ( scrolledRow != originalRow ); // prevent infinite loop
            }

        } else if ( encoderDirectionState == DOWN ) {
            encoderDirectionState = NONE;

            // Part focus, mirrored: downward through this half's span.
            if ( scrollPartIdx >= 0 ) {
                const PartDefinition& p = globalState.parts.parts[ scrollPartIdx ];
                int pj = scrollPartNextPin( p, scrolledRow, false, scrollPartPin < 0 );
                if ( pj >= 0 ) {
                    scrollPartPin = (int8_t)pj;
                    scrolledRow = partPinNode( p, p.pins[ pj ] );
                    focusPart( scrollPartIdx, pj );
                    returnNode = scrolledRow;
                } else {
                    // leaving the part: its paint goes with it (see UP)
                    scrollPartIdx = -1;
                    scrollPartPin = -1;
                    partLabels.clearPartHighlight( );
                }
            }

            if ( returnNode == -1 ) {
                decrementRow( scrolledRow );

                // Find the next stop (part row first - see the UP branch)
                int originalRow = scrolledRow;
                do {
                    int pj = -1;
                    int pi = scrollPartOnRow( scrolledRow, &pj );
                    if ( pi >= 0 ) {
                        scrollPartIdx = (int8_t)pi;
                        scrollPartPin = -1;   // the whole-part landing
                        focusPart( pi, -1 );
                        returnNode = scrolledRow;
                        break;
                    }

                    bool foundConnection = false;
                    int connectedNet = -1;

                    // Check if current scrolledRow has any connections
                    for ( int i = 0; i < numberOfPaths; i++ ) {
                        if ( globalState.connections.paths[ i ].node1 == scrolledRow || globalState.connections.paths[ i ].node2 == scrolledRow ) {
                            foundConnection = true;
                            connectedNet = globalState.connections.paths[ i ].net;
                            break;
                        }
                    }
                    // Allow highlighting rails and GND even without explicit paths
                    if ( !foundConnection ) {
                        if ( scrolledRow == GND ) {
                            foundConnection = true;
                            connectedNet = 1;
                        } else if ( scrolledRow == TOP_RAIL ) {
                            foundConnection = true;
                            connectedNet = 2;
                        } else if ( scrolledRow == BOTTOM_RAIL ) {
                            foundConnection = true;
                            connectedNet = 3;
                        }
                    }

                    if ( foundConnection ) {
                        partLabels.clearPartHighlight( );   // a net stop never wears part paint
                        highlightedNet = connectedNet;
                        brightenedNet = connectedNet;
                        brightenedNode = scrolledRow;
                        brightenedAmount = 80; // Set brightness for highlighting
                        requestLedShow( -1 );
                        highlightNets( 0, highlightedNet, print );
                        returnNode = scrolledRow;
                        break;
                    } else {
                        decrementRow( scrolledRow );
                    }
                } while ( scrolledRow != originalRow ); // prevent infinite loop
            }
        }

        // Ensure we always return a value for mode == 1
        // if (returnNode == -1) {
        //   returnNode = scrolledRow;
        //   Serial.print("returnNode: ");
        //   Serial.println(returnNode);
        //   Serial.flush();
        // }
        if ( returnNode != lastReturnNode ) {
            lastReturnNode = returnNode;
            // Serial.print("returnNode: ");
            // Serial.println(returnNode);
            // Serial.flush();
        }
        return returnNode;
    }

    // Final return for any other modes
    return returnNode;
}

/// @brief  brighten a net
/// @param node - the node to brighten
/// @param addBrightness - the amount to brighten the net 
/// @return the net that was brightened

int Highlighting::brightenNet( int node, int addBrightness ) {

    if ( node == -1 ) {
        // Don't write to netColors[] directly - let assignNetColors() handle it
        // Just clear the brightened state and trigger LED update
        brightenedNode = -1;
        brightenedNet = 0;
        brightenedRail = -1;
        // Use negative value to force clearBeforeSend, ensuring old highlights are fully cleared
        requestLedShow( -1 );  // Trigger full clear + LED update
        return -1;
    }
    addBrightness = 0;

    for ( int i = 0; i < numberOfPaths; i++ ) {

        if ( node == globalState.connections.paths[ i ].node1 || node == globalState.connections.paths[ i ].node2 && globalState.connections.paths[ i ].duplicate == 0 ) {
            /// if (brightenedNet != i) {
            int netToHighlight = globalState.connections.paths[ i ].net;
            
            // // Skip standard highlighting for current sense nets - they use marching ants only
            // if ( currentSenseState.plusConnected && currentSenseState.minusConnected ) {
            //     if ( netToHighlight == currentSenseState.plusNet || netToHighlight == currentSenseState.minusNet ) {
            //         // Don't set brightenedNet, but allow the highlighting system to track it for OLED/persistence
            //         brightenedNode = node;  // Set for persistence check
            //         highlightedNet = netToHighlight;
            //         highlightedRow = node;
            //         requestLedShow( 1 );  // Trigger LED update to show marching ants
            //         return netToHighlight;
            //     }
            // }
            
            brightenedNet = netToHighlight;
            brightenedNode = node;
            // Serial.print("\n\n\rbrightenedNet: ");
            // Serial.println(brightenedNet);
            // Serial.print("net ");
            // Serial.print(globalState.connections.paths[i].net);
            if ( brightenedNet == 1 ) {// TOP_RAIL
                brightenedRail = 1;
                // lightUpRail(-1, 1, 1, addBrightness);
            } else if ( brightenedNet == 2 ) {// BOTTOM_RAIL
                brightenedRail = 0;
                // lightUpRail(-1, 0, 1, addBrightness);
            } else if ( brightenedNet == 3 ) {// GND
                brightenedRail = 2;
                // lightUpRail(-1, 2, 1, addBrightness);
            } else {
                brightenedRail = -1;
                // lightUpNet(brightenedNet, addBrightness);
            }
            // Serial.print("\n\rbrightenedNet = ");
            // Serial.println(brightenedNet);
            brightenedOriginalColor = netColors[ brightenedNet ];
            // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
            requestLedShow( 1 );  // Trigger LED update on core 2
            return brightenedNet;
        }
    }
    switch ( node ) {
    case ( GND ): {
        //  Serial.print("\n\rGND");
        brightenedNode = node;  // Set brightenedNode for button handler
        brightenedNet = 1;
        brightenedRail = 1;
        // lightUpRail(-1, 1, 1, addBrightness);
        return 1;
    }
    case ( TOP_RAIL ): {
        // Serial.print("\n\rTOP_RAIL");
        brightenedNode = node;  // Set brightenedNode for button handler
        brightenedNet = 2;
        brightenedRail = 0;
        // lightUpRail(-1, 0, 1, addBrightness);
        return 2;
    }
    case ( BOTTOM_RAIL ): {
        // Serial.print("\n\rBOTTOM_RAIL");
        brightenedNode = node;  // Set brightenedNode for button handler
        brightenedNet = 3;
        brightenedRail = 2;
        // lightUpRail(-1, 2, 1, addBrightness);
        return 3;
    }
    }

    return -1;
}

/// @brief  mark a net as warning
/// @param -1 to clear warning
/// @return warningNet
int Highlighting::warnNet( int node ) {
    // Serial.print("warnNet node = ");
    // Serial.println(node);
    // Serial.flush();
    if ( node == -1 ) {
        // Don't write to netColors[] directly - let assignNetColors() handle it
        // Just clear the warning state and trigger LED update
        warningNet = -1;
        warningRow = -1;
        // Serial.print("warningNet = ");
        // Serial.println(warningNet);
        // Serial.flush();
        // brightenedRail = -1;
        return -1;
    }
    // addBrightness = 0;
    warningRow = bbPixelToNodesMap[ node ];

    for ( int i = 0; i < numberOfPaths; i++ ) {

        if ( node == globalState.connections.paths[ i ].node1 || node == globalState.connections.paths[ i ].node2 ) {
            /// if (brightenedNet != i) {
            warningNet = globalState.connections.paths[ i ].net;

            // Serial.print("warningNet = ");
            // Serial.println(warningNet);
            // Serial.flush();

            if ( warningNet == 1 ) {
                // brightenedRail = 1;
                // lightUpRail(-1, 1, 1, addBrightness);
            } else if ( warningNet == 2 ) {
                // brightenedRail = 0;
                // lightUpRail(-1, 0, 1, addBrightness);
            } else if ( warningNet == 3 ) {
                // brightenedRail = 2;
                // lightUpRail(-1, 2, 1, addBrightness);
            } else {
                // brightenedRail = -1;
                // lightUpNet(brightenedNet, addBrightness);
            }

            warningOriginalColor = netColors[ warningNet ];
            // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
            requestLedShow( 1 );  // Trigger LED update on core 2
            warningTimer = millis( );
            return warningNet;
        }
    }

    return -1;
}

unsigned long lastWarningTimer = 0;
unsigned long lastHighlightTimer = 0;
unsigned long highlightTimeout = 1800;           // 3 seconds timeout for regular nets
unsigned long persistentHighlightTimeout = 15000; // 15 seconds timeout for rails, DACs, GPIO outputs

unsigned long lastFirstConnectionTimer = 0;

void Highlighting::warnNetTimeout( int clearAll ) {
    // Serial.print("warningTimer = ");
    // Serial.println(warningTimer);
    // Serial.print("warningTimeout = ");
    // Serial.println(warningTimeout);
    // Serial.flush();
    if ( lastWarningTimer == 0 ) {
        lastWarningTimer = millis( );
    }

    if ( lastHighlightTimer == 0 ) {
        lastHighlightTimer = millis( );
    }

    // Check for warning timeout
    if ( warningTimer > 0 && millis( ) - warningTimer > warningTimeout ) {
        // warningTimeout = 0;
        // Serial.println("warningTimer timeout");
        if ( clearAll == 1 ) {
            clearHighlighting( );
        } else {
            // netColors[warningNet] = warningOriginalColor;

            warningNet = -1;
            warningRow = -1;
        }
        lastWarningTimer = millis( );
        warningTimer = 0;

        // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
        requestLedShow( 1 );  // Trigger LED update on core 2
    } else {
        lastWarningTimer = millis( ) - lastWarningTimer;
        // Serial.print("lastWarningTimer = ");
        // Serial.println(lastWarningTimer);
        // Serial.flush();
        // warningTimer = millis();
    }

    // Check for highlighted net timeout
    // Use persistent node system to determine timeout duration
    unsigned long currentTimeout = highlightTimeout;

    if (shouldPersistHighlight(brightenedNode) || partLabels.partHighlightActive()) {
        // Persistent nodes (rails, DACs, GPIO outputs) get much longer
        // timeout - and so does an active part focus (the whole-part card
        // deserves reading time; its own 15s hold matches this window)
        currentTimeout = persistentHighlightTimeout;
    }

    if (highlightTimer > 0 && millis() - highlightTimer > currentTimeout) {
        clearHighlighting(); // Clear all highlighting when timeout expires
        highlightTimer = 0;
        lastHighlightTimer = millis();
    }
}

// ============================================================================
// Unified highlight display sink
// ============================================================================
// Every net-highlight reading - probe tap, clickwheel scroll, and the ~40ms
// live updater (checkForReadingChanges) - funnels through these two helpers
// so name resolution and the OLED/Serial formats can never drift apart
// between input sources again.

// Display name for a net: ISENSE fixed labels win (the live and one-shot
// paths used to disagree here), then the user's custom name, then the
// net's own name, then "Net N". buf must outlive the returned pointer.
static const char* netDisplayName( int net, char* buf, size_t bufLen ) {
    if ( currentSenseState.plusConnected && currentSenseState.minusConnected ) {
        if ( net == currentSenseState.plusNet )
            return "I Sense +";
        if ( net == currentSenseState.minusNet )
            return "I Sense -";
    }
    const char* name = globalState.display.getNetName( net );
    if ( name != nullptr && name[ 0 ] != '\0' )
        return name;
    name = globalState.connections.nets[ net ].name;
    if ( name != nullptr && name[ 0 ] != '\0' )
        return name;
    snprintf( buf, bufLen, "Net %d", net );
    return buf;
}

// The net's estimated current (net voltage scan) as a display string.
// ALWAYS returns a value ("0.0 mA" when nothing measurable) so voltage-
// bearing displays show both readings at all times. buf outlives the return.
static const char* netCurrentValue( int net, char* buf, size_t len ) {
    snprintf( buf, len, "%.1f mA", netCurrent_mA( net ) );
    return buf;
}

// Scanned voltage for the highlighted node (or the net's dominant-path
// midpoint) as a display string; nullptr when the scanner has no fresh
// sample. This is what lets PLAIN bridge nets - no ADC/DAC/rail attached -
// show a voltage line: the background scan already measured their nodes.
static const char* netScanVoltageValue( int net, char* buf, size_t len ) {
    float v;
    if ( brightenedNode > 0 && brightenedNode < NODE_VOLTAGE_MAX &&
         nodeVoltageValid( brightenedNode ) ) {
        v = nodeVoltage[ brightenedNode ];
    } else if ( net > 0 && net < NET_CURRENT_INFO_COUNT &&
                netCurrentInfo[ net ].valid ) {
        v = netCurrentInfo[ net ].voltage;
    } else {
        return nullptr;
    }
    snprintf( buf, len, "%.2f V", v );
    return buf;
}

// ---------------------------------------------------------------------------
// Routable UART display helpers. The framing string and the scrolling live
// view used to be spelled out inline in six near-identical copies.
// ---------------------------------------------------------------------------

// "9600 8N1"
static const char* uartConfigText( char* buf, size_t len ) {
    char parityChar;
    switch ( USBSer1.paritytype( ) ) {
    case 1: parityChar = 'O'; break;
    case 2: parityChar = 'E'; break;
    case 3: parityChar = 'M'; break;
    case 4: parityChar = 'S'; break;
    default: parityChar = 'N'; break;
    }
    snprintf( buf, len, "%d %d%c%d", USBSer1.baud( ), USBSer1.numbits( ),
              parityChar, USBSer1.stopbits( ) + 1 );  // API: 0 -> 1 stop bit
    return buf;
}

static const char* uartDirectionName( bool tx, bool rx ) {
    if ( tx && rx )
        return "UART Tx/Rx";
    return tx ? "UART Tx" : "UART Rx";
}

// Repaint the scrolling byte view from scratch: header, then whatever Tx/Rx
// we have. This renderer stays as-is - it streams live bytes into the small
// scrolling text buffer, which the fixed rich layout can't express.
static void uartRefreshLiveView( bool tx, bool rx, const char* txSnapshot,
                                 size_t txLen, const char* rxSnapshot,
                                 size_t rxLen ) {
    char cfg[ 24 ];
    char hdr[ 48 ];
    snprintf( hdr, sizeof( hdr ), "%s %s\n", uartDirectionName( tx, rx ),
              uartConfigText( cfg, sizeof( cfg ) ) );
    OLEDOut.clear( );
    OLEDOut.print( hdr );
    if ( txLen > 0 ) {
        OLEDOut.print( "Tx: " );
        OLEDOut.print( txSnapshot );
        OLEDOut.print( "\n" );
    }
    if ( rxLen > 0 ) {
        OLEDOut.print( "Rx: " );
        OLEDOut.print( rxSnapshot );
        OLEDOut.print( "\n" );
    }
}

// Gate for the rail/DAC click-to-adjust shortcut (dacs.rail_click_adjust):
// 0 = off, 1 = only with an OLED connected (the default - the OLED's
// "adjust?" prompt is what makes the click discoverable), 2 = always.
// All three touch points of the feature check this one predicate - the
// persistent highlight, the button-press claim, and the prompt - so an
// OLED-less board on the default setting behaves exactly as before.
static bool clickAdjustEnabled( void ) {
    int flag = jumperlessConfig.clickwheel.rail_click_adjust;
    return flag == 2 || ( flag == 1 && oledConnected );
}

// The prompt for adjustable readings: shown only while a click would
// actually enter the voltage adjuster.
static const char* adjustHintText( void ) {
    return clickAdjustEnabled( ) ? "adjust?" : nullptr;
}

// Highlight readings label themselves with the currently brightened node.
// The rendering itself lives in ReadingDisplay so measure mode, the voltage
// adjuster and the probe cursor draw the exact same way.
static void showNetReadingRaw( const char* name, const char* value,
                               const char* value2 = nullptr,
                               const char* hint = nullptr ) {
    ReadingDisplay::show( name, brightenedNode, value, value2, hint );
}

// "7447 LT" for the placed part pin sitting on `node`. Matched by NODE,
// never first-pin-on-net - a net carries many parts' pins at once and the
// wrong part's name on the panel is worse than none (Kevin's ruling,
// 2026-08-27). The part NAME is what truncates, never the pin suffix.
static bool partPinHeaderForNode( int node, char* out, size_t outLen ) {
    if ( node < 1 || node > 60 ) return false;
    for ( int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++ ) {
        const PartDefinition& p = globalState.parts.parts[ i ];
        if ( !p.placed ) continue;
        for ( int j = 0; j < p.numPins && j < MAX_PART_PINS; j++ ) {
            if ( partPinNode( p, p.pins[ j ] ) != node ) continue;
            const char* pn = p.pins[ j ].name;
            int pinLen = (int)strlen( pn );
            int nameBudget = (int)outLen - 1 - pinLen - 1;
            if ( nameBudget < 1 ) nameBudget = 1;
            snprintf( out, outLen, "%.*s %s", nameBudget, p.name, pn );
            return true;
        }
    }
    return false;
}

// The one funnel every net card passes through. When the brightened node is
// a placed part's pin, the card is re-headed with the part identity so the
// part display and the net display are ONE screen (Kevin, 08:38: a wired
// pin's stop showed "the net display we'd show if it wasn't in a part"):
// header "7447 LT" + row, first value row "GND 0.04 V" - source and value
// combined on one small line, the 13:52 "make it smaller" idiom - with the
// second value row and the hint riding through unchanged. Callers that
// compose their OWN part-pin header (showGpioPinReading) paint through
// showNetReadingRaw instead, so a name is never doubled.
static void showNetReading( const char* name, const char* value,
                            const char* value2 = nullptr,
                            const char* hint = nullptr ) {
    char pinName[ 16 ];
    if ( partPinHeaderForNode( brightenedNode, pinName, sizeof( pinName ) ) ) {
        char detail[ 28 ];
        bool haveName = ( name != nullptr && name[ 0 ] != '\0' );
        bool haveValue = ( value != nullptr && value[ 0 ] != '\0' );
        if ( haveName && haveValue )
            snprintf( detail, sizeof( detail ), "%s %s", name, value );
        else
            snprintf( detail, sizeof( detail ), "%s",
                      haveValue ? value : ( haveName ? name : "" ) );
        ReadingDisplay::show( pinName, brightenedNode, detail, value2, hint );
        return;
    }
    showNetReadingRaw( name, value, value2, hint );
}

// A GPIO on a part pin's net shows BOTH identities (Kevin's ask, 2026-08-27):
// "7400 4Y" as the headline with "GPIO 3 out" + HIGH/LOW on the value lines.
// netSemanticName can't name these nets - they are special-function, and its
// authority gate is right for GND/rails - so the part scan runs here without
// it: this is only ever called from inside a GPIO display branch.
static void showGpioPinReading( int netNum, int gpioIdx, bool isOutput,
                                const char* stateString ) {
    char gpioLabel[ 16 ];
    snprintf( gpioLabel, sizeof( gpioLabel ), "GPIO %d %s", gpioIdx + 1,
              isOutput ? "out" : "in" );
    char pinName[ 16 ] = "";
    // The FOCUSED pin first, matched by node: with two parts on one net -
    // exactly the 7447-driving-a-7seg board - the net walk below names
    // whichever part the table lists first, which may not be the pin under
    // the cursor. The net walk stays as the fallback for highlights that
    // arrive from the GPIO side (header pad, bare wire), where no node
    // matches but the net still deserves its part identity.
    if ( !partPinHeaderForNode( brightenedNode, pinName, sizeof( pinName ) ) &&
         netNum > 0 && netNum < MAX_NETS &&
         globalState.connections.nets[ netNum ].number == netNum ) {
        pinName[ 0 ] = '\0';
        for ( int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++ ) {
            const PartDefinition& p = globalState.parts.parts[ i ];
            if ( !p.placed ) continue;
            bool found = false;
            for ( int j = 0; j < p.numPins && j < MAX_PART_PINS; j++ ) {
                int node = partPinNode( p, p.pins[ j ] );
                if ( node < 1 || node > 60 ) continue;
                if ( !netHasNode( netNum, node ) ) continue;
                const char* pn = p.pins[ j ].name;
                int pinLen = (int)strlen( pn );
                int nameBudget = (int)sizeof( pinName ) - 1 - pinLen - 1;
                if ( nameBudget < 1 ) nameBudget = 1;
                snprintf( pinName, sizeof( pinName ), "%.*s %s", nameBudget,
                          p.name, pn );
                found = true;
                break;
            }
            if ( found ) break;
        }
    }
    // Raw paints on purpose: the header is already the part pin (or
    // deliberately not one), and the re-heading funnel would double it.
    if ( pinName[ 0 ] != '\0' ) {
        showNetReadingRaw( pinName, gpioLabel, stateString );
    } else {
        char name[ 16 ];
        if ( netSemanticName( netNum, gpioIdx, name, sizeof( name ) ) )
            showNetReadingRaw( name, stateString );   // the I2C "GPIO N SCL" form
        else
            showNetReadingRaw( gpioLabel, stateString );
    }
}

// The rail/DAC-on-a-part-pin display (Kevin, 12:53: "when we highlight the
// E pin, it's just showing the rail thing, we need the same thing as the
// gpio") is no longer a special case: the showNetReading funnel above
// re-heads EVERY net card - rails, DACs, GND, ADC, plain nets, I sense -
// with the node-matched part pin, so the rail/DAC branches below just call
// it like everyone else.

int Highlighting::highlightNets( int probeReading, int encoderNetHighlighted, int print ) {
    // Serial.print("justReadProbe = ");
    // Serial.println(probeReading);
    // delay(100);

    //  Serial.println("probeReading: ");
    //  Serial.println(probeReading);
    //  Serial.flush();

    int netHighlighted;

    if ( encoderNetHighlighted > 0 ) {

        netHighlighted = encoderNetHighlighted;

    } else {

        netHighlighted = brightenNet( probeReading );
        // A probe tap is a fresh gesture: the part card yields the panel to
        // the tapped net (otherwise the checkForReadingChanges part-focus
        // gate would mute the tap's live readings for the rest of the hold).
        // EXCEPT a tap on the focused part's own pins - the inspect listener
        // owns that display, and racing it here would drop the pin dots.
        if ( netHighlighted > 0 && partLabels.partHighlightActive( ) ) {
            bool onFocusedPart = false;
            int hp = partLabels.partHighlightPart( );
            if ( hp >= 0 && hp < globalState.parts.numParts ) {
                const PartDefinition& fp = globalState.parts.parts[ hp ];
                for ( int j = 0; j < fp.numPins && j < MAX_PART_PINS; j++ ) {
                    if ( partPinNode( fp, fp.pins[ j ] ) == probeReading ) {
                        onFocusedPart = true;
                        break;
                    }
                }
            }
            if ( !onFocusedPart ) partLabels.clearPartHighlight( );
        }
    }

    auto& sensePrintInitialized   = g_readingGuards.sensePrintInitialized;
    auto& lastSenseNetPrinted     = g_readingGuards.lastSenseNetPrinted;
    auto& lastSenseCurrentPrinted = g_readingGuards.lastSenseCurrentPrinted;
    auto& lastSenseVoltagePrinted = g_readingGuards.lastSenseVoltagePrinted;
    auto& lastSenseUpdatePrinted  = g_readingGuards.lastSenseUpdatePrinted;

    // if ( !currentSenseState.plusConnected || !currentSenseState.minusConnected ) {
    //     sensePrintInitialized = false;
    //     lastSenseNetPrinted = -1;
    // }

    if ( netHighlighted > 0 ) {

        highlightedOriginalColor = netColors[ netHighlighted ];
        highlightedNet = netHighlighted;

        // Start the highlight timer
        highlightTimer = millis( );

        // Serial.print("netHighlighted = ");
        // Serial.println(netHighlighted);
        if ( print == 1 ) {
            // No serial pre-clear here: ReadingDisplay clears the line as part
            // of every print, and blanking it for branches that print nothing
            // just threw away the reading still shown on the OLED.
            oled.setTextSize( 1 );
        }
        clearColorOverrides( 1, 1, 0 );
        brightenedRail = -1;
        if ( netHighlighted == 1 ) {
            // GND: rail flag only - the reading falls through to the default
            // branch below so GND prints its scanned V/mA (with the row
            // label) like any plain net. The old name-only "GND" case here
            // was the leftover highlighting path: it flashed a bare "GND"
            // for the ~140ms until checkForReadingChanges' first repaint.
            brightenedRail = 1;
        }
        // The clickwheel scrolls rows WITHIN one net, so the reading
        // branches below reprint on a row change too - the header's node
        // label reads brightenedNode at print time, and a net-only guard
        // left it stale while the brightening moved along the rows.
        auto& lastPrintedRowNode = g_readingGuards.lastPrintedRowNode;
        bool printedRowChanged = ( lastPrintedRowNode != brightenedNode );
        // Snapshot of the reprint gate the default branch's reading branches
        // all test, taken before any of them moves lastPrintedNet - the
        // trailing flush at the end of the switch is out of their scope and
        // has to be gated on the same thing they are.
        bool defaultBranchPrints = ( print == 1 &&
                                     ( lastPrintedNet != netHighlighted || printedRowChanged ) );
        // NOTE: lastPrintedNet is deliberately NOT cleared here anymore.
        // Clearing it every call re-pushed the OLED on every loop while the
        // probe tip was held on a net. Net changes reprint below; value
        // changes are the live updater's job (checkForReadingChanges);
        // clearHighlighting() resets it so a re-tap after timeout reprints.
        switch ( netHighlighted ) {
        case 0:
            break;
        case 2:
            if ( lastPrintedNet != netHighlighted || printedRowChanged ) {
                lastPrintedNet = netHighlighted;
                if ( print == 1 ) {
                    char value[ 28 ];
                    // Hardware truth, not the persisted value: a save=0 write
                    // (the guide's rail restore) moves the rail without
                    // touching globalState.power.
                    snprintf( value, sizeof( value ), "%0.2f V", getDacHardwareVoltage( 2 ) );
                    char curBuf[ 16 ];
                    showNetReading( "Top Rail", value, netCurrentValue( netHighlighted, curBuf, sizeof( curBuf ) ), adjustHintText( ) );
                }
            }
            brightenedRail = 0;
            break;
        case 3:
            if ( lastPrintedNet != netHighlighted || printedRowChanged ) {
                lastPrintedNet = netHighlighted;
                if ( print == 1 ) {
                    char value[ 28 ];
                    snprintf( value, sizeof( value ), "%0.2f V", getDacHardwareVoltage( 3 ) );
                    char curBuf[ 16 ];
                    showNetReading( "Bottom Rail", value, netCurrentValue( netHighlighted, curBuf, sizeof( curBuf ) ), adjustHintText( ) );
                }
            }
            brightenedRail = 2;
            break;
        case 4:
            if ( lastPrintedNet != netHighlighted || printedRowChanged ) {

                DACcolorOverride0 = -2;
                DACcolorOverride1 = 0x000000;
                if ( print == 1 ) {
                    char value[ 28 ];
                    snprintf( value, sizeof( value ), "%0.2f V", getDacVoltage( 0 ) );
                    char curBuf[ 16 ];
                    showNetReading( "DAC 0", value, netCurrentValue( netHighlighted, curBuf, sizeof( curBuf ) ), adjustHintText( ) );
                }
                lastPrintedNet = netHighlighted;
            }
            break;
        case 5:
            if ( lastPrintedNet != netHighlighted || printedRowChanged ) {

                DACcolorOverride0 = 0x000000;
                DACcolorOverride1 = -2;
                if ( print == 1 ) {
                    char value[ 28 ];
                    snprintf( value, sizeof( value ), "%0.2f V", getDacVoltage( 1 ) );
                    char curBuf[ 16 ];
                    showNetReading( "DAC 1", value, netCurrentValue( netHighlighted, curBuf, sizeof( curBuf ) ), adjustHintText( ) );
                }
                lastPrintedNet = netHighlighted;
            }
            break;
        default: {

            // Serial.print("  \t ");
            int specialPrint = 0;

            // Detect alternate functions on this net
            bool uartTxOnNet = false;
            bool uartRxOnNet = false;
            bool i2cOnNet = false;
            bool pwmOnNet = false;
            int functionOnNetIndex = -1; // any other function index 0..9
            if ( netHighlighted > 0 ) {
                // gpio indices 8 and 9 correspond to UART TX (pin 0) and RX (pin 1)
                if ( gpioNet[ 8 ] == netHighlighted && routableGpioFunction( 8 ) == GPIO_FUNC_UART ) {
                    uartTxOnNet = true;
                }
                if ( gpioNet[ 9 ] == netHighlighted && routableGpioFunction( 9 ) == GPIO_FUNC_UART ) {
                    uartRxOnNet = true;
                }
                // I2C can be on any pins configured as I2C and tied to this net
                for ( int i = 0; i < 10; i++ ) {
                    if ( gpioNet[ i ] == netHighlighted ) {
                        if ( gpio_get_function( gpioDef[ i ][ 0 ] ) == GPIO_FUNC_I2C ) {
                            i2cOnNet = true;
                            functionOnNetIndex = i;
                            break;
                        } else if ( gpio_get_function( gpioDef[ i ][ 0 ] ) == GPIO_FUNC_PWM ) {
                            pwmOnNet = true;
                            functionOnNetIndex = i;
                            break;
                        } else if ( gpio_get_function( gpioDef[ i ][ 0 ] ) != GPIO_FUNC_SIO ) {
                            functionOnNetIndex = i; // some other function
                        }
                    }
                }
            }

            int adc = anyAdcConnected( netHighlighted );
            int gpioInputNumber = anyGpioInputConnected( netHighlighted );
            int gpioOutputNumber = anyGpioOutputConnected( netHighlighted );

            // Serial.print("adc = ");
            // Serial.println(adc);
            // Serial.print("gpioInputNumber = ");
            // Serial.println(gpioInputNumber);
            // Serial.print("gpioOutputNumber = ");
            // Serial.println(gpioOutputNumber);

            bool currentSenseNetActive =
                currentSenseState.plusConnected && currentSenseState.minusConnected;
            bool isCurrentSenseNet =
                currentSenseNetActive &&
                (netHighlighted == currentSenseState.plusNet ||
                 netHighlighted == currentSenseState.minusNet);

            if ( isCurrentSenseNet ) {

                int showVoltage = 0;
                

                float current = currentSenseState.filteredCurrent_mA;
                float voltage = currentSenseState.busVoltage_V;
                unsigned long updated = currentSenseState.lastUpdatedMs;
                int direction = currentSenseState.currentDirection;

                bool shouldPrint = false;
                if ( !sensePrintInitialized || lastSenseNetPrinted != netHighlighted ||
                     printedRowChanged ) {
                    shouldPrint = true;
                } else if ( fabsf( current - lastSenseCurrentPrinted ) > 0.05f ||
                            fabsf( voltage - lastSenseVoltagePrinted ) > 0.02f ) {
                    shouldPrint = true;
                }

                if ( print == 1 && shouldPrint ) {
                    (void)direction;
                    char nameBuffer[ 16 ];
                    const char* baseName = netDisplayName( netHighlighted, nameBuffer, sizeof( nameBuffer ) );
                    char vBuf[ 16 ];
                    char value[ 16 ];
                    snprintf( vBuf, sizeof( vBuf ), "%.2f V", voltage );
                    snprintf( value, sizeof( value ), "%+.2f mA", current );
                    showNetReading( baseName, vBuf, value );

                    lastSenseCurrentPrinted = current;
                    lastSenseVoltagePrinted = voltage;
                    lastSenseUpdatePrinted = updated;
                    lastSenseNetPrinted = netHighlighted;
                    sensePrintInitialized = true;
                    lastPrintedNet = netHighlighted;
                } else if ( print == 1 ) {
                    lastPrintedNet = netHighlighted;
                }

                specialPrint = 1;

             } else 
            if ( uartTxOnNet || uartRxOnNet ) {

                    // Keep small persistent buffers so we can append only new bytes
                auto& prevTx    = g_readingGuards.prevTx;
                auto& prevTxLen = g_readingGuards.prevTxLen;
                auto& prevRx    = g_readingGuards.prevRx;
                auto& prevRxLen = g_readingGuards.prevRxLen;

                // Snapshot buffers (small)
                char txSnapshot[64];
                char rxSnapshot[64];

                size_t txLen = AsyncPassthrough::getLastUsbToUartSnapshot(txSnapshot, sizeof(txSnapshot));
                size_t rxLen = AsyncPassthrough::getLastUartRxSnapshot(rxSnapshot, sizeof(rxSnapshot));
                // Serial.print("txLen = ");
                // Serial.println(txLen);
                // Serial.print("rxLen = ");
                // Serial.println(rxLen);
                // Serial.print("txSnapshot = ");
                // Serial.println(txSnapshot);
                // Serial.print("rxSnapshot = ");
                // Serial.println(rxSnapshot);
                // Serial.flush();

                // If there's no data at all, fall back to the previous behavior (show UART config)
                bool haveLiveData = (txLen > 0) || (rxLen > 0);

                if ( lastPrintedNet != netHighlighted ) {
                    // first time highlight -> reset previous-tracking
                    prevTxLen = prevRxLen = 0;
                    prevTx[0] = '\0';
                    prevRx[0] = '\0';

                    if ( print == 1 ) {
                        // Encoder-driven highlights show only static UART
                        // info. Callers signal "probe/node" with <= 0
                        // (Probing's connect cursor passes 0; the old
                        // `!= -1` test misclassified it as encoder).
                        bool calledFromEncoder = ( encoderNetHighlighted > 0 );

                        if ( calledFromEncoder || !haveLiveData ) {
                            // No live data OR encoder-driven highlight -> the
                            // static config reads like any other net's display.
                            char cfg[ 24 ];
                            showNetReading( uartDirectionName( uartTxOnNet, uartRxOnNet ),
                                            uartConfigText( cfg, sizeof( cfg ) ) );
                        } else {
                            // Live data available — header only; the bytes are
                            // appended by checkForReadingChanges().
                            uartRefreshLiveView( uartTxOnNet, uartRxOnNet, txSnapshot, 0,
                                                 rxSnapshot, 0 );
                        }
                    }
                    lastPrintedNet = netHighlighted;
                } else {
                    // already highlighted -> if live data, append only the new bytes
                    if ( print == 1 && haveLiveData ) {
                        // TX: append any new suffix
                        if ( txLen > prevTxLen ) {
                            // if the previous content matches the start of the new snapshot, append the remainder
                            if ( prevTxLen == 0 || memcmp(prevTx, txSnapshot, prevTxLen) == 0 ) {
                                OLEDOut.print(&txSnapshot[prevTxLen]);
                            } else {
                                // content diverged or wrapped — full refresh
                                uartRefreshLiveView( uartTxOnNet, uartRxOnNet, txSnapshot,
                                                     txLen, rxSnapshot, rxLen );
                            }
                        } else if ( txLen < prevTxLen ) {
                            // wrapped/shortened -> full refresh
                            uartRefreshLiveView( uartTxOnNet, uartRxOnNet, txSnapshot, txLen,
                                                 rxSnapshot, rxLen );
                        }

                        // RX: same logic
                        if ( rxLen > prevRxLen ) {
                            if ( prevRxLen == 0 || memcmp(prevRx, rxSnapshot, prevRxLen) == 0 ) {
                                OLEDOut.print(&rxSnapshot[prevRxLen]);
                            } else {
                                uartRefreshLiveView( uartTxOnNet, uartRxOnNet, txSnapshot,
                                                     txLen, rxSnapshot, rxLen );
                            }
                        } else if ( rxLen < prevRxLen ) {
                            uartRefreshLiveView( uartTxOnNet, uartRxOnNet, txSnapshot, txLen,
                                                 rxSnapshot, rxLen );
                        }

                        // Update prev buffers (store full snapshot)
                        prevTxLen = txLen > sizeof(prevTx)-1 ? sizeof(prevTx)-1 : txLen;
                        if ( prevTxLen ) { memcpy(prevTx, txSnapshot, prevTxLen); prevTx[prevTxLen] = '\0'; }
                        else prevTx[0] = '\0';

                        prevRxLen = rxLen > sizeof(prevRx)-1 ? sizeof(prevRx)-1 : rxLen;
                        if ( prevRxLen ) { memcpy(prevRx, rxSnapshot, prevRxLen); prevRx[prevRxLen] = '\0'; }
                        else prevRx[0] = '\0';
                    }
                }
                specialPrint = 1;

            } else if ( i2cOnNet ) {

                if ( lastPrintedNet != netHighlighted || printedRowChanged ) {
                    if ( print == 1 ) {
                        // Simple I2C label without scanning
                        const char* line = "I2C";
                        if ( functionOnNetIndex >= 0 ) {
                            int pin = gpioDef[ functionOnNetIndex ][ 0 ];
                            if ( pin == jumperlessConfig.top_oled.sda_pin ) {
                                line = "I2C  SDA";
                            } else if ( pin == jumperlessConfig.top_oled.scl_pin ) {
                                line = "I2C  SCL";
                            }
                        }
                        char value[ 16 ];
                        snprintf( value, sizeof( value ), "%d KHz", i2cSpeed / 1000 );
                        showNetReading( line, value );
                    }
                    lastPrintedNet = netHighlighted;
                }
                specialPrint = 1;

            } else if ( pwmOnNet ) {

                if ( lastPrintedNet != netHighlighted || printedRowChanged ) {
                    if ( print == 1 ) {
                        float freq = gpioPWMFrequency[ functionOnNetIndex ];
                        float duty = gpioPWMDutyCycle[ functionOnNetIndex ] * 100.0f;
                        char value[ 24 ];
                        snprintf( value, sizeof( value ), "%0.0f Hz  %0.0f%%", freq, duty );
                        showNetReading( "PWM", value );
                    }
                    lastPrintedNet = netHighlighted;
                }
                specialPrint = 1;

            } else if ( functionOnNetIndex != -1 ) {

                if ( lastPrintedNet != netHighlighted || printedRowChanged ) {
                    if ( print == 1 ) {
                        // Pin-aware function name lookup
                        gpio_function_t fun = gpio_get_function( gpioDef[ functionOnNetIndex ][ 0 ] );
                        uint gpioPin = gpioDef[ functionOnNetIndex ][ 0 ];

                        int TxRx = 0;
                        if ( fun == GPIO_FUNC_UART ) {
                            if ( gpioPin == 0 ) {
                                TxRx = 1; // UART Tx
                            } else if ( gpioPin == 1 ) {
                                TxRx = 2; // UART Rx
                            }
                        }
                        // Serial.print(AsyncPassthrough::uartReceived[AsyncPassthrough::uartReceivedHead]);
                        // gpio_function_t fun = routableGpioFunction( functionOnNetIndex );
                        // Serial.print( "Function: " );
                        // Serial.print( fun );
                        // Serial.print( " on GPIO " );
                        // Serial.print( gpioDef[ functionOnNetIndex ][ 0 ] );
                        // Serial.print( "   " );
                        // Serial.print( "Function name: " );
                        // Serial.print( gpio_function_name_for_pin( gpioDef[ functionOnNetIndex ][ 0 ], fun ) );
                        // Serial.print("\t" );
                        // Serial.print("functionOnNetIndex: ");
                        // Serial.print(functionOnNetIndex);
                        // Serial.flush( );


                        
                        const char* fname = gpio_function_name_for_pin( gpioPin, fun );
                        showNetReading( fname, "" );
                    }
                    lastPrintedNet = netHighlighted;
                }
                specialPrint = 1;

            } else if ( ( adc != -1 || gpioInputNumber != -1 || gpioOutputNumber != -1 ) ) {

                if ( lastPrintedNet != netHighlighted || printedRowChanged ) {

                    if ( adc != -1 ) {
                        ADCcolorOverride0 = -2;
                        ADCcolorOverride1 = -2;
                        logoOverrideMap[ 0 ].colorOverride = logoOverrideMap[ 0 ].defaultOverride;
                        logoOverrideMap[ 1 ].colorOverride = logoOverrideMap[ 1 ].defaultOverride;
                        logoOverriden = true;
                        if ( print == 1 ) {
                            float adcV = readAdcVoltage( adc, 32 ); // one read, not two
                            char name[ 12 ];
                            char value[ 28 ];
                            snprintf( name, sizeof( name ), "ADC %d", adc );
                            snprintf( value, sizeof( value ), "%0.2f V", adcV );
                            char curBuf[ 16 ];
                            showNetReading( name, value, netCurrentValue( netHighlighted, curBuf, sizeof( curBuf ) ) );
                        }
                        specialPrint = 1;
                    }

                    if ( gpioInputNumber != -1 ) {
                        GPIOcolorOverride0 = -2;
                        GPIOcolorOverride1 = -2;
                        logoOverrideMap[ 4 ].colorOverride = logoOverrideMap[ 4 ].defaultOverride;
                        logoOverrideMap[ 5 ].colorOverride = logoOverrideMap[ 5 ].defaultOverride;
                        logoOverriden = true;
                        if ( print == 1 ) {
                            // Same source the ~40ms live updater reads
                            // (gpioReading[], which encodes 2 = floating) -
                            // the one-shot used gpioReadWithFloating() and
                            // the two could disagree on first show.
                            int gpioInputState = gpioReading[ gpioInputNumber ];
                            const char* stateString =
                                ( gpioInputState == 0 )   ? "LOW"
                                : ( gpioInputState == 1 ) ? "HIGH"
                                : ( gpioInputState == 2 ) ? "FLOATING"
                                                          : "?";
                            showGpioPinReading( netHighlighted, gpioInputNumber,
                                                false, stateString );
                        }
                        specialPrint = 1;
                    }

                    if ( gpioOutputNumber != -1 ) {
                        GPIOcolorOverride0 = -2;
                        GPIOcolorOverride1 = -2;
                        logoOverrideMap[ 4 ].colorOverride = logoOverrideMap[ 4 ].defaultOverride;
                        logoOverrideMap[ 5 ].colorOverride = logoOverrideMap[ 5 ].defaultOverride;
                        logoOverriden = true;
                        if ( print == 1 ) {
                            int gpioOutputState = gpio_get_out_level( gpioDef[ gpioOutputNumber ][ 0 ] );
                            showGpioPinReading( netHighlighted, gpioOutputNumber,
                                                true, gpioOutputState ? "HIGH" : "LOW" );
                        }
                        specialPrint = 1;
                    }
                }
            } else {
                if ( netHighlighted > 0 ) {
                    // Reprint on net OR row change (the encoder scrolls
                    // rows within one net), NOT on every pass: this branch
                    // was the only one without a lastPrintedNet guard, so a
                    // held probe tip re-pushed "Net N / row X" every service
                    // loop and stomped the live updater's voltage/current
                    // line whenever the scan current happened to read 0.
                    if ( print == 1 && ( lastPrintedNet != netHighlighted ||
                                         printedRowChanged ) ) {
                        char nameBuffer[ 16 ];
                        char nodePin[ 16 ];
                        const char* baseName;
                        if ( partPinHeaderForNode( brightenedNode, nodePin, sizeof( nodePin ) ) )
                            // showNetReading re-heads with THIS pin - and
                            // netSemanticName's first-pin-on-net walk could
                            // name a DIFFERENT part sharing the net, putting
                            // "7SEG52 S4 0.31 V" under a "7447 D" header
                            baseName = netDisplayName( netHighlighted, nameBuffer, sizeof( nameBuffer ) );
                        else if ( netSemanticName( netHighlighted, -1, nameBuffer, sizeof( nameBuffer ) ) )
                            baseName = nameBuffer;   // a placed part's pin label
                        else
                            baseName = netDisplayName( netHighlighted, nameBuffer, sizeof( nameBuffer ) );

                        // Row shows in the sink's header (right side); the
                        // value lines are the SCANNED node voltage (plain
                        // nets have one too - the background scan measured
                        // it) and the net's estimated current.
                        char vBuf[ 16 ];
                        char curBuf[ 16 ];
                        showNetReading( baseName,
                                        netScanVoltageValue( netHighlighted, vBuf, sizeof( vBuf ) ),
                                        netCurrentValue( netHighlighted, curBuf, sizeof( curBuf ) ) );
                    }
                }
                if ( specialPrint == 0 ) {
                    // Serial.println();
                }
            }
            lastPrintedNet = netHighlighted;
        }
            if ( defaultBranchPrints ) {
                Serial.flush( );
            }
        }
        // One row-label latch for every reading branch above. The UART live
        // stream view deliberately ignores row changes (it has no node
        // label), so a scroll within a UART net just skips the reprint.
        if ( print == 1 ) {
            lastPrintedRowNode = brightenedNode;
        }
    }
    // requestLedShow( 1 );

    // Serial.println("netHighlighted: ");
    // Serial.println(netHighlighted);
    // Serial.flush();

    return netHighlighted;
}

int Highlighting::checkForReadingChanges( void ) {
    // Previous-value latches (see LiveReadingGuards above)
    auto& prevAdcReading      = g_readingGuards.prevAdcReading;
    auto& prevGpioInputState  = g_readingGuards.prevGpioInputState;
    auto& prevGpioOutputState = g_readingGuards.prevGpioOutputState;
    auto& prevDacVoltage      = g_readingGuards.prevDacVoltage;
    auto& prevRailVoltage     = g_readingGuards.prevRailVoltage;
    auto& lastMeasuredNet     = g_readingGuards.lastMeasuredNet;
    auto& lastUpdateTime      = g_readingGuards.lastUpdateTime;

    // UART live-display state (small persistent buffers, display-only)
    auto& prevUartTx    = g_readingGuards.prevUartTx;
    auto& prevUartTxLen = g_readingGuards.prevUartTxLen;
    auto& prevUartRx    = g_readingGuards.prevUartRx;
    auto& prevUartRxLen = g_readingGuards.prevUartRxLen;

    // Don't update too frequently
    unsigned long currentTime = millis( );
    // NOTE: removed the 50ms early-exit so checkForReadingChanges() evaluates
    // the UART snapshot and other sensors every time it's called by the service loop.

    // While a PART has focus AND the card owns the panel (highlightedNet
    // parked), the live net reading must not repaint "Net N" over it
    // (Kevin's bench, 2026-08-27: the card lasted seconds before a net
    // reading stomped it). A part-pin focus that deliberately handed the
    // panel to a special net - a GPIO pin wants its live HIGH/LOW and the
    // click-to-toggle feedback - sets highlightedNet and passes through.
    if ( partLabels.partHighlightActive( ) && highlightedNet <= 0 ) {
        showReadingNet = -1;
        lastMeasuredNet = -1;
        return -1;
    }

    // Update showReadingNet to track brightenedNet (but don't clear on timeout)
    if ( brightenedNet > 0 ) {
        showReadingNet = brightenedNet;
    }

    // Check if there's a net to show readings for
    if ( showReadingNet <= 0 ) {
        lastMeasuredNet = -1;
        return -1;
    }

    // Estimated current shown alongside voltages (ADC/DAC/rail branches
    // below); tracked so a current change alone also refreshes the display.
    auto& prevShownCurrent = g_readingGuards.prevShownCurrent;

    // Reset stored values if we switched to a different net
    if ( lastMeasuredNet != showReadingNet ) {
        prevAdcReading = kNoReading;
        prevGpioInputState = -1;
        prevGpioOutputState = -1;
        prevDacVoltage = kNoReading;
        prevRailVoltage = kNoReading;
        prevShownCurrent = kNoReading;

        // Reset UART live-display state when net changes
        prevUartTxLen = 0; prevUartTx[0] = '\0';
        prevUartRxLen = 0; prevUartRx[0] = '\0';

        lastMeasuredNet = showReadingNet;
        lastUpdateTime = currentTime;
        return -1;
    }

    bool displayUpdated = false;
    char valueString[ 30 ];

    // Check for ADC measurements
    int adcChannel = anyAdcConnected( showReadingNet );
    if ( adcChannel != -1 ) {
        float currentAdcReading = readAdcVoltage( adcChannel, 64 );
        float estCurrent = netCurrent_mA( showReadingNet );

        if ( fabs( currentAdcReading - prevAdcReading ) > 0.009 ||
             fabs( estCurrent - prevShownCurrent ) > 0.1f ) {
            prevAdcReading = currentAdcReading;
            prevShownCurrent = estCurrent;

            char name[ 12 ];
            snprintf( name, sizeof( name ), "ADC %d", adcChannel );
            snprintf( valueString, sizeof( valueString ), "%0.2f V", currentAdcReading );
            char curBuf[ 16 ];
            showNetReading( name, valueString, netCurrentValue( showReadingNet, curBuf, sizeof( curBuf ) ) );

            displayUpdated = true;
        }
        showReadingRow = showReadingNet;
    }

    // Check for GPIO input
    int gpioInputNumber = anyGpioInputConnected( showReadingNet );
    if ( gpioInputNumber != -1 ) {
        int currentGpioInputState = gpioReading[ gpioInputNumber ];

        // Update if state changed
        if ( currentGpioInputState != prevGpioInputState ) {
            prevGpioInputState = currentGpioInputState;

            const char* stateString =
                ( currentGpioInputState == 0 )   ? "LOW"
                : ( currentGpioInputState == 1 ) ? "HIGH"
                : ( currentGpioInputState == 2 ) ? "FLOATING"
                                                 : "?";
            showGpioPinReading( showReadingNet, gpioInputNumber, false, stateString );

            displayUpdated = true;
        }
        showReadingRow = showReadingNet;
    }

    // Check for GPIO output
    int gpioOutputNumber = anyGpioOutputConnected( showReadingNet );
    if ( gpioOutputNumber != -1 ) {
        int currentGpioOutputState = gpio_get_out_level( gpioDef[ gpioOutputNumber ][ 0 ] );

        // Update if state changed
        if ( currentGpioOutputState != prevGpioOutputState ) {
            prevGpioOutputState = currentGpioOutputState;

            showGpioPinReading( showReadingNet, gpioOutputNumber, true,
                                currentGpioOutputState ? "HIGH" : "LOW" );

            displayUpdated = true;
        }
        showReadingRow = showReadingNet;
    }

    // --- UART live display: update OLED while this net stays highlighted ---
    // Top level on purpose: this used to be nested inside the GPIO-output
    // branch, so a pure UART net (whose pins are FUNC_UART, not SIO outputs)
    // never got its live Tx/Rx stream here.
    bool uartTxOnNet = false;
    bool uartRxOnNet = false;
    {
        if ( showReadingNet > 0 ) {
            if ( gpioNet[ 8 ] == showReadingNet && routableGpioFunction( 8 ) == GPIO_FUNC_UART ) uartTxOnNet = true;
            if ( gpioNet[ 9 ] == showReadingNet && routableGpioFunction( 9 ) == GPIO_FUNC_UART ) uartRxOnNet = true;
        }

        if ( uartTxOnNet || uartRxOnNet ) {
            // Ensure a static header exists for this highlighted net (printed once)
            if ( lastPrintedNet != showReadingNet ) {
                // Stays on the plain renderer: the scrolling byte view below
                // writes into the small-text buffer right after this.
                char cfg[ 24 ];
                char oledHeader[ 48 ];
                snprintf( oledHeader, sizeof( oledHeader ), "%s\n%s",
                          uartDirectionName( uartTxOnNet, uartRxOnNet ),
                          uartConfigText( cfg, sizeof( cfg ) ) );
                oled.clear();
                oled.clearPrintShow(oledHeader, 2, true, true, true);
                lastPrintedNet = showReadingNet;
            }

            // Always poll snapshots and append any newly-arrived bytes, then consume them
            char txSnapshot[64];
            char rxSnapshot[64];
            size_t txLen = AsyncPassthrough::getLastUsbToUartSnapshot(txSnapshot, sizeof(txSnapshot));
            size_t rxLen = AsyncPassthrough::getLastUartRxSnapshot(rxSnapshot, sizeof(rxSnapshot));

            if ( txLen > 0 ) {
                OLEDOut.print("Tx: ");
                OLEDOut.print(txSnapshot);
                AsyncPassthrough::clearLastUsbToUartSnapshot();
                displayUpdated = true;
            }
            if ( rxLen > 0 ) {
                OLEDOut.print("Rx: ");
                OLEDOut.print(rxSnapshot);
                AsyncPassthrough::clearLastUartRxSnapshot();
                displayUpdated = true;
            }

            showReadingRow = showReadingNet;
        }
    }

    // Check for DAC connections (nets 4 and 5)
    if ( showReadingNet == 4 || showReadingNet == 5 ) {
        int dacNum = showReadingNet - 4;
        float currentDacVoltage = getDacVoltage( dacNum );
        float estCurrent = netCurrent_mA( showReadingNet );

        // Check if change is significant (>0.05V dead zone / >0.1mA)
        if ( fabs( currentDacVoltage - prevDacVoltage ) > 0.05 ||
             fabs( estCurrent - prevShownCurrent ) > 0.1f ) {
            prevDacVoltage = currentDacVoltage;
            prevShownCurrent = estCurrent;

            char name[ 12 ];
            snprintf( name, sizeof( name ), "DAC %d", dacNum );
            snprintf( valueString, sizeof( valueString ), "%0.2f V", currentDacVoltage );
            char curBuf[ 16 ];
            showNetReading( name, valueString, netCurrentValue( showReadingNet, curBuf, sizeof( curBuf ) ), adjustHintText( ) );

            displayUpdated = true;
        }
        showReadingRow = showReadingNet;
    }

    // Check for rail connections (nets 2, 3)
    if ( showReadingNet == 2 || showReadingNet == 3 ) {
        bool top = ( showReadingNet == 2 );
        // Hardware truth (see the rail readout above): a save=0 rail write
        // moves the pin without touching globalState.power.
        float currentRailVoltage = getDacHardwareVoltage( top ? 2 : 3 );
        float estCurrent = netCurrent_mA( showReadingNet );

        // Check if change is significant (>0.05V dead zone / >0.1mA)
        if ( fabs( currentRailVoltage - prevRailVoltage ) > 0.05 ||
             fabs( estCurrent - prevShownCurrent ) > 0.1f ) {
            prevRailVoltage = currentRailVoltage;
            prevShownCurrent = estCurrent;

            snprintf( valueString, sizeof( valueString ), "%0.2f V", currentRailVoltage );
            char curBuf[ 16 ];
            showNetReading( top ? "Top Rail" : "Bottom Rail", valueString, netCurrentValue( showReadingNet, curBuf, sizeof( curBuf ) ), adjustHintText( ) );

            displayUpdated = true;
        }
        showReadingRow = showReadingNet;
    }

    // Check for current sense nets
    if ( showReadingNet > 0 && currentSenseState.plusConnected && currentSenseState.minusConnected ) {
        if ( showReadingNet == currentSenseState.plusNet || showReadingNet == currentSenseState.minusNet ) {
            float current = currentSenseState.filteredCurrent_mA;
            float voltage = currentSenseState.busVoltage_V;

            // Serial.print("current: ");
            // Serial.print(current);
            // Serial.print(" mA");
            // Serial.flush();

            // Serial.print("voltage: ");
            // Serial.print(voltage);
            // Serial.print(" V");
            // Serial.flush();
            
            // Always update current sense display (no dead zone, it changes frequently)
            auto& lastCurrentPrinted = g_readingGuards.lastCurrentPrinted;
            auto& lastVoltagePrinted = g_readingGuards.lastVoltagePrinted;
            
            // Update if current changed by more than 0.05mA or voltage by more than 0.02V
            if (( fabs( current - lastCurrentPrinted ) > 0.05 || fabs( voltage - lastVoltagePrinted ) > 0.1)  && millis() - lastUpdateTime > 15) {
                lastCurrentPrinted = current;
                lastVoltagePrinted = voltage;

                char nameBuffer[ 16 ];
                const char* baseName = netDisplayName( showReadingNet, nameBuffer, sizeof( nameBuffer ) );
                char vBuf[ 16 ];
                snprintf( vBuf, sizeof( vBuf ), "%.2f V", voltage );
                snprintf( valueString, sizeof( valueString ), "%+.2f mA", current );
                showNetReading( baseName, vBuf, valueString );

                displayUpdated = true;
            }
            showReadingRow = showReadingNet;
        }
    }

    // Estimated scan readings, for nets with NO display type of their own.
    // ADC/DAC/rail nets show "V + mA" in their branches above; GPIO and
    // UART nets own their state/stream displays. Without these exclusions
    // this generic fallback fired between their refreshes and stomped them
    // with the plain net view (seen live: "GPIO 3 out / high" replaced by
    // "Net 15 / 3.27 V / 0.0 mA" half a second after highlighting).
    bool hasOwnDisplay =
        ( adcChannel != -1 ) ||
        ( gpioInputNumber != -1 ) ||
        ( gpioOutputNumber != -1 ) ||
        uartTxOnNet || uartRxOnNet ||
        ( showReadingNet >= 2 && showReadingNet <= 5 ); // rails + DACs
    if ( !displayUpdated && !hasOwnDisplay &&
         showReadingNet > 0 && showReadingNet < MAX_NETS ) {
        bool isIsenseNet =
            currentSenseState.plusConnected && currentSenseState.minusConnected &&
            ( showReadingNet == currentSenseState.plusNet ||
              showReadingNet == currentSenseState.minusNet );
        if ( !isIsenseNet ) {
            float estCurrent_mA = netCurrent_mA( showReadingNet );
            char vBuf[ 16 ];
            const char* scanV = netScanVoltageValue( showReadingNet, vBuf, sizeof( vBuf ) );
            auto& lastEstCurrentPrinted = g_readingGuards.lastEstCurrentPrinted;
            auto& lastEstNetPrinted     = g_readingGuards.lastEstNetPrinted;
            auto& lastScanVPrinted      = g_readingGuards.lastScanVPrinted;
            bool changed = ( lastEstNetPrinted != showReadingNet ) ||
                           ( fabs( estCurrent_mA - lastEstCurrentPrinted ) > 0.1f ) ||
                           ( strcmp( scanV ? scanV : "", lastScanVPrinted ) != 0 );
            if ( changed && millis( ) - lastUpdateTime > 100 ) {
                lastEstCurrentPrinted = estCurrent_mA;
                lastEstNetPrinted = showReadingNet;
                snprintf( lastScanVPrinted, sizeof( lastScanVPrinted ), "%s",
                          scanV ? scanV : "" );

                char nameBuffer[ 16 ];
                const char* baseName = netDisplayName( showReadingNet, nameBuffer, sizeof( nameBuffer ) );
                snprintf( valueString, sizeof( valueString ), "%.1f mA", estCurrent_mA );
                showNetReading( baseName, scanV, valueString );

                displayUpdated = true;
                showReadingRow = showReadingNet;
            }
        }
    }

    if ( displayUpdated ) {
        lastUpdateTime = currentTime;
        return 1; // Indicates display was updated
    }

    return -1; // No updates
}

// ============================================================================
// Persistent Node Highlighting and Button Actions
// ============================================================================

/**
 * @brief Static callback for rail voltage updates
 */
static void railVoltageCallback(float value, bool isLive, void* context) {
    int rail = (int)(intptr_t)context;  // 0=both, 1=top, 2=bottom
    
    if (isLive) {
        // Live update - set the hardware AND update globalState
        switch (rail) {
            case 0: // Both rails
                setTopRail(value, 1, 0);  // save=1, saveEEPROM=0
                setBotRail(value, 1, 0);
                globalState.power.topRail = value;
                globalState.power.bottomRail = value;
                break;
            case 1: // Top rail
                setTopRail(value, 1, 0);
                globalState.power.topRail = value;
                break;
            case 2: // Bottom rail
                setBotRail(value, 1, 0);
                globalState.power.bottomRail = value;
                break;
        }
    } else {
        // Preview only - update globalState for LED display but DON'T set hardware
        // The LEDs read from globalState.power, so updating it will show preview
        switch (rail) {
            case 0: // Both rails
                globalState.power.topRail = value;
                globalState.power.bottomRail = value;
                break;
            case 1: // Top rail
                globalState.power.topRail = value;
                break;
            case 2: // Bottom rail
                globalState.power.bottomRail = value;
                break;
        }
        // Force LED update to show preview
        lightUpRail(-1, -1, 1, LEDbrightnessRail);
    }
}

/**
 * @brief Static callback for DAC voltage updates
 */
static void dacVoltageCallback(float value, bool isLive, void* context) {
    int dac = (int)(intptr_t)context;  // 0=DAC0, 1=DAC1
    
    if (isLive) {
        // Live update - set the hardware AND update globalState.
        // checkProbePower=true: this is a USER write, exactly like MicroPython
        // dac_set() - if it parks the probe feed's DAC outside the feed's
        // window the feed relocates NOW (claim latch + nudge), and a turn back
        // inside releases it. With false the DAC moved but the feed sat on it
        // until the next unrelated rebuild.
        if (dac == 0) {
            setDac0voltage(value, 1, 0, true);  // save=1, saveEEPROM=0
            globalState.power.dac0 = value;
        } else {
            setDac1voltage(value, 1, 0, true);
            globalState.power.dac1 = value;
        }
    } else {
        // Preview only - update globalState for LED display but DON'T set
        // hardware. The feed's viability reads this state, so a preview
        // outside the window WOULD relocate the feed if a rebuild ran now -
        // none does: the adjuster loop only services CRITICAL work, and both
        // exits below (cancel restores, confirm writes) leave state and
        // hardware agreeing again before any rebuild can look. (The only
        // residual vector is a MicroPython script rebuilding mid-adjust.)
        if (dac == 0) {
            globalState.power.dac0 = value;
        } else {
            globalState.power.dac1 = value;
        }
        // Force LED update to show preview (DACs are nets 4 and 5)
        lightUpNet(dac == 0 ? 4 : 5, -1, 1, LEDbrightnessSpecial, 0);
    }
}

/**
 * @brief Check if a node should persist in highlighting (not timeout)
 * 
 * Persistent nodes include:
 * - Rails (GND, TOP_RAIL, BOTTOM_RAIL)
 * - DACs (DAC0, DAC1)
 * - GPIO outputs
 * - Current sense nets (when active)
 * 
 * @param node The node to check
 * @return true if node should persist, false otherwise
 */
bool Highlighting::shouldPersistHighlight(int node) {
    // Rails and DACs persist while the click-to-adjust shortcut is armed:
    // the long persistent timeout is what gives the user time to read the
    // "adjust?" prompt and click. Gated so boards without the shortcut
    // keep the short highlight timeout they always had.
    // NET-based on purpose (2 = Top Rail, 3 = Bottom Rail, 4/5 = DACs -
    // MatrixState's fixed special nets): brightenedNode is whatever node
    // the probe last touched and clears to -1 on lift, so keying rails on
    // node == TOP_RAIL made the click register only when the tap happened
    // to land on the rail pad itself - Kevin's "works half the time".
    if (clickAdjustEnabled()) {
        if (highlightedNet >= 2 && highlightedNet <= 5) {
            return true;
        }
    }

    // GPIO outputs persist
    if (highlightedNet > 0 && anyGpioOutputConnected(highlightedNet) != -1) {
        return true;
    }
    
    // Current sense nets persist when active
    // if (highlightedNet > 0 && currentSenseState.plusConnected && currentSenseState.minusConnected) {
    //     if (highlightedNet == currentSenseState.plusNet || highlightedNet == currentSenseState.minusNet) {
    //         return true;
    //     }
    // }
    
    return false;
}

/**
 * @brief Check if Highlighting wants to handle the current button press
 * 
 * This is called by higher-priority services (like Menus) to check if they
 * should defer button handling to the Highlighting system for voltage adjustment.
 * 
 * @return true if button press should be handled by voltage adjustment, false otherwise
 */
bool Highlighting::wantsToHandleButtonPress(void) {
    // Only want to handle if a net is highlighted
    if (highlightedNet <= 0) {
        return false;
    }
    
    // Only handle persistent nodes that can be adjusted
    if (!shouldPersistHighlight(brightenedNode)) {
        return false;
    }
    
    // GND is persistent but not adjustable
    if (brightenedNode == GND) {
        return false;
    }
    
    // Rails and DACs are adjustable - by NET (see shouldPersistHighlight's
    // note; GND is net 1 and never matches). shouldPersistHighlight() above
    // already requires clickAdjustEnabled() for these, but the explicit
    // gate keeps this readable on its own.
    if (clickAdjustEnabled() &&
        highlightedNet >= 2 && highlightedNet <= 5) {
        return true;
    }
    
    // GPIO outputs - for now, don't handle (let menu open)
    // Future: could return true here if we want to toggle GPIO on button press
    
    return false;
}

/**
 * @brief Handle encoder button press when a persistent node is highlighted
 * 
 * Launches appropriate adjustment UI for:
 * - Rails -> voltage adjustment
 * - DACs -> voltage adjustment
 * - GPIO -> (future: toggle output)
 * 
 * @return 1 if button press was handled, 0 if not
 */
int Highlighting::handleEncoderButtonPress(void) {
    // Only handle if something is highlighted (check net, not node which can be 0 or negative)
    if (highlightedNet <= 0) {
        return 0;
    }
    
    // Check if this is a persistent node
    if (!shouldPersistHighlight(brightenedNode)) {
        return 0;
    }
    
    // Handle rails - by NET, matching wantsToHandleButtonPress (net 1 =
    // GND is not adjustable and never claimed; brightenedNode is too
    // volatile to key on - see shouldPersistHighlight's note).
    if (highlightedNet == 2) {
        adjustRailVoltage(1);
        return 1;
    }

    if (highlightedNet == 3) {
        adjustRailVoltage(2);
        return 1;
    }

    // Handle DACs
    if (highlightedNet == 4) {
        adjustDACVoltage(0);
        return 1;
    }
    
    if (highlightedNet == 5) {
        adjustDACVoltage(1);
        return 1;
    }
    
    // GPIO outputs - for now, just indicate we handled it
    // Future: could toggle output here
    if (anyGpioOutputConnected(highlightedNet) != -1) {
        // For now, don't do anything but consume the button press
        // This prevents menu from opening when clicking on GPIO outputs
        return 0;  // Return 0 so menu can still open for GPIO
    }
    
    return 0;
}

/**
 * @brief Launch voltage adjustment UI for a rail
 * 
 * @param rail Which rail to adjust (0=both, 1=top, 2=bottom)
 */
void Highlighting::adjustRailVoltage(int rail) {
    // Save original values for cancellation
    float origTopRail = globalState.power.topRail;
    float origBottomRail = globalState.power.bottomRail;
    
    // Prepare configuration
    VoltageAdjustConfig config;
    config.minVoltage = -8.0;
    config.maxVoltage = 8.0;
    config.enableSnap = false;
    config.liveUpdateInRange = true;
    config.liveUpdateMin = 0.0;
    config.liveUpdateMax = 5.0;
    
    // Set initial value and label based on which rail.
    //
    // RULING (wave 2, task 7 §8.4 / concern 4): seed from HARDWARE TRUTH
    // (getDacHardwareVoltage(2|3) -> railHwVolts[]), not globalState.power.
    // The two diverge whenever a guide quit before its power_on restored the
    // user's rails with save=0 - the rails physically carry the restored
    // voltage while the saved state still reads 0 V. Seeding from the saved
    // state made the adjuster open at 0 V and, because liveUpdateInRange
    // writes as you turn, the FIRST detent yanked a live 3.3 V rail down to
    // near zero. Opening at what the rail is actually doing is the only
    // non-surprising behaviour.
    //
    // This is a write site, not a readout, and that is deliberate here: an
    // explicit adjust-confirm IS a deliberate write - the user is looking at
    // the rail and choosing its value - so persisting the hardware truth they
    // just confirmed is exactly right, and the divergence heals as a result.
    // (Cancel still writes nothing; the callback restores the entry value.)
    switch (rail) {
        case 0: // Both rails
            config.initialValue = (getDacHardwareVoltage(2) + getDacHardwareVoltage(3)) / 2.0;
            config.label = "Rails";
            break;
        case 1: // Top rail
            config.initialValue = getDacHardwareVoltage(2);
            config.label = "Top Rail";
            break;
        case 2: // Bottom rail
            config.initialValue = getDacHardwareVoltage(3);
            config.label = "Bot Rail";
            break;
    }
    
    // Set callback and context
    config.callback = railVoltageCallback;
    config.context = (void*)(intptr_t)rail;
    
    // Run the adjuster
    AdjustResult result = VoltageAdjuster::adjust(config);
    
    if (result == AdjustResult::CONFIRMED) {
        // User confirmed - save voltages to persistent storage
        saveVoltages(globalState.power.topRail, globalState.power.bottomRail,
                     globalState.power.dac0, globalState.power.dac1);
        
        // Re-highlight the rail to show updated voltage. clearHighlighting()
        // zeroes highlightedNet/brightenedNode, so the identity has to be
        // latched across it or the re-highlight highlights nothing.
        int heldNet = highlightedNet;
        int heldNode = brightenedNode;
        clearHighlighting();
        brightenedNet = heldNet;
        brightenedNode = heldNode;
        highlightNets(0, heldNet, 1);
    } else {
        // User cancelled - restore original values in globalState
        globalState.power.topRail = origTopRail;
        globalState.power.bottomRail = origBottomRail;
        
        // Hardware was already restored by VoltageAdjuster callback
        // Re-highlight to refresh display
        int heldNet = highlightedNet;
        int heldNode = brightenedNode;
        clearHighlighting();
        brightenedNet = heldNet;
        brightenedNode = heldNode;
        highlightNets(0, heldNet, 1);
    }
    
    requestLedShow( 1 );
}

/**
 * @brief Launch voltage adjustment UI for a DAC
 * 
 * @param dac Which DAC to adjust (0=DAC0, 1=DAC1)
 */
void Highlighting::adjustDACVoltage(int dac) {
    // Save original values for cancellation
    float origDac0 = globalState.power.dac0;
    float origDac1 = globalState.power.dac1;
    
    // Prepare configuration
    VoltageAdjustConfig config;
    config.minVoltage = -8.0;
    config.maxVoltage = 8.0;
    config.enableSnap = false;
    config.liveUpdateInRange = true;
    config.liveUpdateMin = 0.0;
    config.liveUpdateMax = 5.0;
    
    // Set initial value and label based on which DAC
    config.initialValue = (dac == 0) ? globalState.power.dac0 : globalState.power.dac1;
    config.label = (dac == 0) ? "DAC 0" : "DAC 1";
    
    // Set callback and context
    config.callback = dacVoltageCallback;
    config.context = (void*)(intptr_t)dac;
    
    // Run the adjuster
    AdjustResult result = VoltageAdjuster::adjust(config);
    
    if (result == AdjustResult::CONFIRMED) {
        // Commit the confirmed value to HARDWARE as a user write. Values
        // outside the live-update band (0..5 V) were preview-only while
        // scrolling - state said 7 V, the chip still held the last live
        // value - and the adjuster's own confirm-path write is commented out,
        // so without this the DAC never moved and the feed's viability judged
        // a voltage that wasn't there. In-band values re-write the same word
        // and the driver skips the I2C.
        if (dac == 0) setDac0voltage(config.initialValue, 1, 0, true);
        else          setDac1voltage(config.initialValue, 1, 0, true);
        // User confirmed - save voltages to persistent storage
        saveVoltages(globalState.power.topRail, globalState.power.bottomRail,
                     globalState.power.dac0, globalState.power.dac1);
        
        // Re-highlight the DAC to show updated voltage. clearHighlighting()
        // zeroes highlightedNet/brightenedNode, so the identity has to be
        // latched across it or the re-highlight highlights nothing.
        int heldNet = highlightedNet;
        int heldNode = brightenedNode;
        clearHighlighting();
        brightenedNet = heldNet;
        brightenedNode = heldNode;
        highlightNets(0, heldNet, 1);
    } else {
        // User cancelled - restore the ORIGINAL value in state AND hardware.
        // (The long-press cancel in VoltageAdjuster does not call the
        // callback, so the chip is still at the last live value.) State goes
        // back by assignment (no dirty mark, no undo entry for a no-op);
        // the hardware write is save=0 but still a USER write, so the claim
        // latch / feed follow the restored voltage too.
        globalState.power.dac0 = origDac0;
        globalState.power.dac1 = origDac1;
        if (dac == 0) setDac0voltage(origDac0, 0, 0, true);
        else          setDac1voltage(origDac1, 0, 0, true);
        
        // Re-highlight to refresh display
        int heldNet = highlightedNet;
        int heldNode = brightenedNode;
        clearHighlighting();
        brightenedNet = heldNet;
        brightenedNode = heldNode;
        highlightNets(0, heldNet, 1);
    }
    
    requestLedShow( 1 );
}

// ============================================================================
// NOTE: Measure Mode Implementation has been moved to MeasureMode.cpp
// ============================================================================
// The measure mode functionality is now handled by the MeasureMode service,
// which provides:
// - Ephemeral connections (never saved to YAML)
// - Node validation (ignores special function pads)
// - Optional oscilloscope display
// - Extensible measurement types for future expansion
// See MeasureMode.h and MeasureMode.cpp for the implementation.