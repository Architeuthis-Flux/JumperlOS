// SPDX-License-Identifier: MIT
/**
 * @file MeasureMode.cpp
 * @brief MeasureMode service implementation - voltage measurement with optional oscilloscope
 * 
 * This service handles continuous voltage measurement when the probe switch is in
 * measure mode. It creates ephemeral (temporary) ADC connections that are never
 * saved to the slot file.
 */

#include "MeasureMode.h"
#include "States.h"
#include "Probing.h"
#include "Peripherals.h"
#include "NetManager.h"
#include "Commands.h"
#include "LEDs.h"
#include "oled.h"
#include "Highlighting.h"
#include "externVars.h"  // For measureModeActive indicator
#include "CH446Q.h"
#include "TimeDomainMultiplexer.h"
#include "FakeGpio.h"
#include "Wavegen.h"
#include "config.h"
#include "LogicAnalyzer.h"

// External references
extern JumperlessState globalState;
extern class oled oled;
extern volatile unsigned long lastProbeCurrentCheckTime;
extern volatile int showLEDsCore2;
extern volatile int probeActive;
extern WaveGen wavegen;             // defined in main.cpp
bool isAdcInUseByOtherConnections(int adcChannel);  // TimeDomainMultiplexer.cpp

// ============================================================================
// Singleton Implementation
// ============================================================================

MeasureMode* MeasureMode::instance = nullptr;

MeasureMode& MeasureMode::getInstance() {
    if (instance == nullptr) {
        instance = new MeasureMode();
    }
    return *instance;
}

// Global reference for convenience
// NOTE: Named measureModeService to avoid conflict with Probing::measureMode() function
MeasureMode& measureModeService = MeasureMode::getInstance();

MeasureMode::MeasureMode() {
    // Initialize oscilloscope sample buffer
    for (int i = 0; i < OSCOPE_SAMPLES; i++) {
        oscopeSamples[i] = 0.0f;
    }
}

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
        
        // Only consider probe reading "stable" if we've seen the same value multiple times
        bool probeReadingStable = (stableReadingCount >= STABLE_READINGS_REQUIRED);
        int stableProbeReading = probeReadingStable ? lastProbeReading : -1;
        
        // Validate that the reading is a measurable node (not special function pad)
        if (stableProbeReading > 0 && !isValidMeasurableNode(stableProbeReading)) {
            stableProbeReading = -1;  // Ignore special function pads
        }
        
        // If measure mode is active, update the display
        if (measurementActive) {
            if (oscopeEnabled) {
                updateOscopeDisplay();
            } else {
                updateVoltageDisplay();
            }
            
            // Check if user tapped a different node (via probe) - only if stable
            if (stableProbeReading > 0 && stableProbeReading != measuredNode) {
                // User tapped a different node with stable reading - switch to measuring that one
                startMeasurement(stableProbeReading);
                stableReadingCount = 0;  // Reset for next detection
            }
            
            lastStatus = ServiceStatus::BUSY;
            return lastStatus;  // Don't process other services while in measure mode
        }
        
        // Not in measure mode yet - check if user tapped a node (only if stable)
        if (stableProbeReading > 0) {
            // User tapped a valid node with stable reading - start measuring
            startMeasurement(stableProbeReading);
            stableReadingCount = 0;  // Reset for next detection
            lastStatus = ServiceStatus::BUSY;
            return lastStatus;
        }
        
    } else if (switchPosition == 1) {
        // Switch is in select mode - stop measure mode if active
        if (measurementActive) {
            stopMeasurement();
        }
        // Turn off indicator when leaving measure mode
        if (measureModeActive) {
            measureModeActive = false;
        }
        // Reset measure mode stability tracking when leaving measure mode
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
    
    // Reset oscilloscope state
    oscopeSampleIndex = 0;
    for (int i = 0; i < OSCOPE_SAMPLES; i++) {
        oscopeSamples[i] = 0.0f;
    }
    
    // Highlight the node being measured (minimal LED change)
    brightenNet(node);

#if !defined(OG_JUMPERLESS)
    // Tapping a row that's one end of a routed row-to-row bridge converts
    // that bridge into a current tap: the mux replaces it with the INA0
    // shunt in series and the display below shows mA (REMOVE button or
    // V-<row> undoes it). Non-bridged nodes just measure voltage as always.
    if (node >= 1 && node <= 60) {
        measurementOverlay.addCurrentTapForNode(node);
    }
#endif
}

void MeasureMode::stopMeasurement() {
    if (!measurementActive) {
        return;
    }
    
    // Disconnect ADC
    disconnectADC();
    
    measurementActive = false;
    // Note: measureModeActive is controlled by switch position in service(), not here
    measuredNode = -1;
    adcChannel = -1;
    adcDefine = -1;
    firstReading = true;
    
    // Clear highlighting state
    Highlighting& hl = Highlighting::getInstance();
    hl.brightenedNode = -1;
    hl.brightenedNet = -1;
    hl.brightenedRail = -1;
    hl.highlightedNet = -1;
    hl.clearHighlighting();
    
    // Force LED update with clear
    showLEDsCore2 = -1;  // Negative triggers clearBeforeSend for full refresh
}

// ============================================================================
// ADC Connection Management
// ============================================================================

int MeasureMode::findUnusedADC() {
    // ADC defines: ADC0=110, ADC1=111, ADC2=112, ADC3=113, ADC4=114
    // ADC7=115 is reserved for probe
    const int adcDefines[] = { ADC0, ADC1, ADC2, ADC3, ADC4 };
    
    for (int i = 0; i < 5; i++) {
        bool inUse = false;
        
        // Check if this ADC is connected to anything in bridges
        for (int b = 0; b < globalState.connections.numBridges; b++) {
            if (globalState.connections.bridges[b][0] == adcDefines[i] ||
                globalState.connections.bridges[b][1] == adcDefines[i]) {
                inUse = true;
                break;
            }
        }
        
        if (!inUse) {
            return i;  // Return channel number 0-4
        }
    }
    
    return -1;  // All ADCs in use
}

bool MeasureMode::connectADCToNode(int node) {
    // Find an unused ADC
    int channel = findUnusedADC();
    if (channel == -1) {
        // No ADC available - show error briefly
        Serial.print("\r                                        \r");
        Serial.print("No ADC available");
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
        Serial.print("\r                                        \r");
        Serial.print("Connection failed: ");
        Serial.println(errorMsg);
        adcChannel = -1;
        adcDefine = -1;
        return false;
    }
    
    return true;
}

void MeasureMode::disconnectADC() {
    if (measuredNode != -1 && adcDefine != -1) {
        // Remove the ephemeral ADC connection
        // Pass applyRouting=true to immediately update hardware
        String errorMsg;
        globalState.removeEphemeralConnection(measuredNode, adcDefine, errorMsg,
                                              true,   // applyRouting - immediately apply to hardware
                                              -1);    // ledShowOption - restore normal LED display
    }
}

// ============================================================================
// Voltage Display (Simple Mode)
// ============================================================================

float lastVoltage = 0.0f;
int lastMeasuredNode = -1;
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

#if !defined(OG_JUMPERLESS)
    // Measured node is one end of a current tap -> show the bridge's current
    // AND the probed node's voltage together
    const MeasurementOverlay::CurrentTap* tap = measurementOverlay.tapForNode(measuredNode);
    if (tap != nullptr) {
        static float lastMaShown = NAN;
        bool changed = (measuredNode != lastMeasuredNode) ||
                       (isfinite(tap->ma) != isfinite(lastMaShown)) ||
                       (isfinite(tap->ma) && fabsf(tap->ma - lastMaShown) > 0.05f) ||
                       (fabsf(smoothedVoltage - lastVoltage) > 0.05f);
        if (changed) {
            lastMeasuredNode = measuredNode;
            lastMaShown = tap->ma;
            lastVoltage = smoothedVoltage;
            // Three lines: bridge, voltage, current (clearPrintShow scales
            // multi-line text to fit the display height)
            char oledString[48];
            if (isfinite(tap->ma)) {
                snprintf(oledString, sizeof(oledString), "%d-%d\n% .2f V\n% .2f mA",
                         tap->nodeA, tap->nodeB, smoothedVoltage, tap->ma);
            } else {
                snprintf(oledString, sizeof(oledString), "%d-%d\n% .2f V\n  -- mA",
                         tap->nodeA, tap->nodeB, smoothedVoltage);
            }
            oled.clearPrintShow(oledString, 2, true, true, true);
            Serial.print("                                        \r");
            if (isfinite(tap->ma)) Serial.print(tap->ma, 2);
            else Serial.print("--");
            Serial.printf(" mA  %d-%d  (%.2f V)", tap->nodeA, tap->nodeB, smoothedVoltage);
        }
        return;
    }
