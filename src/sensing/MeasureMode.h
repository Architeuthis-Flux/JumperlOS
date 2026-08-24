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

#endif // MEASUREMODE_H
