// SPDX-License-Identifier: MIT
/**
 * @file MeasureMode.cpp
 * @brief MeasureMode service implementation - voltage measurement
 * 
 * This service handles continuous voltage measurement when the probe switch is in
 * measure mode. It creates ephemeral (temporary) ADC connections that are never
 * saved to the slot file.
 */

#include "MeasureMode.h"
#include "ReadingDisplay.h" // the pinned live reading line
#include "InfraPaths.h"
#include "States.h"
#include "Probing.h"
#include "Peripherals.h"
#include "NetManager.h"
#include "Commands.h"
#include "LEDs.h"
#include "oled.h"
#include "Highlighting.h"
#include "externVars.h"  // For measureModeActive indicator
#include "USBAudio.h"    // usb_audio_probe_activity()

// External references
extern JumperlessState globalState;
extern class oled oled;
extern volatile unsigned long lastProbeCurrentCheckTime;
// Singleton Implementation
// ============================================================================
MeasureMode& MeasureMode::getInstance() {
    static MeasureMode inst;
    return inst;
}

// Global reference for convenience
MeasureMode& measureModeService = MeasureMode::getInstance();

MeasureMode::MeasureMode() {}

// ============================================================================
// Service Implementation
// ============================================================================

ServiceStatus MeasureMode::service() {
    lastStatus = ServiceStatus::IDLE;

#if defined(OG_JUMPERLESS)
    // OG has no probe-pad ADC + connect/measure switch wired into this V5
    // service (its probe is the Phase-2 scanning system). Left enabled it sits
    // in "measure mode", flooding Serial every loop with the voltage line the
    // user saw "constantly flashing", and calling oled.clearPrintShow() into an
    // OLED that's never initialized on OG (uninitialized I2C poke each cycle).
    // No-op until the scanning-probe measure path exists for OG.
    return lastStatus;
#endif

    // Track switch position changes with debounce
    if (switchPosition != lastSwitchPosition) {
        lastSwitchPosition = switchPosition;
        switchStableTime = millis();
    }
    
    // Only act on switch position after it's been stable for debounce period
    bool switchStable = (millis() - switchStableTime) >= SWITCH_DEBOUNCE_MS;
    
    // CRITICAL: Check if we're too close to an INA219 read - if so, skip probe reading this cycle
    unsigned long timeSinceProbeCurrentCheck = millis() - lastProbeCurrentCheckTime;
    bool safeToReadProbe = (timeSinceProbeCurrentCheck >= PROBE_GUARD_MS);
    
    if (switchPosition == 0 && switchStable) {
        // Switch is stably in measure mode - show indicator immediately
        if (!measureModeActive) {
            measureModeActive = true;  // Turn logo purple right away
        }
        
        // Get probe reading - but only if safe to read (not too close to INA219 check)
        int currentProbeReading = -1;
        if (safeToReadProbe) {
            currentProbeReading = probing.getLastProbeReading();
            
            // Track consecutive stable readings
            if (currentProbeReading > 0) {
                if (currentProbeReading == lastProbeReading) {
                    stableReadingCount++;
                } else {
                    // Different reading - reset counter
                    lastProbeReading = currentProbeReading;
                    stableReadingCount = 1;
                }
            } else {
                // No reading - don't reset immediately, might just be a gap
                if (stableReadingCount > 0) {
                    stableReadingCount--;
                }
                if (stableReadingCount == 0) {
                    lastProbeReading = -1;
                }
            }
        }
        
        // ONE positive cached reading IS the tap (the arc's contract, now
        // shared with Highlighting and PartLabels): justReadProbe already
        // requires two consecutive decodes before reporting a new row, and it
        // re-emits a HELD row only once per 500 ms with -1 in between - so
        // this counter only ever reached 5 because this service happens to be
        // periodUs=0 and samples the 10 ms cache window many times per pass.
        // Any scheduling change would have silently killed measure-mode taps
        // (sweep finding). The counter still runs the probe-lifted bookkeeping.
        bool probeReadingStable = (stableReadingCount >= 1);
        int stableProbeReading = probeReadingStable ? lastProbeReading : -1;
        
        // Validate that the reading is a measurable node (not special function pad)
        if (stableProbeReading > 0 && !isValidMeasurableNode(stableProbeReading)) {
            stableProbeReading = -1;  // Ignore special function pads
        }
        
        // If measure mode is active, update the display
        if (measurementActive) {
#if USB_AUDIO_ENABLE
            // Measuring means the probe tip is parked on a node: keep the USB
            // audio capture yielded so the tip/pad channels read live.
            usb_audio_probe_activity();
#endif
            updateVoltageDisplay();
            
            // Check if user tapped a different node (via probe) - only if stable
            if (stableProbeReading > 0 && stableProbeReading != measuredNode) {
                // User tapped a different node with stable reading - switch to measuring that one
                startMeasurement(stableProbeReading);
                stableReadingCount = 0;  // Reset for next detection
            }
            
            lastStatus = ServiceStatus::BUSY;
            // NB: this return skips only the REST OF THIS FUNCTION. It does
            // not stop other services - BUSY is documented as "actively
            // working but non-blocking" (JumperlOS.h) and serviceAll() latches
            // blockingService only on BLOCKING. ProbePads (20Hz), ProbeSwitch
            // (500ms), Peripherals (150ms) and Probing (100Hz) all keep
            // running, and keep printing to the same terminal, while a
            // measurement is live. The comment here used to claim otherwise,
            // which is how several at-cursor prints ended up fighting the
            // live reading line for the user's input row.
            return lastStatus;
        }
        
        // Not in measure mode yet - check if user tapped a node (only if stable)
        if (stableProbeReading > 0) {
            // User tapped a valid node with stable reading - start measuring
            startMeasurement(stableProbeReading);
            stableReadingCount = 0;  // Reset for next detection
            lastStatus = ServiceStatus::BUSY;
            return lastStatus;
        }
        
    } else if (switchPosition != 0) {
        // ANY non-measure switch position releases measure mode. This used to
        // be `switchPosition == 1`, which meant position -1 ("unknown" - what
        // probing.checkSwitchPosition() holds when it can't sense, and what
        // jl_set_switch_position() may legally write) latched the measurement
        // on forever: the display kept repainting one row's voltage and
        // service() returned BUSY, starving every other service.
        // Still scoped to != 0 so the 300ms debounce window at position 0
        // (where switchStable is false) leaves an active measurement alone.
        // EDGE-TRIGGERED. This branch runs on every service pass while the
        // switch sits in the normal select position, so the teardown must fire
        // only on the transition out of measure mode. Unconditionally it was
        // ruinous: Highlighting is registered immediately before this service
        // at the same priority, so in the SAME pass it pins a live reading and
        // we wipe it - clearLiveSerialLine() keys off the shared pinReserved
        // flag and has no idea who owns the row. A changing value re-pinned and
        // got erased at up to 25 Hz, scrolling the terminal and eating the
        // user's typed line every ~40 ms.
        if ( measurementActive || measureModeActive ) {
            // Leaving measure mode: erase the live line so the last voltage
            // doesn't sit above the prompt looking current. Deliberately NOT
            // done inside stopMeasurement(): startMeasurement() calls that on
            // every node change, and erasing per node change would blank the
            // reading between rows instead of letting it repaint in place.
            ReadingDisplay::clearLiveSerialLine();
            stopMeasurement();          // no-ops when nothing is active
            measureModeActive = false;  // turn the logo indicator off
        }
        // Stability tracking is cheap and must reset every pass, so a
        // half-accumulated count can't latch onto a stale node on the way back.
        stableReadingCount = 0;
        lastProbeReading = -1;
    }
    
    return lastStatus;
}