#endif

    if (fabs(smoothedVoltage - lastVoltage) > 0.01 || measuredNode != lastMeasuredNode ) {  
        lastMeasuredNode = measuredNode;
    // Format voltage and node info for OLED
    char oledString[30];
    sprintf(oledString, "%s\n  % .2f V", definesToChar(measuredNode, 0), smoothedVoltage);
    
    // Update OLED display
    oled.clearPrintShow(oledString, 2, true, true, true);
    
    // Update serial output (clear line first)
    
    Serial.print("                                        \r");
    Serial.print(smoothedVoltage, 2);
    Serial.print(" V  row ");
        Serial.print(definesToChar(measuredNode, 0));
        lastVoltage = smoothedVoltage;
    }
}

// ============================================================================
// Oscilloscope Mode
// ============================================================================

void MeasureMode::enableOscilloscope(bool enable) {
    oscopeEnabled = enable;
    if (enable) {
        // Reset oscilloscope state
        oscopeSampleIndex = 0;
        for (int i = 0; i < OSCOPE_SAMPLES; i++) {
            oscopeSamples[i] = smoothedVoltage;  // Initialize to current voltage
        }
        oscopeMin = 0.0f;
        oscopeMax = 3.3f;
    }
}

void MeasureMode::setOscopeTimebase(int timebaseMs) {
    oscopeTimebaseMs = constrain(timebaseMs, 1, 1000);
}

void MeasureMode::sampleForOscope() {
    // Sample at rate determined by timebase
    unsigned long sampleInterval = (oscopeTimebaseMs * 1000UL) / OSCOPE_SAMPLES;  // microseconds
    unsigned long now = micros();
    
    if (now - lastOscopeSampleTime >= sampleInterval) {
        lastOscopeSampleTime = now;
        
        // Take sample with fewer averages for speed
        oscopeSamples[oscopeSampleIndex] = readAdcVoltage(adcChannel, 4);
        oscopeSampleIndex = (oscopeSampleIndex + 1) % OSCOPE_SAMPLES;
    }
}

void MeasureMode::autoRangeOscope() {
    // Find min/max in sample buffer
    float min = 3.3f, max = 0.0f;
    for (int i = 0; i < OSCOPE_SAMPLES; i++) {
        if (oscopeSamples[i] < min) min = oscopeSamples[i];
        if (oscopeSamples[i] > max) max = oscopeSamples[i];
    }
    
    // Add 10% margin
    float range = max - min;
    oscopeMin = min - range * 0.1f;
    oscopeMax = max + range * 0.1f;
    
    // Ensure minimum range to avoid division by zero
    if (oscopeMax - oscopeMin < 0.1f) {
        float center = (max + min) / 2.0f;
        oscopeMin = center - 0.05f;
        oscopeMax = center + 0.05f;
    }
    
    // Clamp to valid voltage range
    if (oscopeMin < 0.0f) oscopeMin = 0.0f;
    if (oscopeMax > 3.3f) oscopeMax = 3.3f;
}

float MeasureMode::voltageToPixelY(float voltage) {
    // OLED is 64 pixels tall, plot area is 56 pixels (leaving 8 for status bar)
    constexpr int PLOT_HEIGHT = 56;
    
    // Clamp voltage to range
    if (voltage < oscopeMin) voltage = oscopeMin;
    if (voltage > oscopeMax) voltage = oscopeMax;
    
    // Map voltage to pixel (inverted - 0V at bottom, 3.3V at top)
    float normalized = (voltage - oscopeMin) / (oscopeMax - oscopeMin);
    return PLOT_HEIGHT - 1 - (normalized * (PLOT_HEIGHT - 1));
}

void MeasureMode::drawOscopeGrid() {
    // Draw dotted grid lines
    // OLED is 128x64, plot area is 128x56
    
    // Vertical divisions (every 16 pixels = 8 divisions)
    for (int x = 0; x < 128; x += 16) {
        for (int y = 0; y < 56; y += 4) {
            oled.setPixel(x, y, 1);
        }
    }
    
    // Horizontal divisions (center line and quarters)
    int centerY = 28;
    int quarterY = 14;
    int threeQuarterY = 42;
    
    for (int x = 0; x < 128; x += 4) {
        oled.setPixel(x, centerY, 1);
        oled.setPixel(x, quarterY, 1);
        oled.setPixel(x, threeQuarterY, 1);
    }
}

void MeasureMode::drawOscopeWaveform() {
    // Draw waveform as connected line segments
    for (int x = 0; x < 127; x++) {
        int idx1 = (oscopeSampleIndex + x) % OSCOPE_SAMPLES;
        int idx2 = (oscopeSampleIndex + x + 1) % OSCOPE_SAMPLES;
        
        int y1 = (int)voltageToPixelY(oscopeSamples[idx1]);
        int y2 = (int)voltageToPixelY(oscopeSamples[idx2]);
        
        // Clamp to plot area
        y1 = constrain(y1, 0, 55);
        y2 = constrain(y2, 0, 55);
        
        // Draw line segment (vertical line if points differ significantly)
        if (y1 == y2) {
            oled.setPixel(x, y1, 1);
        } else {
            // Draw vertical line between points
            int yMin = min(y1, y2);
            int yMax = max(y1, y2);
            for (int y = yMin; y <= yMax; y++) {
                oled.setPixel(x, y, 1);
            }
        }
    }
}

void MeasureMode::drawOscopeStatusBar() {
    // Status bar at bottom (y=56 to y=63)
    // Draw separator line
    for (int x = 0; x < 128; x++) {
        oled.setPixel(x, 56, 1);
    }
    
    // Format status text
    char status[32];
    sprintf(status, "%.2fV @%d %dms", smoothedVoltage, measuredNode, oscopeTimebaseMs);
    
    // Draw status text using OLED's small font
    oled.setCursor(0, 63);
    oled.setTextSize(1);
    oled.print(status);
}

