// SPDX-License-Identifier: MIT
#ifndef MEASUREMODE_H
#define MEASUREMODE_H

#include <Arduino.h>
#include "JumperlessDefines.h"
#include "JumperlOS.h"

// ============================================================================
// Service Precedence Control
// ============================================================================
// When the switch is in measure mode and a node has an existing connection,
// both MeasureMode and Highlighting want to handle it. This macro controls
// which service takes precedence.
//
// Set to 1: MeasureMode takes precedence (voltage measurement shown)
// Set to 0: Highlighting takes precedence (net highlighting shown)
#define MEASUREMODE_TAKES_PRECEDENCE 1

/**
 * @brief Measurement types for future expansion
 * 
 * Currently only VOLTAGE is implemented, but the architecture supports:
 * - Logic probe for digital signal analysis
 * - Current measurement via INA219
 * - Data scanning for protocol detection (UART, I2C)
 */
enum class MeasurementType {
    VOLTAGE,      // ADC voltage reading (implemented)
    LOGIC_PROBE,  // Future: Digital logic state (HIGH/LOW/FLOATING)
    CURRENT,      // Future: Current measurement via INA219
    DATA_SCAN     // Future: UART/I2C bus scanning
};

/**
 * @brief MeasureMode service - handles voltage measurement when switch is in measure position
 * 
 * When the probe switch is in measure mode (position 0), this service:
 * - Detects when user taps a breadboard node
 * - Creates an ephemeral (temporary, never-saved) ADC connection
 * - Continuously displays voltage on OLED and serial
 * - Optionally shows a mini oscilloscope waveform
 * 
 * Design notes:
 * - Uses ephemeral connections that bypass markDirty() and are never saved
 * - Validates that tapped nodes are real measurable nodes (ignores special pads)
 * - Extensible for future measurement types (logic probe, current, data scanning)
 */
class MeasureMode : public Service {
public:
    // Get singleton instance
    static MeasureMode& getInstance();
    
    // Prevent copying
    MeasureMode(const MeasureMode&) = delete;
    MeasureMode& operator=(const MeasureMode&) = delete;
    
