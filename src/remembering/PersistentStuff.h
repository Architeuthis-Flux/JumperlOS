#pragma once

// Uncomment to use EEPROM storage instead of config file
//#define EEPROMSTUFF

#include "config.h"
#include <EEPROM.h>
#include "configManager.h"
#include "hardwarestuff/RoutableGpio.h"

#ifndef PERSSISTENTSTUFF_H
#define PERSSISTENTSTUFF_H

extern bool debugFP;
extern bool debugFPtime;

extern bool debugNM;
extern bool debugNMtime;

extern bool debugNTCC;
extern bool debugNTCC2;

extern bool debugLEDs;
extern bool debugMM;
extern bool debugWaitLoopTiming;
extern bool debugUSB;


extern bool calibrateOnStart;
extern bool firstStart;

extern const char firmwareVersion[16];

// =============================================================================
// EEPROM persistent store (survives a filesystem wipe)
// =============================================================================
// EEPROM is no longer the source of truth for runtime settings - config.txt is.
// The ONLY things that live in EEPROM now are the values that must survive an
// FS wipe / reflash: hardware identity, the last-run firmware version (for
// update sensing), and the full analog calibration. They're packed into one
// magic+version tagged struct so a single ~one-sector commit persists them all.
//
// Layout version. Bump whenever the field set / order changes so an older
// store is treated as "needs migration".
#define EEPROM_STORE_MAGIC 0x4A4C5331u  // "JLS1"
#define EEPROM_STORE_VERSION 1

struct EepromStore {
    uint32_t magic;    // EEPROM_STORE_MAGIC
    uint16_t version;  // EEPROM_STORE_VERSION
    uint16_t size;     // sizeof(EepromStore) - sanity guard

    // --- identity ---
    int32_t generation;
    int32_t revision;
    int32_t probe_revision;
    char    last_version[16];  // mirrors jumperlessConfig.firmware.last_version

    // --- calibration (mirror of the kept jumperlessConfig.calibration fields) ---
    int32_t top_rail_zero;     float top_rail_spread;
    int32_t bottom_rail_zero;  float bottom_rail_spread;
    int32_t dac_0_zero;        float dac_0_spread;
    int32_t dac_1_zero;        float dac_1_spread;
    float   adc_0_zero;        float adc_0_spread;
    float   adc_1_zero;        float adc_1_spread;
    float   adc_2_zero;        float adc_2_spread;
    float   adc_3_zero;        float adc_3_spread;
    float   adc_4_zero;        float adc_4_spread;
    float   adc_7_zero;        float adc_7_spread;
    int32_t probe_max;
    int32_t probe_min;
    float   probe_switch_threshold_high;
    float   probe_switch_threshold_low;
    float   measure_mode_output_voltage;
};

// Boot: read the store and apply hardware identity to jumperlessConfig. Called
// from loadHardwareFromEEPROM() BEFORE loadConfig(). Returns true if a valid
// store was found (so reconcile knows whether to seed one).
bool eepromStoreLoadAndApplyIdentity(void);

// Boot: called AFTER loadConfig(). If a valid store exists it wins for the
// kept fields (re-applied over whatever config.txt held, so they survive an FS
// wipe). If no valid store exists (legacy layout or fresh chip) it seeds one
// from the now-fully-loaded jumperlessConfig and marks it for deferred commit.
void eepromReconcileAfterConfig(void);

// Copy the kept jumperlessConfig fields into the in-RAM EEPROM buffer and mark
// it pending. Used by calibration save and identity / firmware-version updates.
// Does NOT touch flash - the deferred flush path (or eepromCommitSafe) does.
void eepromPersistFromConfig(void);

// Deferred-commit plumbing. eepromMarkDirty() just flags a pending commit;
// the actual EEPROM.commit() (a flash erase+program) happens later from the
// FileCache flush service's Core-1-parked + fs_mutex window.
void eepromMarkDirty(void);
bool eepromCommitPending(void);

// Commit the pending EEPROM buffer to flash. *Held* variant assumes the caller
// ALREADY holds pauseCore2ForFlash() + fs_mutex (FileCache coalesced path).
// *Safe* variant takes the whole envelope itself (boot / reboot-imminent
// paths that can't defer). Both no-op when nothing is pending. Return true if
// a commit actually happened.
bool eepromCommitHeld(void);
bool eepromCommitSafe(void);

void debugFlagSet(int flag);
void saveLEDbrightness(int forceDefaults = 0);

void saveDuplicateSettings(int forceDefaults = 0);
void saveVoltages(float top , float bot , float dac0 ,  float dac1 );
void readVoltages(void);
void saveDebugFlags(void);
void saveDacCalibration(void);

void saveLogoBindings(void);
void readLogoBindings(void);

void readSettingsFromConfig();


#endif