// ============================================================================
// Node Validation
// ============================================================================

bool MeasureMode::isValidMeasurableNode(int node) {
    // Breadboard rows (1-60) - the most common case
    if (node >= 1 && node <= 60) return true;
    
    // Nano header pins (70-93, excluding special pins like RESET, AREF)
    if (node >= NANO_D0 && node <= NANO_A7) return true;
    
    // Rails are measurable
    if (node == TOP_RAIL || node == BOTTOM_RAIL) return true;
    
    // DACs are measurable (useful for checking DAC output)
    if (node == DAC0 || node == DAC1) return true;
    
    // Supply pins are measurable
    if (node == SUPPLY_3V3 || node == SUPPLY_5V) return true;
    
    // GND is technically measurable (should read ~0V)
    if (node == GND) return true;
    
    // Special function pads - NOT measurable (ignore silently)
    // LOGO_PAD_TOP (142), GPIO_PAD (144), ADC_PAD (146), etc.
    // These pads control special functions and shouldn't be measured
    return false;
}

// ============================================================================
// Measurement Control
// ============================================================================

void MeasureMode::startMeasurement(int node) {
    // Don't start if already measuring this node
    if (measurementActive && measuredNode == node) {
        return;
    }

    // Same net = same voltage: the ADC route in place already reads this
    // node. Re-routing anyway cost two full fabric rebuilds per row hop
    // (ephemeral remove + add), each one a chance to glitch the display
    // bus mid-chunk - and measure mode hops rows constantly.
    {
        extern int8_t nodeToNetIndex[256];
        if (measurementActive && node > 0 && node < 256 &&
            measuredNode > 0 && measuredNode < 256 &&
            nodeToNetIndex[node] > 0 &&
            nodeToNetIndex[node] == nodeToNetIndex[measuredNode]) {
            measuredNode = node;
            lastUpdateTime = 0;   // repaint the label for the new node now
            firstReading = true;
            brightenNet(node);
            return;
        }
    }

    // Stop any existing measurement first
    if (measurementActive) {
        stopMeasurement();
    }
    
    // Connect ADC to the node
    if (!connectADCToNode(node)) {
        return;  // Failed to connect - error already reported
    }
    
    measuredNode = node;
    measurementActive = true;
    // Note: measureModeActive is controlled by switch position in service(), not here
    lastUpdateTime = 0;  // Force immediate first update
    firstReading = true;  // Reset smoothing
    
    // Highlight the node being measured (minimal LED change)
    brightenNet(node);
}