void MeasureMode::updateOscopeDisplay() {
    if (!measurementActive || adcChannel < 0) {
        return;
    }
    
    // Sample for oscilloscope (rate determined by timebase)
    sampleForOscope();
    
    // Also update smoothed voltage for status bar
    float rawVoltage = readAdcVoltage(adcChannel, 8);
    if (firstReading) {
        smoothedVoltage = rawVoltage;
        firstReading = false;
    } else {
        smoothedVoltage = (SMOOTHING_FACTOR * rawVoltage) + ((1.0f - SMOOTHING_FACTOR) * smoothedVoltage);
    }
    
    // Rate limit display updates
    unsigned long now = millis();
    if (now - lastUpdateTime < OSCOPE_UPDATE_INTERVAL_MS) {
        return;
    }
    lastUpdateTime = now;
    
    // Auto-range based on captured data
    autoRangeOscope();
    
    // Clear and redraw display
    oled.clearFramebuffer();
    
    // Draw in order: grid, waveform, status bar
    drawOscopeGrid();
    drawOscopeWaveform();
    drawOscopeStatusBar();
    
    // Send to display
    oled.flushFramebuffer();
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

// ============================================================================
// ============================================================================
// Measurement Overlay
// ============================================================================
// ============================================================================

MeasurementOverlay* MeasurementOverlay::instance = nullptr;

MeasurementOverlay& MeasurementOverlay::getInstance() {
    if (instance == nullptr) {
        instance = new MeasurementOverlay();
    }
    return *instance;
}

MeasurementOverlay& measurementOverlay = MeasurementOverlay::getInstance();

MeasurementOverlay::MeasurementOverlay() {
    for (int i = 0; i <= 60; i++) {
        rowVolts[i] = 0.0f;
        rowStatus[i] = OVERLAY_ROW_NONE;
        rowNet[i] = -1;
    }
}

// ----------------------------------------------------------------------------
// Row set
// ----------------------------------------------------------------------------

void MeasurementOverlay::addRow(int row) {
    if (row < 1 || row > 60) return;
    rowMask = rowMask | (1ULL << (row - 1));
    __dmb();
}

void MeasurementOverlay::removeRow(int row) {
    if (row < 1 || row > 60) return;
    rowMask = rowMask & ~(1ULL << (row - 1));
    __dmb();
}

void MeasurementOverlay::clearRowSet() {
    rowMask = 0;
    __dmb();
}

bool MeasurementOverlay::rowInSet(int row) const {
    if (row < 1 || row > 60) return false;
    uint64_t mask = rowMask;
    if (mask == 0) return true;  // empty set = scan every row
    return (mask & (1ULL << (row - 1))) != 0;
}

#if !defined(OG_JUMPERLESS)

// ----------------------------------------------------------------------------
// V5 crossbar topology tables (mirrors rev5plusXmap in MatrixState.cpp;
// runLaneSelfCheck() verifies them against the live chipStates at runtime)
// ----------------------------------------------------------------------------

// Breadboard chip A-H -> X lane wired to chip K / I / J
static const int8_t chipToKlaneX[8] = { 9, 11, 13, 15, 1, 3, 5, 7 };
static const int8_t chipToIlaneX[8] = { 0, 2, 4, 6, 8, 10, 12, 14 };
static const int8_t chipToJlaneX[8] = { 1, 3, 5, 7, 9, 11, 13, 15 };

// Chip I/J X11 = ISENSE_PLUS / ISENSE_MINUS
#define ISENSE_PLUS_X 11
#define ISENSE_MINUS_X 11

// Chip K's X pins for the I and J wires, and (on chips I/J) the X pin of the
// K wire. Verified live by runLaneSelfCheck().
#define CHIP_K_ILANE_X 13
#define CHIP_K_JLANE_X 14
#define IJ_TO_K_X 14

// Row (1-28, 31-58) -> breadboard chip + Y lane.
// ponytail: rows 29/30/59/60 live directly on chips K/L and would need a
// Y-lane bounce; they're only reachable via the net-tap fallback. Upgrade
// path: add a K/L bounce resolver if anyone misses those four rows.
static inline bool rowToChipY(int row, int8_t& chip, int8_t& y) {
    if (row >= 1 && row <= 28) {
        chip = (row - 1) / 7;
        y = ((row - 1) % 7) + 1;
        return true;
    }
    if (row >= 31 && row <= 58) {
        chip = 4 + (row - 31) / 7;
        y = ((row - 31) % 7) + 1;
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Crossbar path-send generation counter (bumped by sendPaths() in CH446Q.cpp).
// Any full/incremental path resend can move routing, so caches keyed on this.
// ----------------------------------------------------------------------------
extern volatile uint32_t crossbarPathGeneration;

// ----------------------------------------------------------------------------
// Row -> ADC path cache. Accessed by core 2 (sweep) and core 0 (current
// window taps) - both only ever run while holding the core_sync mutex, so no
// extra locking is needed.
// ----------------------------------------------------------------------------

#define ROWPATH_INVALID 0  // unroutable this generation
#define ROWPATH_DIRECT  1  // deterministic: bb chip hop + chip K hop
#define ROWPATH_NET_TAP 2  // net already reaches chip K; single K hop taps it
#define ROWPATH_IJ      3  // via chip I/J wire + a free K Y lane as bounce

struct RowAdcPath {
    int8_t kind = ROWPATH_INVALID;
    int8_t bbChip = -1, bbX = -1, bbY = -1;  // bb-side hop (DIRECT: K lane, IJ: I/J lane)
    int8_t kY = -1;                          // chip K Y lane for the ADC X (IJ: bounce lane)
    int8_t aggChip = -1;                     // ROWPATH_IJ only: CHIP_I or CHIP_J
    int8_t aggKx = -1;                       // ROWPATH_IJ only: K-side X of that wire (13/14)
    uint32_t gen = 0;
    bool resolved = false;                   // resolved for current gen?
};

static RowAdcPath rowPathCache[61];

static void __not_in_flash_func(rebuildRowNets)(MeasurementOverlay& mo) {
    for (int i = 0; i <= 60; i++) mo.rowNet[i] = -1;
    for (int i = 0; i < globalState.connections.numPaths && i < MAX_BRIDGES; i++) {
        const pathStruct& p = globalState.connections.paths[i];
        if (p.node1 >= 1 && p.node1 <= 60) mo.rowNet[p.node1] = p.net;
        if (p.node2 >= 1 && p.node2 <= 60) mo.rowNet[p.node2] = p.net;
    }
}

// Is this X lane free on the given chip (optionally ignoring one Y)?
static inline bool xLaneFree(int chip, int x, int exceptY = -1) {
    for (int y = 0; y < 8; y++) {
        if (y == exceptY) continue;
        if (lastChipXY[chip].connected[y] & (1 << x)) return false;
    }
    return true;
}

// A chip K Y lane is a usable bounce only when idle on BOTH ends: no X bits
// on the K side, and the owning breadboard chip's K-wire X unused on the bb
// side (rows can hang off the wire even when the K end looks clear).
static int __not_in_flash_func(pickFreeKBounceLane)() {
    for (int y = 7; y >= 0; y--) {
        if (lastChipXY[CHIP_K].connected[y] != 0) continue;
        if (!xLaneFree(y, chipToKlaneX[y])) continue;
        return y;
    }
    return -1;
}

// I/J bounce resolver: rail/DAC stacking routinely occupies EVERY chip's K Y
// lane (K-side bits = kSideOk false), killing the direct tier for whole chips
// at a time - that was the "rows 1-30 don't read" bug. Route around it:
// row -> chip's I (or J) wire -> the I/J<->K wire -> any K Y lane that's idle
// on both ends -> ADC X. Four fresh crosspoints, nothing shared with routed
// nets, so nothing to glitch. (An inserted current tap occupies the I/J
// lanes of its two chips; rows there fall back to the other wire or go
// stale for the dwell - the lane checks below handle it.)
static bool __not_in_flash_func(tryResolveIJPath)(int row, RowAdcPath& p) {
    int8_t chip, y;
    if (!rowToChipY(row, chip, y)) return false;
    for (int viaJ = 0; viaJ < 2; viaJ++) {
        int aggChip = viaJ ? CHIP_J : CHIP_I;
        int8_t aggX = viaJ ? chipToJlaneX[chip] : chipToIlaneX[chip];
        int8_t kx = viaJ ? CHIP_K_JLANE_X : CHIP_K_ILANE_X;
        if (!xLaneFree(chip, aggX)) continue;                    // chip's I/J wire busy
        if (lastChipXY[aggChip].connected[chip] != 0) continue;  // I/J lane for chip busy
        if (!xLaneFree(aggChip, IJ_TO_K_X)) continue;            // I/J<->K wire, I/J end
        if (!xLaneFree(CHIP_K, kx)) continue;                    // I/J<->K wire, K end
        int bounce = pickFreeKBounceLane();
        if (bounce < 0) return false;  // no bounce lane free for either wire
        p.kind = ROWPATH_IJ;
        p.bbChip = chip;
        p.bbX = aggX;
        p.bbY = y;
        p.kY = (int8_t)bounce;
        p.aggChip = (int8_t)aggChip;
        p.aggKx = kx;
        return true;
    }
    return false;
}

static void __not_in_flash_func(resolveRowPath)(MeasurementOverlay& mo, int row) {
    RowAdcPath& p = rowPathCache[row];
    p.resolved = true;
    p.gen = crossbarPathGeneration;
    p.kind = ROWPATH_INVALID;

    // 1) Deterministic 2-crosspoint lane: row's chip Y -> chip's K lane -> chip K
    int8_t chip, y;
    if (rowToChipY(row, chip, y)) {
        int8_t kx = chipToKlaneX[chip];
        // Router may already have this row on the K wire (reused, not re-sent)
        bool preExisting = getConnectionBit(lastChipXY[chip], kx, y);
        bool laneFree = xLaneFree(chip, kx, preExisting ? y : -1);
        // K side: fresh connection needs an empty Y lane (a used one belongs
        // to another net); if the row's net already owns the wire, any X bits
        // on that Y are the same net - tapping it is fine.
        bool kSideOk = preExisting || (lastChipXY[CHIP_K].connected[chip] == 0);
        if (laneFree && kSideOk) {
            p.kind = ROWPATH_DIRECT;
            p.bbChip = chip;
            p.bbX = kx;
            p.bbY = y;
            p.kY = chip;
            return;
        }
    }

    // 2) Fallback: the row's net is already routed through chip K somewhere -
    //    reuse the router's own path and just tap it with one K crosspoint.
    int net = mo.rowNet[row];
    if (net > 0) {
        for (int i = 0; i < globalState.connections.numPaths && i < MAX_BRIDGES; i++) {
            const pathStruct& path = globalState.connections.paths[i];
            if (path.net != net) continue;
            for (int j = 0; j < 4; j++) {
                if (path.chip[j] == CHIP_K && path.y[j] >= 0 && path.y[j] < 8) {
                    p.kind = ROWPATH_NET_TAP;
                    p.kY = (int8_t)path.y[j];
                    return;
                }
            }
        }
    }

    // 3) I/J bounce - see tryResolveIJPath(). ponytail: unroutable rows stay
    //    INVALID this generation (rendered stale); we deliberately never
    //    invoke the full router from here. Known ceiling: if stacking ever
    //    eats every K lane on BOTH ends, tier 3 has no bounce lane - upgrade
    //    path is reserving one K lane from the router's stacking.
    tryResolveIJPath(row, p);
}

static RowAdcPath* __not_in_flash_func(getRowPath)(MeasurementOverlay& mo, int row) {
    if (row < 1 || row > 60) return nullptr;
    RowAdcPath& p = rowPathCache[row];
    if (!p.resolved || p.gen != crossbarPathGeneration) {
        resolveRowPath(mo, row);
    }
    return &p;
}

// Pick an ADC (0-3) that nothing else owns: not FakeGPIO TDM's, not routed
// in the bridge array, and with an idle chip K lane.
static int __not_in_flash_func(pickFreeAdc)() {
    for (int adc = 0; adc < 4; adc++) {
        if (adc == tdmInputs.adcChannel) continue;
        if (isAdcInUseByOtherConnections(adc)) continue;
        if (!xLaneFree(CHIP_K, 8 + adc)) continue;
        return adc;
    }
    return -1;
}

// Chip K X15 = GND (see chipStatus K xMap)
#define CHIP_K_GND_X 15

// Un-netted row reading below this renders dark (FLOATING)
#define OVERLAY_FLOATING_DEADBAND_V 0.2f

// Phantom-charge check for un-netted rows: CH446Q off-leakage charges a
// floating row over hours to a solid-looking voltage (hardware-observed:
// ~3.3V after a day powered on) and the high-Z read faithfully reports it.
// Ground the ROW for 20us and re-read: a real source recovers well inside
// the 80us settle, held charge stays collapsed. Skipped when the reading
// matches the value already confirmed on a previous sweep, so genuinely
// driven rows only eat the pulse when their voltage actually changes.
static float __not_in_flash_func(phantomRecheck)(float v, int adc, int samples,
                                                 int kY, float skipV) {
    if (fabsf(v) < OVERLAY_FLOATING_DEADBAND_V) return v;      // already floating
    if (isfinite(skipV) && fabsf(v - skipV) < 0.3f) return v;  // confirmed earlier
    sendXYraw(CHIP_K, CHIP_K_GND_X, kY, 1);
    delayMicroseconds(20);
    sendXYraw(CHIP_K, CHIP_K_GND_X, kY, 0);
    delayMicroseconds(80);
    return readAdcVoltage(adc, samples);
}

// Connect the row to the ADC, read, and disconnect ONLY what we connected.
// Returns voltage; ok=false when the lanes were unusable this instant.
// discharge: bleed held charge off the ADC lane through chip K's GND
// crosspoint BEFORE the row is connected. The lane is high impedance and
// holds the PREVIOUS row's voltage; a floating row just inherits that charge
// ("reads the next row"). Only used for un-netted rows (a net would be
// briefly grounded otherwise). confirmSkipV: when finite and given discharge,
// enables the phantom-charge recheck (see phantomRecheck) with that value as
// the "already confirmed" skip; pass NAN to always recheck.
//
// ponytail: tried driving the RP2350B ADC pin low as SIO instead - faster,
// no crossbar strobes - but the ADC front-end op-amp saturates and the next
// rows trail a GND reading. Stay on the crossbar GND pulse.
static float __not_in_flash_func(readRowVoltageViaPath)(const RowAdcPath& p, int adc,
                                                        int samples, bool& ok,
                                                        bool discharge = false,
                                                        bool phantomCheck = false,
                                                        float confirmSkipV = NAN) {
    ok = false;
    if (p.kind == ROWPATH_INVALID || p.kY < 0) return 0.0f;
    int adcX = 8 + adc;

    // Last-instant congestion check (state can shift between slices)
    if (!xLaneFree(CHIP_K, adcX)) return 0.0f;

    if (p.kind == ROWPATH_IJ) {
        // Re-verify every wire in the chain (state can shift between slices)
        if (!xLaneFree(p.bbChip, p.bbX)) return 0.0f;
        if (lastChipXY[p.aggChip].connected[p.bbChip] != 0) return 0.0f;
        if (!xLaneFree(p.aggChip, IJ_TO_K_X)) return 0.0f;
        if (!xLaneFree(CHIP_K, p.aggKx)) return 0.0f;
        if (lastChipXY[CHIP_K].connected[p.kY] != 0) return 0.0f;
        if (!xLaneFree(p.kY, chipToKlaneX[p.kY])) return 0.0f;

        sendXYraw(CHIP_K, adcX, p.kY, 1);
        sendXYraw(CHIP_K, p.aggKx, p.kY, 1);
        sendXYraw(p.aggChip, IJ_TO_K_X, p.bbChip, 1);
        if (discharge) {
            // Bleed the chain before the row joins (K Y is otherwise empty)
            sendXYraw(CHIP_K, CHIP_K_GND_X, p.kY, 1);
            delayMicroseconds(20);
            sendXYraw(CHIP_K, CHIP_K_GND_X, p.kY, 0);
        }
        sendXYraw(p.bbChip, p.bbX, p.bbY, 1);

        delayMicroseconds(80);
        float v = readAdcVoltage(adc, samples);
        if (discharge && phantomCheck) {
            v = phantomRecheck(v, adc, samples, p.kY, confirmSkipV);
        }

        sendXYraw(p.bbChip, p.bbX, p.bbY, 0);
        sendXYraw(p.aggChip, IJ_TO_K_X, p.bbChip, 0);
        sendXYraw(CHIP_K, p.aggKx, p.kY, 0);
        sendXYraw(CHIP_K, adcX, p.kY, 0);
        ok = true;
        return v;
    }

    bool bbWasConnected = true;
    if (p.kind == ROWPATH_DIRECT) {
        bbWasConnected = getConnectionBit(lastChipXY[p.bbChip], p.bbX, p.bbY);
        if (!bbWasConnected) {
            if (!xLaneFree(p.bbChip, p.bbX)) return 0.0f;
            if (lastChipXY[CHIP_K].connected[p.kY] != 0) return 0.0f;
        }
    }

    sendXYraw(CHIP_K, adcX, p.kY, 1);

    // Ground pulse BEFORE the row is connected - only safe when this Y lane
    // carries nothing but our fresh tap (kind DIRECT, fresh bb hop)
    if (discharge && p.kind == ROWPATH_DIRECT && !bbWasConnected) {
        sendXYraw(CHIP_K, CHIP_K_GND_X, p.kY, 1);
        delayMicroseconds(20);
        sendXYraw(CHIP_K, CHIP_K_GND_X, p.kY, 0);
    }

    if (p.kind == ROWPATH_DIRECT && !bbWasConnected) {
        sendXYraw(p.bbChip, p.bbX, p.bbY, 1);
    }

    // CH446Q analog switch settling (see TimeDomainMultiplexer.cpp: 30us was
    // too short, 80us isolates reliably)
    delayMicroseconds(80);
    float v = readAdcVoltage(adc, samples);
    if (discharge && phantomCheck && p.kind == ROWPATH_DIRECT && !bbWasConnected) {
        v = phantomRecheck(v, adc, samples, p.kY, confirmSkipV);
    }
    sendXYraw(CHIP_K, adcX, p.kY, 0);

    if (p.kind == ROWPATH_DIRECT && !bbWasConnected) {
        sendXYraw(p.bbChip, p.bbX, p.bbY, 0);
    }

    ok = true;
    return v;
}

// Effective display mode: the config setting, or - when the probe switch sits
// in the measure position with the config toggle off - COLOR mode. COLOR only
// touches the center-channel LED per row, so wire rendering stays legible;
// the invasive BAR/DOT modes are opt-in through config/menu.
static inline int overlayEffectiveMode() {
    // Experimental gate: overlay ships dark (including the measure-switch
    // force-on) until [experimental] dev_features is set.
    if (!jumperlessConfig.experimental.dev_features) {
        return OVERLAY_MODE_OFF;
    }
    int mode = jumperlessConfig.display.measurement_overlay;
    if (mode == OVERLAY_MODE_OFF && switchPosition == 0) {
        mode = OVERLAY_MODE_COLOR;
    }
    return mode;
}

// ----------------------------------------------------------------------------
// Core 2: voltage sweep slice
// ----------------------------------------------------------------------------
// Called from core2stuff() while core_sync is held. Self-throttled to one
// slice per ~4ms, up to 5 rows per slice -> full 60-row sweep in ~50ms.
// (Was 8ms/4 rows = ~120ms. Crossbar GND discharge adds ~2 strobes + 20us
// per un-netted row; 4ms/5 still ~50ms. Vr prints slice/sweep timing.)
// Always fully disconnects before returning.

#define OVERLAY_SLICE_INTERVAL_US 4000
#define OVERLAY_ROWS_PER_SLICE 5
// A value jump bigger than this needs two consecutive sweeps to agree
// before it's published (kills single-read glitches)
#define OVERLAY_JUMP_V 0.5f
// Hold off sweeping this long after any routing change (transients settle,
// LEDs hold their last values meanwhile)
#define OVERLAY_ROUTE_SETTLE_MS 100

// Measured sweep timing, printed by printRowSweep() (Vr / :overlay:rows)
static volatile uint32_t overlaySliceUsEma = 0;   // EMA of slice CPU cost
static volatile uint32_t overlaySweepMs = 0;      // last full 60-row pass

void __not_in_flash_func(measurementOverlayCore2Tick)() {
    MeasurementOverlay& mo = MeasurementOverlay::getInstance();

    if (overlayEffectiveMode() == OVERLAY_MODE_OFF) return;
    // Defer to everything else: flash writes, routing in flight, probe
    // scanning the crossbar, wavegen streaming (per plan; sweep itself is
    // I2C-free but stays out of the way), logic analyzer (loop1 already
    // skips core2stuff while LA runs).
    if (pauseCore2 || sendAllPathsCore2 != 0 || probeActive != 0) return;
    if (wavegen.isRunning()) return;

    static unsigned long lastSliceUs = 0;
    unsigned long nowUs = micros();
    if (nowUs - lastSliceUs < OVERLAY_SLICE_INTERVAL_US) return;
    lastSliceUs = nowUs;

    // Value seen once that jumped too far from the published one; must repeat
    // next sweep before it shows. NAN = nothing pending.
    static float pendingV[61];
    static bool pendingInit = false;
    if (!pendingInit) {
        for (int i = 0; i <= 60; i++) pendingV[i] = NAN;
        pendingInit = true;
    }

    // Routing changed -> invalidate path caches and rebuild row->net map
    static uint32_t seenGen = 0xFFFFFFFF;
    static unsigned long genChangeMs = 0;
    if (seenGen != crossbarPathGeneration) {
        seenGen = crossbarPathGeneration;
        genChangeMs = millis();
        for (int i = 0; i <= 60; i++) {
            rowPathCache[i].resolved = false;
            pendingV[i] = NAN;
        }
        rebuildRowNets(mo);
    }
    // Hold last values until post-routing transients die down
    if (millis() - genChangeMs < OVERLAY_ROUTE_SETTLE_MS) return;

    int adc = pickFreeAdc();
    if (adc < 0) return;  // all ADCs busy - hold last values

    unsigned long sliceStartUs = micros();

    static int nextRow = 1;
    static uint8_t sweepCount = 0;
    int measured = 0;
    for (int tries = 0; tries < 60 && measured < OVERLAY_ROWS_PER_SLICE; tries++) {
        int row = nextRow;
        nextRow = (nextRow % 60) + 1;
        if (nextRow == 1) {
            // Wrapped: one full pass over all 60 rows completed
            static unsigned long lastWrapMs = 0;
            unsigned long nowMs = millis();
            if (lastWrapMs != 0) overlaySweepMs = nowMs - lastWrapMs;
            lastWrapMs = nowMs;
            sweepCount++;
        }
        if (!mo.rowInSet(row)) continue;

        RowAdcPath* p = getRowPath(mo, row);
        bool ok = false;
        float v = 0.0f;
        bool unNetted = (mo.rowNet[row] <= 0);
        if (p != nullptr && p->kind != ROWPATH_INVALID) {
            // Un-netted rows: pre-discharge the lane (inherited charge) and
            // phantom-recheck live-looking readings (leakage-charged rows).
            // A value that already survived a recheck skips the pulse - but
            // re-confirm every 32nd sweep (~1.6s) or a row whose source was
            // removed would hold its leakage-refreshed voltage forever.
            // (Hardware-observed: a hard-wired 3.3V row eats ~33mA for 20us
            // per pulse, so keep the repeat rate low.)
            bool reconfirm = (sweepCount & 31) == 0;
            float skipV = (!reconfirm && mo.rowStatus[row] == OVERLAY_ROW_FRESH)
                              ? mo.rowVolts[row] : NAN;
            v = readRowVoltageViaPath(*p, adc, 2, ok, unNetted, unNetted, skipV);
        }
        if (ok && (!isfinite(v) || fabsf(v) > 10.0f)) ok = false;  // absurd - drop
        if (!ok) {
            if (mo.rowStatus[row] == OVERLAY_ROW_FRESH) {
                mo.rowStatus[row] = OVERLAY_ROW_STALE;  // hold last value, dimmed
            }
            continue;
        }

        // Big jump from the published value: hold the display and require the
        // next sweep to agree (within OVERLAY_JUMP_V) before publishing.
        // Single-read glitches never render; real changes lag one sweep.
        uint8_t st = mo.rowStatus[row];
        if (st != OVERLAY_ROW_NONE && fabsf(v - mo.rowVolts[row]) > OVERLAY_JUMP_V) {
            if (isnan(pendingV[row]) || fabsf(v - pendingV[row]) > OVERLAY_JUMP_V) {
                pendingV[row] = v;
                measured++;
                continue;
            }
        }
        pendingV[row] = NAN;
        // Within noise of what's already shown: leave the display alone
        if (st == OVERLAY_ROW_FRESH && fabsf(v - mo.rowVolts[row]) < 0.05f) {
            measured++;
            continue;
        }
        mo.rowVolts[row] = v;
        mo.rowStatus[row] = (unNetted && fabsf(v) < OVERLAY_FLOATING_DEADBAND_V)
                                ? OVERLAY_ROW_FLOATING
                                : OVERLAY_ROW_FRESH;
        measured++;
    }

    uint32_t sliceUs = (uint32_t)(micros() - sliceStartUs);
    overlaySliceUsEma = overlaySliceUsEma ? (overlaySliceUsEma * 7 + sliceUs) / 8 : sliceUs;

    __dmb();  // publish sweep results to core 0 readers
}

// ----------------------------------------------------------------------------
// Core 2: LED paint
// ----------------------------------------------------------------------------
// Called after showNets()/showAllRowAnimations() so the overlay owns its LEDs
// for this frame.
//
// Physical LED order (verified against printGraphicsRow, which renders text
// unflipped on both sections): base+0 is the TOP of every 5-LED column for
// ALL 60 rows. So the center-channel LED is base+4 on the top section and
// base+0 on the bottom section.
//
// Positive bars grow from the center channel outward; negative bars grow from
// the rail side inward (mirrored), with the negative hue measurementToColor()
// already applies. Highlighted nets get brightened instead of skipped.

// From Graphics.cpp (externs here instead of including Graphics.h, whose
// header-static CurrentSenseOverlayState would cost this TU ~3.8KB of RAM):
// rows the marching-ants virtual wire occupies this frame, and the net the
// wire renderer painted on each row LED this frame (0 = untouched).
extern bool currentSenseVirtualRow[61];
extern int wireStatus[64][5];

void __not_in_flash_func(measurementOverlayPaint)() {
    MeasurementOverlay& mo = MeasurementOverlay::getInstance();

    int mode = overlayEffectiveMode();

    // Pixels the overlay painted last frame (netted rows included). Clearing
    // exactly these - instead of whole 5-LED columns - keeps row animations,
    // wires and ants intact around the overlay. Pixels the wire renderer
    // repainted this frame (wireStatus non-zero) are skipped: blacking them
    // out would punch one-frame holes in the wire/ant rendering.
    static uint8_t paintedMask[61] = { 0 };
    auto clearMaskedPixels = [&](int row) {
        int base = (row - 1) * 5;
        for (int k = 0; k < 5; k++) {
            if ((paintedMask[row] & (1 << k)) && wireStatus[row][k] == 0) {
                leds.setPixelColor(base + k, 0);
            }
        }
        paintedMask[row] = 0;
    };

    // On overlay turn-off (e.g. switch leaves measure position), clear what
    // we painted once - no other renderer repaints those LEDs.
    static int lastMode = OVERLAY_MODE_OFF;
    if (mode == OVERLAY_MODE_OFF) {
        if (lastMode != OVERLAY_MODE_OFF) {
            for (int row = 1; row <= 60; row++) {
                clearMaskedPixels(row);
                mo.rowStatus[row] = OVERLAY_ROW_NONE;
            }
        }
        lastMode = OVERLAY_MODE_OFF;
        return;
    }
    lastMode = mode;

    for (int row = 1; row <= 60; row++) {
        int base = (row - 1) * 5;
        uint8_t st = mo.rowStatus[row];

        // Clear last frame's pixels; repainted ones are just overwritten below.
        if (paintedMask[row]) {
            clearMaskedPixels(row);
        }

        // Rows the marching-ants virtual wire occupies belong to the ants
        // this frame - painting voltage here would stomp the ant bridge.
        if (currentSenseVirtualRow[row]) continue;

        if (!mo.rowInSet(row)) continue;
        if (st == OVERLAY_ROW_NONE || st == OVERLAY_ROW_FLOATING) continue;  // dark

        int net = mo.rowNet[row];
        uint8_t newMask = 0;

        float v = mo.rowVolts[row];
        uint32_t color = measurementToColor(v, -8.0f, 8.0f);

        int bright = 0;
        if (st == OVERLAY_ROW_STALE) bright -= 40;
        if (net > 0 && (net == brightenedNet || net == highlightedNet)) {
            bright += 70;  // active highlight still shows the overlay, brighter
        }

        bool top = (row <= 30);
        int centerOffset = top ? 4 : 0;
        int railOffset = top ? 0 : 4;
        int dirFromCenter = top ? -1 : 1;

        if (mode == OVERLAY_MODE_COLOR) {
            leds.setPixelColor(base + centerOffset, scaleBrightness(color, bright));
            newMask = (uint8_t)(1 << centerOffset);
        } else {
            // Bargraph / dot: 1V per LED on magnitude (rounded to nearest
            // volt), sign sets fill direction. Rounding means idle-net noise
            // (hardware-observed up to ~0.45V) stays a steady presence dot
            // instead of flickering LED 1 on and off.
            float mag = fabsf(v);
            int n = (int)roundf(mag);
            if (n < 0) n = 0;
            if (n > 5) n = 5;
            if (n == 0) {
                // Presence dot so a selected 0V row is distinguishable from off
                leds.setPixelColor(base + centerOffset, scaleBrightness(color, bright - 60));
                newMask = (uint8_t)(1 << centerOffset);
            } else {
                bool negative = (v < 0.0f);
                for (int i = 0; i < n; i++) {
                    int offset;
                    if (!negative) {
                        offset = centerOffset + dirFromCenter * i;  // center -> rail
                    } else {
                        offset = railOffset - dirFromCenter * i;    // rail -> center
                    }
                    bool tip = (i == n - 1);
                    if (mode == OVERLAY_MODE_DOT && !tip) continue;
                    // On netted rows a full bar obliterates the wire rendering;
                    // overdraw only the tip + center anchor and leave the rest
                    // of the wire visible. Un-netted rows get the full bar.
                    if (mode == OVERLAY_MODE_BAR && net > 0 &&
                        !tip && offset != centerOffset) {
                        continue;
                    }
                    // Tip pops, fill is dimmer; >5V tip gets an extra kick like
                    // the rails' danger dot (measurementToColor already
                    // desaturates >5.4V)
                    int ledBright = bright + (tip ? (mag > 5.0f ? 80 : 40) : -25);
                    leds.setPixelColor(base + offset, scaleBrightness(color, ledBright));
                    newMask |= (uint8_t)(1 << offset);
                }
            }
        }

        // Track everything we painted - netted rows included. The wire
        // renderer only repaints pixels on the wire route (wireStatus != 0),
        // so an overlay pixel off the route (e.g. a bar tip after the bar
        // shrinks) would otherwise linger forever. clearMaskedPixels()
        // skips the wire-covered ones.
        paintedMask[row] = newMask;
    }
}

// ----------------------------------------------------------------------------
// Core 0: MeasurementOverlay service
// ----------------------------------------------------------------------------

void MeasurementOverlay::handleRowSetButtons() {
    // While the probe switch is in measure position and a row is being
    // measured: CONNECT adds it to the overlay row set, REMOVE removes it -
    // unless the row is a current tap, in which case REMOVE kills the tap
    // (the bridge goes back to a plain wire). Consuming the press here
    // (we're registered ahead of Probing) keeps it from triggering probe
    // connect/clear.
    if (switchPosition != 0 || !measureModeService.isMeasurementActive()) return;
    int node = measureModeService.getMeasuredNode();
    if (node < 1 || node > 60) return;

    int press = ProbeButton::getInstance().getButtonPress(true);
    if (press == 2) {
        addRow(node);
        Serial.printf("\n\roverlay + row %d\n\r", node);
    } else if (press == 1) {
        if (tapForNode(node) != nullptr) {
            removeCurrentTap(node);
            Serial.printf("\n\rcurrent tap on row %d removed\n\r", node);
        } else {
            removeRow(node);
            Serial.printf("\n\roverlay - row %d%s\n\r", node,
                          (rowMask == 0) ? " (empty set: showing all rows)" : "");
        }
    }
}

// ----------------------------------------------------------------------------
// Current taps: user-designated node pairs measured in series.
//
// The BRIDGE ARRAY is the source of truth. Inserting a tap swaps the pair's
// bridge for two EPHEMERAL bridges (ISENSE_PLUS->A, ISENSE_MINUS->B) and
// lets the normal router do the wiring - so the paths dump always shows the
// real crossbar state, and the whole existing manual-ISENSE stack drives
// itself: rebuildShownReadings() detects the nets, Peripherals polls INA0
// continuously into currentSenseState, and the marching-ants overlay + OLED
// current readouts light up with zero special-casing. Every swap step is
// make-before-break across two reroutes, so the circuit never opens.
//
// Ephemeral bridges never serialize; the DISPLACED user bridge must (a save
// mid-tap would otherwise lose the wire) - serializeBridges() pulls it from
// overlayTapDisplacedBridge(). The displace/restore netlist edits are
// "quiet": no markDirty, no undo records.
//
// ponytail: a physically-jumpered pair (real wire in the breadboard)
// bypasses the shunt and reads low; that's inherent (the 17-28 LED session).
// ----------------------------------------------------------------------------

// The user bridge currently swapped out for the shunt (only ever one: only
// one tap is inserted at a time).
static struct {
    bool valid = false;
    int n1 = -1, n2 = -1, dup = 0;
    uint32_t color = 0xFFFFFFFF;
} displacedBridge;

// Any ISENSE bridge that ISN'T the active tap's ephemeral pair means the
// user hand-wired the current sense - it owns INA0, mux backs off.
static bool manualIsenseWiring(const MeasurementOverlay::CurrentTap* active) {
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        int n1 = globalState.connections.bridges[i][0];
        int n2 = globalState.connections.bridges[i][1];
        if (n1 != ISENSE_PLUS && n2 != ISENSE_PLUS &&
            n1 != ISENSE_MINUS && n2 != ISENSE_MINUS) continue;
        if (active != nullptr) {
            if ((n1 == ISENSE_PLUS && n2 == active->nodeA) ||
                (n2 == ISENSE_PLUS && n1 == active->nodeA)) continue;
            if ((n1 == ISENSE_MINUS && n2 == active->nodeB) ||
                (n2 == ISENSE_MINUS && n1 == active->nodeB)) continue;
        }
        return true;
    }
    return false;
}

bool MeasurementOverlay::addCurrentTap(int nodeA, int nodeB) {
    // Experimental gate: don't register taps that serviceCurrentCycling()
    // would never insert (they'd sit at "holding" forever).
    if (!jumperlessConfig.experimental.dev_features) return false;
    if (!globalState.hasConnection(nodeA, nodeB)) return false;  // not a bridge
    // Ephemeral bridges (measure-mode ADC, our own ISENSE) aren't tappable
    if (globalState.isEphemeralConnection(nodeA, nodeB)) return false;

    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (taps[i].valid &&
            ((taps[i].nodeA == nodeA && taps[i].nodeB == nodeB) ||
             (taps[i].nodeA == nodeB && taps[i].nodeB == nodeA))) {
            return true;  // already tapped
        }
    }
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (!taps[i].valid) {
            taps[i].nodeA = (int16_t)nodeA;
            taps[i].nodeB = (int16_t)nodeB;
            taps[i].ma = NAN;
            taps[i].lastMs = 0;
            taps[i].valid = true;
            return true;
        }
    }
    return false;  // all slots full
}

