/*!
 *  @file waveGen.h
 *
 *   Waveform Generator using MCP4728
 *  Provides high-performance, non-blocking waveform generation
 *
 *  T3.3 (C14 in CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md): the stream is
 *  hardware-owned - a pre-built I2C command image, three DMA channels and a
 *  DMA pacing timer feed the MCP4728 through I2C0's TX FIFO; no core runs a
 *  per-sample loop, no ISR per sample. Started / stopped / re-planned from
 *  core 0 (every caller is core 0: the MicroPython API, the menus); core 1's
 *  service() is a monitor only. The legacy per-sample CPU loop remains as
 *  the fallback if a DMA channel or timer cannot be claimed.
 */

#ifndef _WAVEGEN_H
#define _WAVEGEN_H

#include "Arduino.h"
#include "MCP4728.h"

// Waveform types
typedef enum {
    WAVEGEN_SINE,
    WAVEGEN_TRIANGLE,
    WAVEGEN_SAWTOOTH,
    WAVEGEN_SQUARE,
} waveGen_waveform_t;

// Channel selection
typedef enum {
    WAVEGEN_DAC0,
    WAVEGEN_DAC1,
    WAVEGEN_DAC2,
    WAVEGEN_DAC3,
} waveGen_channel_t;

// Frequency threshold for non-blocking mode (Hz) - LEGACY CPU PATH ONLY.
// Below this frequency, service() releases control to allow other tasks to run
#define WAVEGEN_NONBLOCKING_THRESHOLD_HZ 1000.0f

// Diagnostics for X (see WaveGen::getDmaStatus)
struct WaveGenDmaStatus {
    bool     dmaAvailable;   // channels + timer claimed (else the CPU loop is the path)
    uint8_t  fallbackReason; // 0 = ok, 1 = not I2C0/Wire, 2 = no DMA channels, 3 = no DMA timer, 4 = no memory, 5 = not built (OG)
    bool     streaming;      // DMA stream armed right now
    bool     wedged;         // the monitor saw no progress for > 200 ms while streaming
    uint32_t tableSize;      // N (power of two)
    uint32_t divider;        // D
    uint16_t timerX, timerY; // pacing timer fraction
    float    tickHz;         // sample rate = clk_sys * X / Y / D
    float    actualHz;       // tickHz / N
    float    capacityHz;     // bus capacity in samples/s (from the I2C0 SCL counts)
    uint32_t laps;           // waveform cycles counted by the monitor (ring wraps)
    uint32_t txAborts;       // TX_ABRT events seen (NAK etc.) - cleared and counted
    uint32_t starts;         // DMA stream starts
    uint32_t stops;
    uint32_t restarts;       // setter-triggered re-plans while running
    uint32_t stopWaitMaxUs;  // longest stop-time wait for the bus to go idle
    uint32_t wedgeRecoveries;// stops that had to abort a mid-burst A / push a partial sample
    uint32_t abortTimeouts;  // channel aborts that did not go quiet within 2 ms (never expected)
    uint32_t busYields;      // foreign I2C0 transactions the stream paused for (the arbiter)
    uint32_t busYieldMaxUs;  // longest pause wait (drain) for one of those
    uint32_t busForcedResumes; // pauses the monitor had to release after 100 ms (a nostop write with no partner)
};

/*!
 *    @brief  Class for  waveform generation
 */
class WaveGen {
public:
    WaveGen();
    ~WaveGen();  // Destructor to free dynamically allocated buffers

    // Configuration
    bool begin(uint8_t i2c_address = MCP4728_I2CADDR_DEFAULT, TwoWire *wire = &Wire);
    void end();

    // Waveform configuration
    void setChannel(waveGen_channel_t channel);
    void setWaveform(waveGen_waveform_t waveform);
    void setFrequency(float frequency_hz);
    void setAmplitude(float amplitude_v);
    void setOffset(float offset_v);
    // Query current configuration
    waveGen_channel_t getChannel() const { return _channel; }
    waveGen_waveform_t getWaveform() const { return _waveform; }
    float getFrequency() const { return _frequency_hz; }
    float getAmplitude() const { return _amplitude_v; }
    float getOffset() const { return _offset_v; }

    // Control
    bool start();
    void stop();
    bool isRunning() const { return _running; }
    // True only on the legacy fallback path, where the stream is a blocking
    // loop on core 1 (core 1 cannot serve anything meanwhile). On the DMA
    // path this is false while isRunning() is true - the bus is busy, core 1
    // is not. Commands.cpp's refresh wait keys on this, the bus gates key on
    // isRunning().
    bool isCoreLoopStreaming() const { return _running && !_dmaAvailable; }

    // Service function - call frequently from main loop (core 1). DMA path:
    // a monitor (progress, TX_ABRT, laps). Legacy path: the blocking loop.
    void service();

    // The I2C0 arbiter (T3.3): any foreign Wire transaction on I2C0 while the
    // DMA stream is live pauses the stream at a sample boundary (pacing timer
    // to 0, in-flight burst drained, bus idle) and resumes it afterwards, in
    // place - the linker-wrapped i2c_{write,read}_blocking_until in
    // I2C0Arbiter.cpp call these around every Wire master transaction, so an
    // API dac_set() or ina_get_current() during a stream reads/writes the
    // real thing (a ~50-100 us dip in the wave) instead of colliding on the
    // bus. Internal pollers (INA, OLED, scans) keep skipping on isRunning()
    // - a bus gate that costs the wave nothing. busPause() returns true if it
    // paused (then busResume() must follow).
    bool busPause();
    void busResume();

