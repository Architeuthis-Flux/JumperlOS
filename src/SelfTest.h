// SPDX-License-Identifier: MIT
//
// Unattended factory hardware self test.
//
// Runs on first start (from the tail of calibrateDacs), from the backtick
// config commands `self_test / `self_test_report, or as individual clickwheel
// apps. Results are painted as a breadboard LED overlay (cleared on reset),
// printed as a sentinel-framed JSON line for host collection, and stored in
// /selftest.json.

#ifndef SELFTEST_H
#define SELFTEST_H

enum SelfTestStatus {
    SELFTEST_NOTRUN = 0,
    SELFTEST_PASS = 1,
    SELFTEST_FAIL = 2,
    SELFTEST_SKIP = 3,
};

#define SELFTEST_NUM_TESTS 5
// Test indices (order = LED overlay band order, left to right)
#define SELFTEST_PROBE_CABLE 0
#define SELFTEST_CROSSBAR 1
#define SELFTEST_TIP_VOLTAGE 2
#define SELFTEST_PSRAM 3
#define SELFTEST_PERIPHERALS 4

struct SelfTestReport {
    SelfTestStatus status[ SELFTEST_NUM_TESTS ];
    char detail[ SELFTEST_NUM_TESTS ][ 96 ];
};

// Run every test, paint the overlay, print human + machine reports, write
// /selftest.json. Failed tests are RE-RUN in rounds until everything passes
// (a report is printed per round; only the final one is stored) - any human
// input during the 5s countdown between rounds accepts the failing report
// instead. fromFirstStart additionally waits up to 10s for a serial host
// before starting (USB isn't open yet on a fresh first boot) and writes the
// one-shot marker so the report is re-printed after the ending restart.
// Manual full runs (fromFirstStart=false) end in the hold-then-reset below;
// the first-start path (calibrateDacs tail) holds for input, chains into the
// interactive probe pad calibration, wipes the undo history, then resets.
void runFullSelfTest( bool fromFirstStart );

// Hold the finished result on the breadboard LEDs until any human input
// (probe button, encoder click/turn, or a serial byte), then restart.
void selfTestWaitForInputThenReset( void );

// Same hold-for-input, but WITHOUT the restart - nextWhat names what the
// touch will do (e.g. "start probe pad calibration"). The first-start flow
// uses it to chain into the interactive pad calibration before resetting.
void selfTestWaitForInput( const char* nextWhat );

// Remove the self-test result overlay from the breadboard LEDs.
void selfTestClearOverlay( void );

// Individual test apps (registered in apps[] / clickwheel calibration menu)
void probeCableTestApp( void );
void crossbarTestApp( void );
void tipVoltageTestApp( void );
void psramTestApp( void );
void fullSelfTestApp( void );

// Re-print the stored /selftest.json report (sentinel-framed, for hosts)
void selfTestPrintStoredReport( void );

// Boot hook: if the one-shot marker exists (written by the first-start run),
// re-print the stored JSON report for hosts that missed it across the USB
// re-enumeration, then delete the marker. The overlay is NOT repainted: the
// operator already inspected and dismissed it during the hold-then-reset.
void selfTestShowSavedResultIfPending( void );

#endif // SELFTEST_H