void MeasureMode::stopMeasurement() {
    if (!measurementActive) {
        return;
    }
    
    // Disconnect ADC
    disconnectADC();
    
    int wasNode = measuredNode;
    measurementActive = false;
    // Note: measureModeActive is controlled by switch position in service(), not here
    measuredNode = -1;
    adcChannel = -1;
    adcDefine = -1;
    firstReading = true;

    // Stability tracking has to die with the measurement, not just on the
    // switch-position path: an external stop (Python session end) would
    // otherwise leave a full count + stale node ready to re-latch instantly.
    stableReadingCount = 0;
    lastProbeReading = -1;
    
    // Clear highlighting state
    Highlighting& hl = Highlighting::getInstance();
    hl.brightenedNode = -1;
    hl.brightenedNet = -1;
    hl.brightenedRail = -1;
    hl.highlightedNet = -1;
    hl.clearHighlighting();
    
    // Redraw the nets WITHOUT a clear-first refresh - the old unconditional
    // clear blanked the ENTIRE board on every tip lift / row change while
    // probing around in measure mode. The one thing the clear actually
    // handled was a measured node with no net (nothing repaints its
    // highlight pixels), so unpaint just that node's LED(s) instead.
    extern int8_t nodeToNetIndex[256];
    bool nodeHasNet = wasNode > 0 && wasNode < 256 && nodeToNetIndex[wasNode] > 0;
    if (!nodeHasNet) {
        if (wasNode >= 1 && wasNode <= 60) {
            // Breadboard row: 5 pixels per row.
            for (int col = 0; col < 5; col++) {
                int pixel = (wasNode - 1) * 5 + col;
                if (pixel >= 0 && pixel < LED_COUNT) {
                    leds.setPixelColor(pixel, 0);
                }
            }
        } else if (wasNode >= NANO_D0 && wasNode < 120 && nodesToPixelMap[wasNode] >= 0) {
            // Nano header / special-function pin: a single LED in the top
            // strip. showNets() only ever REPAINTS pixels that still belong
            // to a net, so once the ephemeral ADC net is gone nothing clears
            // this one - it stayed lit after selecting a new node ("measure
            // mode isn't clearing the nano header"). Mirror the +320 top-strip
            // offset showNets() paints it at.
#if defined(OG_JUMPERLESS)
            leds.setPixelColor(nodesToPixelMap[wasNode], 0);
#else
            leds.setPixelColor(nodesToPixelMap[wasNode] + 320, 0);
#endif
        }
    }
    requestLedShow( 1 );
}

// ============================================================================
// ADC Connection Management
// ============================================================================

int MeasureMode::findUnusedADC() {
    // Pool-arbitrated: measure mode may use ADC0-4 (mask 0x1F; ADC7 is
    // hardwired to the probe tip). Exclusive ownership until disconnectADC.
    return infraAcquireAdc(INFRA_ADC_MEASURE, 0x1F, false);
}