bool MeasurementOverlay::addCurrentTapForNode(int node) {
    // First bridge touching this node becomes the tap; the tapped node gets
    // the ISENSE+ side so positive mA = current flowing INTO it. Skip
    // ISENSE bridges (manual wiring or our own) and ephemerals (measure
    // mode's own ADC bridge is on this node RIGHT NOW).
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        int n1 = globalState.connections.bridges[i][0];
        int n2 = globalState.connections.bridges[i][1];
        if (n1 != node && n2 != node) continue;
        if (n1 == n2) continue;
        int partner = (n1 == node) ? n2 : n1;
        if (partner == ISENSE_PLUS || partner == ISENSE_MINUS) continue;
        if (globalState.isEphemeralConnection(n1, n2)) continue;
        if (addCurrentTap(node, partner)) return true;
    }
    return false;
}

void MeasurementOverlay::claimTapNets(uint8_t* netDecor, uint8_t antsValue) const {
    if (!jumperlessConfig.experimental.dev_features ||
        !jumperlessConfig.display.current_cycling) {
        return;
    }
    extern int findNodeInNet(int node);
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (!taps[i].valid) continue;
        int nodes[2] = { taps[i].nodeA, taps[i].nodeB };
        for (int j = 0; j < 2; j++) {
            int net = findNodeInNet(nodes[j]);
            if (net > 0 && net < MAX_NETS) netDecor[net] = antsValue;
        }
    }
}