    // Service interface
    ServiceStatus service() override;
    const char* getName() const override { return "MeasureMode"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    
    // Core measurement control
    void startMeasurement(int node);
    void stopMeasurement();
    bool isMeasurementActive() const { return measurementActive; }
    int getMeasuredNode() const { return measuredNode; }
    float getLastVoltage() const { return smoothedVoltage; }
    
    // Oscilloscope mode control
    void enableOscilloscope(bool enable);
    bool isOscopeEnabled() const { return oscopeEnabled; }
    void setOscopeTimebase(int timebaseMs);  // Time per screen (1-1000ms)
    int getOscopeTimebase() const { return oscopeTimebaseMs; }
    
    // Measurement type (for future expansion)
    void setMeasurementType(MeasurementType type);
    MeasurementType getMeasurementType() const { return currentType; }
    
    // Node validation
    static bool isValidMeasurableNode(int node);
    
private:
    MeasureMode();
    ~MeasureMode() = default;
    
    static MeasureMode* instance;
    
    // ========================================================================
    // Measurement State
    // ========================================================================
    bool measurementActive = false;
    int measuredNode = -1;
    int adcChannel = -1;           // ADC channel number (0-4)
    int adcDefine = -1;            // ADC define value (ADC0-ADC4)
    MeasurementType currentType = MeasurementType::VOLTAGE;
    
    // ========================================================================
    // Oscilloscope State
    // ========================================================================
    bool oscopeEnabled = false;
    static constexpr int OSCOPE_SAMPLES = 128;  // Match OLED width
    float oscopeSamples[OSCOPE_SAMPLES];
    int oscopeSampleIndex = 0;
    float oscopeMin = 0.0f;
    float oscopeMax = 3.3f;
    int oscopeTimebaseMs = 100;    // Time per screen in milliseconds
    unsigned long lastOscopeSampleTime = 0;
    
    // ========================================================================
    // Timing and Rate Limiting
    // ========================================================================
    unsigned long lastUpdateTime = 0;
    static constexpr int UPDATE_INTERVAL_MS = 100;        // Display update rate
    static constexpr int OSCOPE_UPDATE_INTERVAL_MS = 20;  // Faster for oscilloscope
    
    // ========================================================================
    // Voltage Smoothing
    // ========================================================================
    float smoothedVoltage = 0.0f;
    bool firstReading = true;
    static constexpr float SMOOTHING_FACTOR = 0.85f;  // 0.0-1.0, lower = smoother
    
    // ========================================================================
    // Switch Debouncing
    // ========================================================================
    int lastSwitchPosition = -1;
    unsigned long switchStableTime = 0;
    static constexpr int SWITCH_DEBOUNCE_MS = 300;
    static constexpr int PROBE_GUARD_MS = 15;  // Don't read too close to INA219
    
    // ========================================================================
    // Probe Stability Tracking
    // ========================================================================
    int lastProbeReading = -1;
    int stableReadingCount = 0;
    static constexpr int STABLE_READINGS_REQUIRED = 5;
    
    // ========================================================================
    // Helper Methods
    // ========================================================================
    int findUnusedADC();
    bool connectADCToNode(int node);
    void disconnectADC();
    
    // Display methods
    void updateVoltageDisplay();
    void updateOscopeDisplay();
    void drawOscopeGrid();
    void drawOscopeWaveform();
    void drawOscopeStatusBar();
    
    // Oscilloscope helpers
    float voltageToPixelY(float voltage);
    void autoRangeOscope();
    void sampleForOscope();
};

// Global reference to singleton for convenience
// NOTE: Named measureModeService to avoid conflict with Probing::measureMode() function
extern MeasureMode& measureModeService;

// ============================================================================
// Measurement Overlay (V5 only)
// ============================================================================
// Time-multiplexes free ADCs across breadboard rows to show a live voltage
// overlay on the center-channel row LEDs, plus user-designated CURRENT TAPS:
// row-to-row bridges whose direct crossbar path is swapped for the INA219
// shunt (in series, continuous measurement), with the one shunt multiplexed
// across all taps.
//
// Split across cores:
//  - Core 0: MeasurementOverlay service (config, row-set buttons via probe,
//    current-sense windows - all I2C stays on core 0 like every other INA use)
//  - Core 2: measurementOverlayCore2Tick() (crossbar voltage sweep) and
//    measurementOverlayPaint() (LED rendering), called from core2stuff()
//    while the core_sync mutex is held.

// Display modes (stored in jumperlessConfig.display.measurement_overlay)
#define OVERLAY_MODE_OFF   0
#define OVERLAY_MODE_COLOR 1  // center-channel LED colored by voltage
#define OVERLAY_MODE_BAR   2  // rail-style bargraph, 1V per LED
#define OVERLAY_MODE_DOT   3  // only the tip LED of where the bar would end

// Per-row sweep status
#define OVERLAY_ROW_NONE     0  // never measured
#define OVERLAY_ROW_FRESH    1  // measured this sweep
#define OVERLAY_ROW_STALE    2  // lanes congested this sweep - holding last value
#define OVERLAY_ROW_FLOATING 3  // not in a net + reading in deadband - render dark

class MeasurementOverlay : public Service {
public:
    static MeasurementOverlay& getInstance();
    MeasurementOverlay(const MeasurementOverlay&) = delete;
    MeasurementOverlay& operator=(const MeasurementOverlay&) = delete;

    // Service interface (core 0). HIGH priority but only does cheap
    // coordination: probe-button row-set edits and current-sense windows.
    ServiceStatus service() override;
    const char* getName() const override { return "MeasureOverlay"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }

    // ------------------------------------------------------------------
    // Row set (session-only). Empty mask = scan all 60 rows.
    // ------------------------------------------------------------------
    void addRow(int row);
    void removeRow(int row);
    void clearRowSet();
    bool rowInSet(int row) const;      // honors empty-mask = all-rows rule
    uint64_t getRowMask() const { return rowMask; }