bool MeasureMode::connectADCToNode(int node) {
    // Find an unused ADC
    int channel = findUnusedADC();
    if (channel == -1) {
        // No ADC available - show error briefly
        ReadingDisplay::emitLiveSerialLine("No ADC available");
        oled.clearPrintShow("No ADC\navailable", 2, true, true, true);
        return false;
    }
    
    // Convert channel number to ADC define
    const int adcDefines[] = { ADC0, ADC1, ADC2, ADC3, ADC4 };
    adcChannel = channel;
    adcDefine = adcDefines[channel];
    
    // Connect ADC to the node using EPHEMERAL connection
    // This bypasses markDirty() and will never be saved to the slot file
    // Pass applyRouting=true to immediately route and send to hardware
    // Use ledShowOption=0 to minimize visual disruption during measurement
    String errorMsg;
    if (!globalState.addEphemeralConnection(node, adcDefine, errorMsg, 
                                            true,   // applyRouting - immediately apply to hardware
                                            0)) {   // ledShowOption - no LED update during measurement
        // Single line, no newline: a newline here would scroll the reserved
        // area out from under its own anchor.
        char failLine[64];
        snprintf(failLine, sizeof(failLine), "Connection failed: %s", errorMsg.c_str());
        ReadingDisplay::emitLiveSerialLine(failLine);
        adcChannel = -1;
        adcDefine = -1;
        return false;
    }
    
    return true;
}

void MeasureMode::disconnectADC() {
    if (measuredNode != -1 && adcDefine != -1) {
        // Remove the ephemeral ADC connection
        // Pass applyRouting=true to immediately update hardware.
        // ledShowOption 1 = plain redraw. It used to pass -1 assuming
        // "default", but refreshLocalConnections feeds this straight into
        // showLEDsCore2 where negative means CLEAR-before-send - which
        // blanked the whole board for a few ms on every tip lift / row
        // change in measure mode.
        String errorMsg;
        globalState.removeEphemeralConnection(measuredNode, adcDefine, errorMsg,
                                              true,   // applyRouting - immediately apply to hardware
                                              1);     // ledShowOption - plain redraw, no clear
    }
    // Return the channel to the ADC pool (the ephemeral machinery above owns
    // routing/cleanup; the pool only arbitrates WHICH ADC we held).
    infraReleaseAdc(INFRA_ADC_MEASURE);
}

// ============================================================================
// Voltage Display (Simple Mode)
// ============================================================================

static float lastVoltage = 0.0f;
static int lastMeasuredNode = -1;
void MeasureMode::updateVoltageDisplay() {
    if (!measurementActive || adcChannel < 0) {
        return;
    }
    
    unsigned long now = millis();
    if (now - lastUpdateTime < UPDATE_INTERVAL_MS) {
        return;  // Rate limit updates
    }
    lastUpdateTime = now;
    
    // CRITICAL: Don't read ADC too close to an INA219 probe current check
    if ((now - lastProbeCurrentCheckTime) < 10) {
        return;  // Skip this update cycle - too close to INA219 read
    }
    
    // Read voltage from the ADC (more samples for stability)
    float rawVoltage = readAdcVoltage(adcChannel, 16);
    
    // Apply exponential moving average for smoothing
    if (firstReading) {
        smoothedVoltage = rawVoltage;
        firstReading = false;
    } else {
        smoothedVoltage = (SMOOTHING_FACTOR * rawVoltage) + ((1.0f - SMOOTHING_FACTOR) * smoothedVoltage);
    }
    
    // Fix -0.00 display issue
    if (smoothedVoltage == -0.00f) {
        smoothedVoltage = 0.00f;
    }
    if (fabs(smoothedVoltage - lastVoltage) > 0.01 || measuredNode != lastMeasuredNode) {
        lastMeasuredNode = measuredNode;
        lastVoltage = smoothedVoltage;

        // Both halves through the shared reading display: the node name is the
        // header, the voltage the big centered value, and the serial side lands
        // on the pinned rows above the input line. The second value slot stays
        // open for a current reading, the way I Sense nets show V over mA.
        char valueString[16];
        snprintf(valueString, sizeof(valueString), "%0.2f V", smoothedVoltage);
        ReadingDisplay::show(definesToChar(measuredNode, 0), -1, valueString);
    }
}

// ============================================================================
// Measurement Type (for future expansion)
// ============================================================================

void MeasureMode::setMeasurementType(MeasurementType type) {
    if (type != currentType) {
        // Stop current measurement when changing type
        if (measurementActive) {
            stopMeasurement();
        }
        currentType = type;
    }
}