bool MeasurementOverlay::removeCurrentTap(int node) {
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (!taps[i].valid) continue;
        if (taps[i].nodeA == node || taps[i].nodeB == node) {
            if (i == activeTap) {
                restoreTap(i);
                activeTap = -1;
            }
            taps[i].valid = false;
            return true;
        }
    }
    return false;
}

void MeasurementOverlay::clearCurrentTaps() {
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (taps[i].valid && taps[i].nodeA >= 0) removeCurrentTap(taps[i].nodeA);
    }
}

int MeasurementOverlay::tapCount() const {
    int n = 0;
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (taps[i].valid) n++;
    }
    return n;
}

const MeasurementOverlay::CurrentTap* MeasurementOverlay::tapForNode(int node) const {
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (taps[i].valid && (taps[i].nodeA == node || taps[i].nodeB == node)) {
            return &taps[i];
        }
    }
    return nullptr;
}

// Exported for States.cpp serializeBridges(): the displaced bridge must
// still serialize or saving mid-tap would lose the user's wire.
bool overlayTapDisplacedBridge(int* n1, int* n2, int* dup, uint32_t* color) {
    if (!displacedBridge.valid) return false;
    *n1 = displacedBridge.n1;
    *n2 = displacedBridge.n2;
    *dup = displacedBridge.dup;
    *color = displacedBridge.color;
    return true;
}