    // ------------------------------------------------------------------
    // Current taps: user-designated node pairs measured in series.
    // The ACTIVE tap's bridge is displaced from the bridge array and
    // replaced with ephemeral ISENSE_PLUS->A / ISENSE_MINUS->B bridges,
    // routed by the normal router - the bridge array stays the source of
    // truth for what's physically on the crossbar, and the existing
    // currentSenseState machinery (Peripherals INA polling, marching-ants
    // overlay, OLED readouts) drives itself. Inactive taps keep their
    // normal bridge. A single tap stays inserted permanently. Added by
    // probing a bridged row in measure mode, or V+<a>-<b> in the terminal.
    // ------------------------------------------------------------------
    static constexpr int MAX_CURRENT_TAPS = 8;
    struct CurrentTap {
        int16_t nodeA = -1;        // ISENSE+ side (positive mA = A -> B)
        int16_t nodeB = -1;        // ISENSE- side
        float ma = NAN;            // last reading (NAN = not measured yet)
        unsigned long lastMs = 0;
        bool valid = false;
    };
    bool addCurrentTap(int nodeA, int nodeB);   // idempotent; needs a routed bridge
    bool addCurrentTapForNode(int node);        // tap the first bridge touching node
    bool removeCurrentTap(int node);            // remove any tap touching node
    void clearCurrentTaps();
    int tapCount() const;
    const CurrentTap* tapForNode(int node) const;
    // The tap currently wired through the shunt (nullptr if none)
    const CurrentTap* activeTapInfo() const {
        return (activeTap >= 0 && taps[activeTap].valid) ? &taps[activeTap] : nullptr;
    }

    // Mark every net touched by a registered tap (active or not) in the LED
    // decoration table so row animations don't flap between ANIM and ANTS
    // while the mux rotates. Called by arbitrateNetDecorations() each frame;
    // no-op when cycling is disabled.
    void claimTapNets(uint8_t* netDecor, uint8_t antsValue) const;

    void printCurrentComparison(Stream* out);   // terminal tap table
    void printRowSweep(Stream* out);            // per-row volts/status/path debug dump
    void runLaneSelfCheck(Stream* out);         // verify deterministic tables vs router

    // ------------------------------------------------------------------
    // Shared sweep state - written on core 2, read on both cores.
    // ------------------------------------------------------------------
    float rowVolts[61];              // index by row 1-60
    volatile uint8_t rowStatus[61];  // OVERLAY_ROW_*
    volatile int16_t rowNet[61];     // net number per row (-1 = none), rebuilt per generation

private:
    MeasurementOverlay();
    ~MeasurementOverlay() = default;
    static MeasurementOverlay* instance;

    volatile uint64_t rowMask = 0;   // bit (row-1) set = row explicitly selected

    CurrentTap taps[MAX_CURRENT_TAPS];
    int activeTap = -1;              // tap currently wired through ISENSE (-1 none)
    unsigned long tapDwellStartMs = 0;
    unsigned long tapInsertedMs = 0; // readings older than this belong to the previous tap
    // With >1 tap, how long each stays inserted before rotating. A rotation
    // is ONE incremental reroute (restore + insert edits batched); the INA
    // continuous poll runs every 50ms, so 400ms gives several fresh
    // readings per dwell.
    //
    // DON'T lower this much: a 150ms dwell was tried (2026-07-20) and the
    // reroute-per-150ms storm broke routing and display behavior in practice
    // (user-reported; each swap is a full incremental reroute that races
    // user edits, redraws the LEDs, and re-arms the voltage sweep's 100ms
    // route-settle hold). Faster multiplexing needs a design that doesn't
    // reroute per swap, not a smaller constant.
    static constexpr unsigned long TAP_DWELL_MS = 400;

    void handleRowSetButtons();
    void serviceCurrentCycling();
    // reroute=false batches the netlist edits for a combined swap (caller
    // must reroute afterwards - insertTap(_, true) or refreshLocalConnections)
    bool insertTap(int idx, bool reroute = true);
    void restoreTap(int idx, bool reroute = true);
    void readActiveTap();            // copy currentSenseState into taps[activeTap]

    friend void measurementOverlayCore2Tick();
    friend void measurementOverlayPaint();
};

// Core-2 entry points (no-ops when overlay + cycling are disabled)
void measurementOverlayCore2Tick();
void measurementOverlayPaint();

extern MeasurementOverlay& measurementOverlay;

#endif // MEASUREMODE_H