    // Statistics
    uint32_t getSuccessfulWrites() const { return _successful_writes; }
    uint32_t getFailedWrites() const { return _failed_writes; }
    float getActualFrequency() const { return _actual_frequency; }
    size_t getTableSize() const { return _table_size; }
    void getDmaStatus(WaveGenDmaStatus* out) const;

    // Frequency management
    float getAchievableFrequency(float desired_freq) const;
    float setFrequencyAdjusted(float frequency_hz);

    // Fallback mode for stability (legacy flag; the DMA path ignores it)
    void setFallbackMode(bool use_fallback) { _use_fallback = use_fallback; }
    bool isFallbackMode() const { return _use_fallback; }

    // Buffer mode information (legacy; kept for callers)
    size_t getBufferSize() const { return _buffer_size; }
    size_t getBufferCycles() const { return _buffer_cycles; }

    // Calibration (if needed)
    void setCalibrationOffset(waveGen_channel_t channel, float offset_v);
    void setCalibrationGain(waveGen_channel_t channel, float gain);

private:
    // Configuration
    MCP4728 _dac;
    volatile waveGen_channel_t _channel;
    volatile waveGen_waveform_t _waveform;
    volatile float _frequency_hz;
    volatile float _amplitude_v;
    volatile float _offset_v;

    // State
    volatile bool _running;
    volatile bool _initialized;
    bool _use_fallback;

    // Timing (legacy)
    uint32_t _last_sample_time;
    uint32_t _sample_interval_us;
    uint32_t _sample_count;

    // Waveform data (dynamically allocated to save RAM)
    static const size_t MAX_WAVEFORM_TABLE_SIZE = 512;   // DMA path: N is a power of two <= this (512 points per cycle is plenty for a 12-bit DAC over I2C; 1024 cost 4 KB more SRAM)
    volatile uint16_t* _waveform_table;  // MAX_WAVEFORM_TABLE_SIZE codes
    volatile size_t _table_size;        // Current table size (dynamic)
    volatile size_t _table_index;
    bool _waveform_table_allocated;     // Track allocation state
    // Sample repeat control for very low frequencies (legacy path)
    volatile uint32_t _repeat_factor;     // number of times to resend each sample
    volatile uint32_t _repeat_remaining;  // countdown for repeats before advancing index

    // Legacy buffer bookkeeping (stats only)
    size_t _buffer_size;
    size_t _buffer_cycles;
    volatile bool _params_changed;      // legacy loop: exit and rebuild on a setter

    // Statistics
    uint32_t _successful_writes;
    uint32_t _failed_writes;
    uint32_t _last_stats_time;
    float _actual_frequency;
    volatile uint32_t _stats_window_start_us;
    volatile uint32_t _samples_since_stats;
    volatile uint32_t _indices_since_stats;

    // Calibration
    float _calibration_offset[4];
    float _calibration_gain[4];

    // Internal functions
    void _buildWaveformTable();
    uint16_t _voltsToCode(float voltage, waveGen_channel_t channel);
    void _updateTableSize();
    void _updateSampleInterval();
    void _updateStatistics();
    void _serviceLegacy();

    // ---- T3.3: the DMA stream --------------------------------------------
    // Plan: N (power of two), D, X/Y for a requested frequency; the
    // achievable frequency is clk_sys * X / (Y * D * N).
    struct DmaPlan { uint32_t N; uint32_t D; uint16_t X, Y; float tickHz; float actualHz; bool ok; };
    DmaPlan _planRate(float frequency_hz) const;
    float _busCapacityHz() const;       // samples/s the I2C0 bus can carry
    void _dmaClaim();                   // once, from begin(): channels, timer, image
    void _dmaBuildImage();              // image + address ring from the table
    bool _dmaStart();                   // program I2C0 + DMA, start the pacing (core 0)
    void _dmaStop();                    // stop pacing, drain, release the bus (core 0)
    void _dmaRestartIfRunning();        // setters: re-plan and restart while running
    void _dmaMonitor();                 // service() on core 1: progress, TX_ABRT, laps

    bool     _dmaAvailable;
    uint8_t  _dmaFallbackReason;
    volatile bool _dmaStreaming;
    volatile bool _dmaWedged;
    int      _chA, _chB1, _chB2;        // A: image -> I2C0 TX; B1: pacing divider; B2: address ring -> A's read trigger
    int      _dmaTimer;
    uint16_t* _image;                   // 3 entries per sample (cmd, hi, lo|STOP)
    bool     _imageAllocated;
    DmaPlan  _plan;
    // monitor / stats
    uint32_t _lastRingAddr;
    uint32_t _lastProgressUs;
    uint32_t _laps;
    uint32_t _txAborts;
    uint32_t _dmaStarts, _dmaStops, _dmaRestarts, _stopWaitMaxUs, _wedgeRecoveries, _abortTimeouts;
    volatile bool _busPaused;           // arbiter: stream paused for a foreign transaction
    uint32_t _busYields, _busYieldMaxUs;
    uint32_t _busPauseStartUs, _busForcedResumes;
    int      _lockId;                   // SIO spinlock guarding start/stop/pause/resume across cores
    bool _drainForForeign(uint32_t budgetUs);   // timer to 0 + A idle + FIFO empty + bus idle
    uint32_t _dummySrc, _dummyDst;      // B1's no-op transfer endpoints
};

#endif