extern volatile bool refreshLocalInProgress;  // Commands.cpp

// Swap the tap's bridge for ephemeral ISENSE+/- bridges and reroute once.
// Core 0 only (refreshLocalConnections requirement). reroute=false leaves
// the netlist edited but unrouted so a rotation can batch restore+insert
// into a single reroute.
bool MeasurementOverlay::insertTap(int idx, bool reroute) {
    const CurrentTap& t = taps[idx];
    if (!globalState.hasConnection(t.nodeA, t.nodeB)) return false;
    if (globalState.isEphemeralConnection(t.nodeA, t.nodeB)) return false;

    // Capture what we're displacing so restore/serialize can reproduce it
    int dup = globalState.getConnectionDuplicates(t.nodeA, t.nodeB);
    uint32_t color = 0xFFFFFFFF;
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        if ((globalState.connections.bridges[i][0] == t.nodeA &&
             globalState.connections.bridges[i][1] == t.nodeB) ||
            (globalState.connections.bridges[i][0] == t.nodeB &&
             globalState.connections.bridges[i][1] == t.nodeA)) {
            color = globalState.connections.bridgeColors[i];
            break;
        }
    }

    String err;
    if (!globalState.addEphemeralConnection(ISENSE_PLUS, t.nodeA, err, false, 0)) {
        return false;
    }
    if (!globalState.addEphemeralConnection(ISENSE_MINUS, t.nodeB, err, false, 0)) {
        globalState.removeEphemeralConnection(ISENSE_PLUS, t.nodeA, err, false, 0);
        return false;
    }
    if (!globalState.removeConnection(t.nodeA, t.nodeB, err, true)) {
        globalState.removeEphemeralConnection(ISENSE_MINUS, t.nodeB, err, false, 0);
        globalState.removeEphemeralConnection(ISENSE_PLUS, t.nodeA, err, false, 0);
        return false;
    }
    displacedBridge.valid = true;
    displacedBridge.n1 = t.nodeA;
    displacedBridge.n2 = t.nodeB;
    displacedBridge.dup = dup;
    displacedBridge.color = color;

    if (reroute) {
        refreshLocalConnections(1, 1, 0);  // one reroute for the whole swap
        waitCore2();
    }
    tapInsertedMs = millis();
    return true;
}

// Put the user's bridge back, drop the ISENSE bridges, reroute once.
void MeasurementOverlay::restoreTap(int idx, bool reroute) {
    const CurrentTap& t = taps[idx];
    String err;
    globalState.removeEphemeralConnection(ISENSE_PLUS, t.nodeA, err, false, 0);
    globalState.removeEphemeralConnection(ISENSE_MINUS, t.nodeB, err, false, 0);
    if (displacedBridge.valid) {
        globalState.addConnection(displacedBridge.n1, displacedBridge.n2, err,
                                  displacedBridge.dup, true);
        if (displacedBridge.color != 0xFFFFFFFF) {
            for (int i = 0; i < globalState.connections.numBridges; i++) {
                if ((globalState.connections.bridges[i][0] == displacedBridge.n1 &&
                     globalState.connections.bridges[i][1] == displacedBridge.n2) ||
                    (globalState.connections.bridges[i][0] == displacedBridge.n2 &&
                     globalState.connections.bridges[i][1] == displacedBridge.n1)) {
                    globalState.connections.bridgeColors[i] = displacedBridge.color;
                    break;
                }
            }
        }
        displacedBridge.valid = false;
    }
    if (reroute) {
        refreshLocalConnections(1, 1, 0);
        waitCore2();
    }
}

// The routed ISENSE bridges make rebuildShownReadings() detect plus/minus
// nets and Peripherals polls INA0 into currentSenseState continuously (the
// exact manual-wiring machinery) - just copy the freshest reading over.
// Readings from before this tap's insert belong to the PREVIOUS tap (at
// fast dwells the 50ms poll can lag a swap) - drop those.
void MeasurementOverlay::readActiveTap() {
    if (!currentSenseState.active) return;
    if (currentSenseState.lastUpdatedMs < tapInsertedMs) return;
    taps[activeTap].ma = currentSenseState.current_mA;
    taps[activeTap].lastMs = currentSenseState.lastUpdatedMs;
}

void MeasurementOverlay::serviceCurrentCycling() {
    // All crossbar work goes through the normal netlist reroute machinery;
    // just stay out of the way of anything that owns the router right now.
    if (wavegen.isRunning()) return;
    if (probeActive != 0 || pauseCore2 || sendAllPathsCore2 != 0) return;
    if (refreshLocalInProgress) return;
    if (logicAnalyzer.is_running() || logicAnalyzer.is_armed()) return;

    unsigned long now = millis();

    // External netlist wipe (slot load, nodes_clear, undo) took our
    // ephemeral ISENSE bridges with it - the displaced bridge came back (or
    // was deliberately dropped) with that state; nothing to restore.
    if (activeTap >= 0 &&
        (!globalState.hasConnection(ISENSE_PLUS, taps[activeTap].nodeA) ||
         !globalState.hasConnection(ISENSE_MINUS, taps[activeTap].nodeB))) {
        displacedBridge.valid = false;
        taps[activeTap].valid = false;
        activeTap = -1;
    }

    // Drop inactive taps whose bridge disappeared (user unplugged the wire)
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        if (!taps[i].valid || i == activeTap) continue;
        if (!globalState.hasConnection(taps[i].nodeA, taps[i].nodeB)) {
            taps[i].valid = false;
        }
    }

    bool enabled = (jumperlessConfig.experimental.dev_features != 0) &&
                   (jumperlessConfig.display.current_cycling != 0) &&
                   !manualIsenseWiring(activeTapInfo());
    if (!enabled) {
        if (activeTap >= 0) {
            restoreTap(activeTap);
            activeTap = -1;
        }
        return;
    }

    int nTaps = tapCount();
    if (nTaps == 0) return;

    // Rotate: only when >1 tap wants the shunt (a lone tap stays inserted
    // permanently - zero disturbance, true continuous measurement). The
    // restore + insert netlist edits are batched into ONE incremental
    // reroute. Failed inserts also wait a dwell before retrying (no
    // reroute storms).
    if (activeTap >= 0 && nTaps > 1 && now - tapDwellStartMs >= TAP_DWELL_MS) {
        int next = activeTap;
        for (int step = 1; step <= MAX_CURRENT_TAPS; step++) {
            int i = (activeTap + step) % MAX_CURRENT_TAPS;
            if (taps[i].valid) {
                next = i;
                break;
            }
        }
        restoreTap(activeTap, false);
        activeTap = -1;
        tapDwellStartMs = now;
        if (insertTap(next, false)) activeTap = next;
        // One reroute covers the batched restore+insert (or just the
        // restore if the insert failed)
        refreshLocalConnections(1, 1, 0);
        waitCore2();
        tapInsertedMs = millis();
    } else if (activeTap < 0 && now - tapDwellStartMs >= TAP_DWELL_MS) {
        tapDwellStartMs = now;
        for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
            if (taps[i].valid && insertTap(i)) {
                activeTap = i;
                break;
            }
        }
    }

    if (activeTap >= 0) readActiveTap();
}

ServiceStatus MeasurementOverlay::service() {
    lastStatus = ServiceStatus::IDLE;
    handleRowSetButtons();
    serviceCurrentCycling();
    return lastStatus;
}

// ----------------------------------------------------------------------------
// Terminal output
// ----------------------------------------------------------------------------

// Vr: dump the sweep's view of every row (status, voltage, resolved path)
void MeasurementOverlay::printRowSweep(Stream* out) {
    static const char* statusNames[] = { "none", "ok", "stale", "float" };
    static const char* kindNames[] = { "-", "direct", "tap", "via" };
    out->printf("\n\rslice ~%luus (%d rows / %dms), full sweep %lums\n\r",
                (unsigned long)overlaySliceUsEma, OVERLAY_ROWS_PER_SLICE,
                OVERLAY_SLICE_INTERVAL_US / 1000, (unsigned long)overlaySweepMs);
    out->println("\n\rrow   volts   status  net  path");
    for (int row = 1; row <= 60; row++) {
        if (!rowInSet(row)) continue;
        uint8_t st = rowStatus[row];
        const RowAdcPath& p = rowPathCache[row];
        out->printf(" %2d  %7.3f  %-6s  %3d  %s", row, rowVolts[row],
                    statusNames[st <= 3 ? st : 0], rowNet[row],
                    kindNames[(p.resolved && p.kind <= 3) ? p.kind : 0]);
        if (p.resolved && p.kind == ROWPATH_DIRECT) {
            out->printf(" chip %c x%d y%d -> K y%d", 'A' + p.bbChip, p.bbX, p.bbY, p.kY);
        } else if (p.resolved && p.kind == ROWPATH_NET_TAP) {
            out->printf(" K y%d", p.kY);
        } else if (p.resolved && p.kind == ROWPATH_IJ) {
            out->printf(" chip %c x%d y%d -> %c -> K x%d bounce y%d", 'A' + p.bbChip,
                        p.bbX, p.bbY, (p.aggChip == CHIP_J) ? 'J' : 'I', p.aggKx, p.kY);
        }
        out->println();
    }
    out->flush();
}

void MeasurementOverlay::printCurrentComparison(Stream* out) {
    out->println("\n\rcurrent taps: INA0 in series, multiplexed (positive mA flows A -> B)");
    uint64_t mask = rowMask;
    out->printf("overlay mode %d (effective %d), current cycling %s, row set 0x%08lx%08lx%s\n\r",
                jumperlessConfig.display.measurement_overlay, overlayEffectiveMode(),
                jumperlessConfig.display.current_cycling ? "on" : "off",
                (unsigned long)(mask >> 32), (unsigned long)(mask & 0xFFFFFFFF),
                mask == 0 ? " (all rows)" : "");
    out->println("\n\r tap  A - B       mA      age ms   state");
    bool any = false;
    unsigned long now = millis();
    for (int i = 0; i < MAX_CURRENT_TAPS; i++) {
        const CurrentTap& t = taps[i];
        if (!t.valid) continue;
        any = true;
        out->printf("  %d  %3d - %-3d ", i, t.nodeA, t.nodeB);
        if (isnan(t.ma)) out->print("      -  ");
        else out->printf(" %8.3f ", t.ma);
        if (t.lastMs == 0) out->print("       - ");
        else out->printf(" %8lu ", now - t.lastMs);
        out->printf("  %s\n\r", (i == activeTap) ? "MEASURING" : "holding");
    }
    if (!any) {
        out->println(" (no taps - probe a bridged row in measure mode, or V+<a>-<b>)");
    }
    if (manualIsenseWiring(activeTapInfo())) {
        out->println(" NOTE: manual ISENSE wiring owns INA0 - taps paused until it's removed");
    }
    out->flush();
}

// Runnable self-check (ponytail): verify the deterministic lane tables against
// the live chipStates xMap/yMap the router actually uses. Fails loudly if the
// hardcoded tables ever drift from MatrixState.cpp.
void MeasurementOverlay::runLaneSelfCheck(Stream* out) {
    int fails = 0;

    for (int c = 0; c < 8; c++) {
        const chipStatus& cs = globalState.connections.chipStates[c];
        if (cs.xMap[chipToKlaneX[c]] != CHIP_K) {
            out->printf("FAIL chip %c: xMap[%d]=%d != CHIP_K\n\r", 'A' + c,
                        chipToKlaneX[c], cs.xMap[chipToKlaneX[c]]);
            fails++;
        }
        if (cs.xMap[chipToIlaneX[c]] != CHIP_I) {
            out->printf("FAIL chip %c: xMap[%d]=%d != CHIP_I\n\r", 'A' + c,
                        chipToIlaneX[c], cs.xMap[chipToIlaneX[c]]);
            fails++;
        }
        if (cs.xMap[chipToJlaneX[c]] != CHIP_J) {
            out->printf("FAIL chip %c: xMap[%d]=%d != CHIP_J\n\r", 'A' + c,
                        chipToJlaneX[c], cs.xMap[chipToJlaneX[c]]);
            fails++;
        }
    }

    // Row -> (chip, Y) formula vs bbNodesToChip + chip yMap
    for (int row = 1; row <= 60; row++) {
        int8_t chip, y;
        if (!rowToChipY(row, chip, y)) continue;  // 29/30/59/60: net-tap only
        if (bbNodesToChip[row] != chip) {
            out->printf("FAIL row %d: bbNodesToChip=%d, formula=%d\n\r", row,
                        bbNodesToChip[row], chip);
            fails++;
        }
        // yMap stores the row number itself (TOP_1=1 ... BOTTOM_28=58)
        if (globalState.connections.chipStates[chip].yMap[y] != row) {
            out->printf("FAIL row %d: chip %c yMap[%d]=%d\n\r", row, 'A' + chip, y,
                        globalState.connections.chipStates[chip].yMap[y]);
            fails++;
        }
    }

    // ISENSE X positions on chips I/J
    if (globalState.connections.chipStates[CHIP_I].xMap[ISENSE_PLUS_X] != ISENSE_PLUS) {
        out->println("FAIL chip I: X11 != ISENSE_PLUS");
        fails++;
    }
    if (globalState.connections.chipStates[CHIP_J].xMap[ISENSE_MINUS_X] != ISENSE_MINUS) {
        out->println("FAIL chip J: X11 != ISENSE_MINUS");
        fails++;
    }

    // LED decoration arbiter: sense nets must be ANTS, never ANIM
    extern int netDecorSelfCheck(Stream* out);
    fails += netDecorSelfCheck(out);

    if (fails == 0) {
        out->println("lane self-check PASS (K/I/J lanes, row map, ISENSE)");
    } else {
        out->printf("lane self-check: %d FAILURES\n\r", fails);
    }
    out->flush();
}

#else  // OG_JUMPERLESS ------------------------------------------------------

// OG has 1 LED per row and a different crossbar (CHIP_L hub); the overlay is
// V5-only. Keep the API so shared code links.
void measurementOverlayCore2Tick() {}
void measurementOverlayPaint() {}
void MeasurementOverlay::handleRowSetButtons() {}
void MeasurementOverlay::serviceCurrentCycling() {}
bool MeasurementOverlay::insertTap(int, bool) { return false; }
void MeasurementOverlay::restoreTap(int, bool) {}
void MeasurementOverlay::readActiveTap() {}
bool MeasurementOverlay::addCurrentTap(int, int) { return false; }
bool MeasurementOverlay::addCurrentTapForNode(int) { return false; }
bool MeasurementOverlay::removeCurrentTap(int) { return false; }
void MeasurementOverlay::clearCurrentTaps() {}
int MeasurementOverlay::tapCount() const { return 0; }
const MeasurementOverlay::CurrentTap* MeasurementOverlay::tapForNode(int) const { return nullptr; }
void MeasurementOverlay::claimTapNets(uint8_t*, uint8_t) const {}
bool overlayTapDisplacedBridge(int*, int*, int*, uint32_t*) { return false; }
ServiceStatus MeasurementOverlay::service() { return lastStatus = ServiceStatus::IDLE; }
void MeasurementOverlay::printCurrentComparison(Stream* out) { out->println("overlay is V5-only"); }
void MeasurementOverlay::printRowSweep(Stream* out) { out->println("overlay is V5-only"); }
void MeasurementOverlay::runLaneSelfCheck(Stream* out) { out->println("overlay is V5-only"); }

#endif  // OG_JUMPERLESS
