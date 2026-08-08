#include "PersistentStuff.h"
#include "FileParsing.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "NetManager.h"
#include "Probing.h"
#include "Peripherals.h"
#include <EEPROM.h>
#include "Graphics.h"
#include "configManager.h"
#include "config.h"
#include "States.h"
#include "ArduinoStuff.h"
#include "externVars.h"        // pauseCore2ForFlash / fs_mutex_* for eepromCommitSafe
#include "AsyncPassthrough.h"  // suspend/resumeUARTRxIRQ around the commit envelope

bool firstStart = false;

// =============================================================================
// EEPROM persistent store (identity + firmware version + calibration)
// =============================================================================
// config.txt is the source of truth for runtime settings. EEPROM holds ONLY
// the values that must survive an FS wipe, packed into one struct (EepromStore)
// and committed via the deferred FileCache flush path (or eepromCommitSafe on
// boot/reboot). debugFlagInit() and the per-setting EEPROM byte map are gone.

static EepromStore   g_store;
static bool          g_storeValid = false;
static volatile bool g_eepromCommitPending = false;

static bool g_eepromBegun = false;
static void eepromBeginOnce(void) {
  if (!g_eepromBegun) { EEPROM.begin(512); g_eepromBegun = true; }
}

void eepromMarkDirty(void)   { g_eepromCommitPending = true; }
bool eepromCommitPending(void) { return g_eepromCommitPending; }

// Copy identity + last_version + calibration FROM the store INTO jumperlessConfig.
static void applyStoreToConfig(void) {
  jumperlessConfig.hardware.generation     = g_store.generation;
  jumperlessConfig.hardware.revision       = g_store.revision;
  jumperlessConfig.hardware.probe_revision = g_store.probe_revision;
  strncpy(jumperlessConfig.firmware.last_version, g_store.last_version,
          sizeof(jumperlessConfig.firmware.last_version) - 1);
  jumperlessConfig.firmware.last_version[sizeof(jumperlessConfig.firmware.last_version) - 1] = '\0';

  jumperlessConfig.calibration.top_rail_zero      = g_store.top_rail_zero;
  jumperlessConfig.calibration.top_rail_spread    = g_store.top_rail_spread;
  jumperlessConfig.calibration.bottom_rail_zero   = g_store.bottom_rail_zero;
  jumperlessConfig.calibration.bottom_rail_spread = g_store.bottom_rail_spread;
  jumperlessConfig.calibration.dac_0_zero         = g_store.dac_0_zero;
  jumperlessConfig.calibration.dac_0_spread       = g_store.dac_0_spread;
  jumperlessConfig.calibration.dac_1_zero         = g_store.dac_1_zero;
  jumperlessConfig.calibration.dac_1_spread       = g_store.dac_1_spread;
  jumperlessConfig.calibration.adc_0_zero = g_store.adc_0_zero; jumperlessConfig.calibration.adc_0_spread = g_store.adc_0_spread;
  jumperlessConfig.calibration.adc_1_zero = g_store.adc_1_zero; jumperlessConfig.calibration.adc_1_spread = g_store.adc_1_spread;
  jumperlessConfig.calibration.adc_2_zero = g_store.adc_2_zero; jumperlessConfig.calibration.adc_2_spread = g_store.adc_2_spread;
  jumperlessConfig.calibration.adc_3_zero = g_store.adc_3_zero; jumperlessConfig.calibration.adc_3_spread = g_store.adc_3_spread;
  jumperlessConfig.calibration.adc_4_zero = g_store.adc_4_zero; jumperlessConfig.calibration.adc_4_spread = g_store.adc_4_spread;
  jumperlessConfig.calibration.adc_7_zero = g_store.adc_7_zero; jumperlessConfig.calibration.adc_7_spread = g_store.adc_7_spread;
  jumperlessConfig.calibration.probe_max  = g_store.probe_max;
  jumperlessConfig.calibration.probe_min  = g_store.probe_min;
  jumperlessConfig.calibration.probe_switch_threshold_high = g_store.probe_switch_threshold_high;
  jumperlessConfig.calibration.probe_switch_threshold_low  = g_store.probe_switch_threshold_low;
  jumperlessConfig.calibration.measure_mode_output_voltage = g_store.measure_mode_output_voltage;
}

// Copy FROM jumperlessConfig INTO the in-RAM store struct (no flash yet).
static void fillStoreFromConfig(void) {
  g_store.magic   = EEPROM_STORE_MAGIC;
  g_store.version = EEPROM_STORE_VERSION;
  g_store.size    = (uint16_t)sizeof(EepromStore);
  g_store.generation     = jumperlessConfig.hardware.generation;
  g_store.revision       = jumperlessConfig.hardware.revision;
  g_store.probe_revision = jumperlessConfig.hardware.probe_revision;
  strncpy(g_store.last_version, jumperlessConfig.firmware.last_version, sizeof(g_store.last_version) - 1);
  g_store.last_version[sizeof(g_store.last_version) - 1] = '\0';
  g_store.top_rail_zero      = jumperlessConfig.calibration.top_rail_zero;
  g_store.top_rail_spread    = jumperlessConfig.calibration.top_rail_spread;
  g_store.bottom_rail_zero   = jumperlessConfig.calibration.bottom_rail_zero;
  g_store.bottom_rail_spread = jumperlessConfig.calibration.bottom_rail_spread;
  g_store.dac_0_zero         = jumperlessConfig.calibration.dac_0_zero;
  g_store.dac_0_spread       = jumperlessConfig.calibration.dac_0_spread;
  g_store.dac_1_zero         = jumperlessConfig.calibration.dac_1_zero;
  g_store.dac_1_spread       = jumperlessConfig.calibration.dac_1_spread;
  g_store.adc_0_zero = jumperlessConfig.calibration.adc_0_zero; g_store.adc_0_spread = jumperlessConfig.calibration.adc_0_spread;
  g_store.adc_1_zero = jumperlessConfig.calibration.adc_1_zero; g_store.adc_1_spread = jumperlessConfig.calibration.adc_1_spread;
  g_store.adc_2_zero = jumperlessConfig.calibration.adc_2_zero; g_store.adc_2_spread = jumperlessConfig.calibration.adc_2_spread;
  g_store.adc_3_zero = jumperlessConfig.calibration.adc_3_zero; g_store.adc_3_spread = jumperlessConfig.calibration.adc_3_spread;
  g_store.adc_4_zero = jumperlessConfig.calibration.adc_4_zero; g_store.adc_4_spread = jumperlessConfig.calibration.adc_4_spread;
  g_store.adc_7_zero = jumperlessConfig.calibration.adc_7_zero; g_store.adc_7_spread = jumperlessConfig.calibration.adc_7_spread;
  g_store.probe_max = jumperlessConfig.calibration.probe_max;
  g_store.probe_min = jumperlessConfig.calibration.probe_min;
  g_store.probe_switch_threshold_high = jumperlessConfig.calibration.probe_switch_threshold_high;
  g_store.probe_switch_threshold_low  = jumperlessConfig.calibration.probe_switch_threshold_low;
  g_store.measure_mode_output_voltage = jumperlessConfig.calibration.measure_mode_output_voltage;
}

bool eepromStoreLoadAndApplyIdentity(void) {
  eepromBeginOnce();

  // First-start marker - kept as its own byte (survives an FS wipe). This is a
  // secondary first-boot signal; the primary one is config.txt being absent.
  if (EEPROM.read(FIRSTSTARTUPADDRESS) != 0xAA) {
    firstStart = true;
    EEPROM.write(FIRSTSTARTUPADDRESS, 0xAA);
    eepromMarkDirty();
  }

  extern int revisionNumber;
  extern int probeRevision;

  EepromStore tmp;
  EEPROM.get(EEPROM_STORE_ADDRESS, tmp);
  if (tmp.magic == EEPROM_STORE_MAGIC &&
      tmp.version == EEPROM_STORE_VERSION &&
      tmp.size == (uint16_t)sizeof(EepromStore)) {
    g_store = tmp;
    g_storeValid = true;
    // Apply identity now so early boot (PSRAM sizing / probe rev) sees it.
    if (g_store.revision > 0 && g_store.revision <= 10)
      jumperlessConfig.hardware.revision = g_store.revision;
    if (g_store.probe_revision > 0 && g_store.probe_revision <= 10)
      jumperlessConfig.hardware.probe_revision = g_store.probe_revision;
    jumperlessConfig.hardware.generation = (g_store.generation > 0) ? g_store.generation : 5;
    revisionNumber = jumperlessConfig.hardware.revision;
    probeRevision  = jumperlessConfig.hardware.probe_revision;
    return true;
  }

  // Legacy layout / fresh chip: migrate just the identity bytes if valid. The
  // calibration is preserved via config.txt on a normal upgrade and re-seeded
  // into the store by eepromReconcileAfterConfig().
  g_storeValid = false;
  int legacyRev   = EEPROM.read(REVISIONADDRESS);
  int legacyProbe = EEPROM.read(PROBE_REVISIONADDRESS);
  if (legacyRev > 0 && legacyRev <= 10)   jumperlessConfig.hardware.revision = legacyRev;
  if (legacyProbe > 0 && legacyProbe <= 10) jumperlessConfig.hardware.probe_revision = legacyProbe;
  revisionNumber = jumperlessConfig.hardware.revision;
  probeRevision  = jumperlessConfig.hardware.probe_revision;
  return false;
}

void eepromReconcileAfterConfig(void) {
  eepromBeginOnce();
  if (g_storeValid) {
    // EEPROM wins for the kept fields so they survive an FS wipe (which resets
    // config.txt to defaults). Re-apply over whatever loadConfig() set.
    applyStoreToConfig();
    extern int revisionNumber;
    extern int probeRevision;
    revisionNumber = jumperlessConfig.hardware.revision;
    probeRevision  = jumperlessConfig.hardware.probe_revision;
  } else {
    // No valid store yet (legacy unit upgrading, or fresh chip). config.txt is
    // now fully loaded, so seed the store from it and schedule a deferred
    // commit. On a normal upgrade this preserves the existing calibration.
    fillStoreFromConfig();
    EEPROM.put(EEPROM_STORE_ADDRESS, g_store);
    g_storeValid = true;
    eepromMarkDirty();
  }
}

void eepromPersistFromConfig(void) {
  eepromBeginOnce();
  fillStoreFromConfig();
  // Only flag a pending flash commit when a kept field actually changed. This
  // keeps the EEPROM commit truly lazy: a saveConfig() that touched only
  // non-calibration settings leaves the store byte-identical, so no commit is
  // scheduled and eepromCommitSafe() stays a no-op (no Core-2 pause / flash
  // erase). EEPROM.get/put operate on the in-RAM mirror, so this diff is cheap.
  EepromStore existing;
  EEPROM.get(EEPROM_STORE_ADDRESS, existing);
  bool changed = (memcmp(&existing, &g_store, sizeof(EepromStore)) != 0);
  EEPROM.put(EEPROM_STORE_ADDRESS, g_store);
  g_storeValid = true;
  if (changed) eepromMarkDirty();
}

bool eepromCommitHeld(void) {
  if (!g_eepromCommitPending) return false;
  // Caller already holds pauseCore2ForFlash() + fs_mutex. EEPROM.commit() does
  // its own noInterrupts()+idleOtherCore() internally; that is safe here
  // because Core 1 is parked and fs_mutex serializes us against any SPIFTL op.
  EEPROM.commit();
  g_eepromCommitPending = false;
  return true;
}

bool eepromCommitSafe(void) {
  if (!g_eepromCommitPending) return false;
  AsyncPassthrough::suspendUARTRxIRQ();
  bool was_paused = pauseCore2ForFlash(100);
  bool committed = false;
  if (fs_mutex_acquire_timeout_ms(5000)) {
    EEPROM.commit();
    g_eepromCommitPending = false;
    committed = true;
    fs_mutex_release();
  }
  unpauseCore2ForFlash(was_paused);
  AsyncPassthrough::resumeUARTRxIRQ();
  return committed;
}

void saveDacCalibration(void)
  {
  // Mirror calibration into config (the human-readable copy)...
  jumperlessConfig.calibration.dac_0_spread = dacSpread[0];
  jumperlessConfig.calibration.dac_1_spread = dacSpread[1];
  jumperlessConfig.calibration.top_rail_spread = dacSpread[2];
  jumperlessConfig.calibration.bottom_rail_spread = dacSpread[3];
  jumperlessConfig.calibration.dac_0_zero = dacZero[0];
  jumperlessConfig.calibration.dac_1_zero = dacZero[1];
  jumperlessConfig.calibration.top_rail_zero = dacZero[2];
  jumperlessConfig.calibration.bottom_rail_zero = dacZero[3];
  jumperlessConfig.calibration.adc_0_spread = adcSpread[0];
  jumperlessConfig.calibration.adc_1_spread = adcSpread[1];
  jumperlessConfig.calibration.adc_2_spread = adcSpread[2];
  jumperlessConfig.calibration.adc_3_spread = adcSpread[3];
  jumperlessConfig.calibration.adc_4_spread = adcSpread[4];
  jumperlessConfig.calibration.adc_7_spread = adcSpread[7];
  jumperlessConfig.calibration.adc_0_zero = adcZero[0];
  jumperlessConfig.calibration.adc_1_zero = adcZero[1];
  jumperlessConfig.calibration.adc_2_zero = adcZero[2];
  jumperlessConfig.calibration.adc_3_zero = adcZero[3];
  jumperlessConfig.calibration.adc_4_zero = adcZero[4];
  jumperlessConfig.calibration.adc_7_zero = adcZero[7];

  // ...and into the durable EEPROM store (FS-wipe survivor). The actual flash
  // commit is deferred to the FileCache flush window - saveConfig() below will
  // trigger a file save that coalesces the EEPROM commit into the same Core-1
  // pause (see eepromCommitHeld() in the FileCache flush path).
  eepromPersistFromConfig();

  saveConfig();
  //Serial.println("DAC calibration saved to both EEPROM (deferred) and config file");
  }

void debugFlagSet(int flag) {
  // config.txt is the source of truth. Each case derives the new value from the
  // CURRENT jumperlessConfig field (a single source - no more EEPROM read/toggle
  // drift that desynced the menu) and updates the runtime global to match.
  // Persistence: the debug menu calls saveConfig() ONCE after applying all
  // diffs; the bulk/standalone cases (0/6/9/13) save themselves.
  switch (flag) {
    case 1:  debugFP    = !jumperlessConfig.debug.file_parsing;       jumperlessConfig.debug.file_parsing      = debugFP;    break;
    case 2:  debugNM    = !jumperlessConfig.debug.net_manager;        jumperlessConfig.debug.net_manager       = debugNM;    break;
    case 3:  debugNTCC  = !jumperlessConfig.debug.nets_to_chips;      jumperlessConfig.debug.nets_to_chips     = debugNTCC;  break;
    case 4:  debugNTCC2 = !jumperlessConfig.debug.nets_to_chips_alt;  jumperlessConfig.debug.nets_to_chips_alt = debugNTCC2; break;
    case 5:  debugLEDs  = !jumperlessConfig.debug.leds;               jumperlessConfig.debug.leds              = debugLEDs;  break;
    case 6:  debugLA    = !jumperlessConfig.debug.logic_analyzer;     jumperlessConfig.debug.logic_analyzer    = debugLA;
             saveConfig(); break;  // standalone caller (Menus) - persist now
    case 7:  showProbeCurrent = jumperlessConfig.debug.show_probe_current ? 0 : 1;
             jumperlessConfig.debug.show_probe_current = showProbeCurrent; break;
    case 8:
      if (jumperlessConfig.serial_1.print_passthrough == 0)      jumperlessConfig.serial_1.print_passthrough = 2;
      else if (jumperlessConfig.serial_1.print_passthrough == 1) jumperlessConfig.serial_1.print_passthrough = 0;
      else if (jumperlessConfig.serial_1.print_passthrough == 2) jumperlessConfig.serial_1.print_passthrough = 1;
      break;
    case 14: debugWaitLoopTiming = !debugWaitLoopTiming; break;  // runtime only
    case 15: debugUSB = !debugUSB; break;                        // runtime only

    case 0:  // all off
      debugFP = debugFPtime = debugNM = debugNMtime = false;
      debugNTCC = debugNTCC2 = debugLEDs = debugLA = false;
      debugWaitLoopTiming = false;
      debugArduino = 0;
      showProbeCurrent = 0;
      debugProbing = 0;
      jumperlessConfig.debug.probing           = false;
      jumperlessConfig.debug.file_parsing      = false;
      jumperlessConfig.debug.net_manager       = false;
      jumperlessConfig.debug.nets_to_chips     = false;
      jumperlessConfig.debug.nets_to_chips_alt = false;
      jumperlessConfig.debug.leds              = false;
      jumperlessConfig.debug.logic_analyzer    = false;
      jumperlessConfig.debug.arduino           = 0;
      jumperlessConfig.debug.show_probe_current = 0;
      saveConfig();
      Serial.println("All debug flags disabled (saved to config.txt)");
      break;

    case 9:  // all on
      debugFP = debugFPtime = debugNM = debugNMtime = true;
      debugNTCC = debugNTCC2 = debugLEDs = debugLA = true;
      debugWaitLoopTiming = true;
      showProbeCurrent = 1;
      debugProbing = 1;
      jumperlessConfig.debug.probing           = true;
      jumperlessConfig.debug.file_parsing      = true;
      jumperlessConfig.debug.net_manager       = true;
      jumperlessConfig.debug.nets_to_chips     = true;
      jumperlessConfig.debug.nets_to_chips_alt = true;
      jumperlessConfig.debug.leds              = true;
      jumperlessConfig.debug.logic_analyzer    = true;
      jumperlessConfig.debug.show_probe_current = 1;
      saveConfig();
      Serial.println("All debug flags enabled (saved to config.txt)");
      break;

    case 10: rotaryEncoderMode = 0; break;  // runtime only now
    case 11: rotaryEncoderMode = 1; break;  // runtime only now
    case 12: break;                          // display mode - reserved
    case 13: jumperlessConfig.display.net_color_mode = netColorMode; saveConfig(); break;
    default: break;
    }
  }

void saveVoltages(float top, float bot, float dac0, float dac1) {
  //#ifdef EEPROMSTUFF
  // EEPROM.put(TOP_RAIL_ADDRESS0, top);
  // EEPROM.put(BOTTOM_RAIL_ADDRESS0, bot);
  // EEPROM.put(DAC0_ADDRESS0, dac0);
  // EEPROM.put(DAC1_ADDRESS0, dac1);
  // EEPROM.commit();
  // delayMicroseconds(100);
  // //#endif

      // Voltage state is now stored in globalState.power (already updated by caller)
  // No need to save to config - voltages are saved in YAML state files
  
  //configChanged = true;
  // saveConfig();
  }

void saveDuplicateSettings(int forceDefaults) {

  configChanged = true;

  // saveConfig();
  }

void readVoltages(void) {


  return;
  }

void saveLogoBindings(void) {
  // config.txt is the source of truth (EEPROM no longer stores logo bindings).
  jumperlessConfig.logo_pads.top_guy = logoTopSetting[0];
  jumperlessConfig.logo_pads.bottom_guy = logoBottomSetting[0];
  jumperlessConfig.logo_pads.building_pad_top = buildingTopSetting[0];
  jumperlessConfig.logo_pads.building_pad_bottom = buildingBottomSetting[0];

  configChanged = true;

  //  saveConfig();
  }

void readLogoBindings(void) {
  // Load from config.txt (called from readSettingsFromConfig). The [1] slot is
  // a runtime-only companion value that was never meaningfully persisted.
  logoTopSetting[0]        = jumperlessConfig.logo_pads.top_guy;
  logoBottomSetting[0]     = jumperlessConfig.logo_pads.bottom_guy;
  buildingTopSetting[0]    = jumperlessConfig.logo_pads.building_pad_top;
  buildingBottomSetting[0] = jumperlessConfig.logo_pads.building_pad_bottom;
  }

void saveLEDbrightness(int forceDefaults) {
  if (forceDefaults == 1) {
    LEDbrightness = DEFAULTBRIGHTNESS;
    LEDbrightnessRail = DEFAULTRAILBRIGHTNESS;
    LEDbrightnessSpecial = DEFAULTSPECIALNETBRIGHTNESS;
    menuBrightnessSetting = 0;
    }

  // Save to config file (EEPROM no longer stores brightness).
  jumperlessConfig.display.led_brightness = LEDbrightness;
  jumperlessConfig.display.rail_brightness = LEDbrightnessRail;
  jumperlessConfig.display.special_net_brightness = LEDbrightnessSpecial;
  jumperlessConfig.display.menu_brightness = menuBrightnessSetting;

  configChanged = true;
  //saveConfig();
  }


void updateStateFromGPIOConfig(int onlyIdx) {
#if defined(OG_JUMPERLESS)
  // SKIPPED on OG: same hazard as readSettingsFromConfig()/setGPIO() etc - the
  // gpioDef bank (pins 20-27) is the V5 routable-GPIO map, but on the OG pins
  // 20-23 are the CH446Q chip selects for chips I/J/K/L. Driving them here
  // (reachable via the probe GPIO-direction selection, a Phase-2 path on OG)
  // would re-mux those CS lines to inputs and break all SF routing.
  return;
#endif
  // NOTE: the probe's GPIO buffer-power claim (debug.probe_power_gpio) holds
  // one of these pins output-HIGH; forcing it through the "output starts
  // low" default here grounded ROUTABLE_BUFFER_IN and killed measure-mode
  // probing with the claim bridge still in place. Skip the claimed pin.
  // (An old stray `break` also made this function only ever touch the FIRST
  // SIO pin - i.e. GPIO_1, the usual claim - regardless of which pin the
  // caller had changed. Callers now pass the index they modified.)
  extern int probeGpioPowerClaimIdx(void);
  int claimIdx = probeGpioPowerClaimIdx();

  for (int i = 0; i < 10; i++) {  // Changed from 8 to 10 to include UART pins
    // Map gpioState to direction and pull settings

    int gpio_pin = gpioDef[i][0];  // Map GPIO 0-7 to pins 20-27
    if (onlyIdx >= 0 && i != onlyIdx) {
      continue;
    }
    if (i == claimIdx) {
      continue;
    }
    if (gpio_function_map[i] == GPIO_FUNC_SIO) {

      switch (globalState.config.gpioDirection[i]) {
        case 0: // output (starts low)
          gpioState[i] = 0;
          gpio_set_dir(gpio_pin, true);  // Set as output
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          // gpio_put(gpio_pin, 0);
          break;
        case 1: // input (pulls per config)
          switch (globalState.config.gpioPulls[i]) {
            case 0: // pulldown
              gpioState[i] = 4;
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, false, true);  // Pull down
              break;
            case 1: // pullup
              gpioState[i] = 3;
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, true, false);  // Pull up
              break;
            case 2: // no pull
              gpioState[i] = 2;
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, false, false);  // No pulls
              break;
            case 3: // bus keeper
              gpioState[i] = 7;  // New state for bus keeper
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, true, true);  // Both pulls enabled = bus keeper
              break;

            }
          break;

        }

      }
    }
  }

void updateGPIOConfigFromState(void) {
#if defined(OG_JUMPERLESS)
  // The V5 routable-GPIO bank described by gpioDef (pins 20-27) does NOT exist
  // on the OG. There, pins 20-23 are the CH446Q chip selects for chips I/J/K/L,
  // pin 24 is RESET, pin 25 is the WS2812 LED data line and 26-27 are ADC
  // inputs. Running gpio_set_dir()/gpio_set_pulls() over that bank here re-muxes
  // the SF chip-select pins into GPIO inputs, after which setCSex() can no
  // longer assert them -- so every breadboard<->SF (nano/DAC/ADC) connection
  // silently fails to program while A-H (CS 6-13, untouched) keep working. The
  // OG has no user-routable GPIO bank here, so skip entirely (matches
  // initGPIO()/setGPIO()).
  return;
#endif
  // Serial.println("updateGPIOConfigFromState");
  // Serial.flush();
  // return;
  int changed = 0;
  for (int i = 0; i < 10; i++) {  // Changed from 8 to 10 to include UART pins
    // Map gpioState to direction and pull settings

    int gpio_pin = gpioDef[i][0];  // Map GPIO 0-7 to pins 20-27

    // Skip MicroPython-claimed pins (same contract as readGPIO()/setGPIO()):
    // machine.Pin owns dir/pulls until jl_gpio_release_all_pins(), and claimed
    // pins report GPIO_FUNC_SIO so they'd otherwise be stomped right here.
    if (globalState.config.gpioPythonOwned[i]) {
      continue;
    }

    if (gpio_function_map[i] == GPIO_FUNC_SIO) {

      switch (gpioState[i]) {
        case 0: // output low
          if (globalState.config.gpioDirection[i] != 0 || globalState.config.gpioPulls[i] != 2) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 0; // output
          globalState.config.gpioPulls[i] = 2; // no pull
          gpio_set_dir(gpio_pin, true);  // Set as output
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          break;
        case 1: // output high
          if (globalState.config.gpioDirection[i] != 0 || globalState.config.gpioPulls[i] != 2) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 0; // output
          globalState.config.gpioPulls[i] = 2;
          gpio_set_dir(gpio_pin, true);  // Set as output
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          break;
        case 2: // input
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 2) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 2; // no pull
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          break;
        case 3: // input pullup
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 1) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 1; // pullup
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, true, false);  // Pull up
          break;
        case 4: // input pulldown
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 0) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 0; // pulldown
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, false, true);  // Pull down
          break;
        case 5: // unknown
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 0) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // default to input
          globalState.config.gpioPulls[i] = 0; // default to pulldown
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, false, true);  // Pull down
          break;
        case 6: // do nothing
          break;
        case 7: // bus keeper
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 3) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 3; // bus keeper
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, true, true);  // Both pulls enabled = bus keeper
          break;
        }
      }
    }


  if (changed == 1) {
    configChanged = true; // Mark config as changed so it will be saved
    }

  }

// (runCommandAfterReset / lastCommandRead / lastCommandWrite removed - the
// reset-command handoff feature was unused, so its EEPROM control bytes are
// gone too.)

void readSettingsFromConfig() {
  // Debug flags
  debugFP = jumperlessConfig.debug.file_parsing;
  //debugFPtime = jumperlessConfig.debug_flags.file_parsing_time;
  debugNM = jumperlessConfig.debug.net_manager;
  //debugNMtime = jumperlessConfig.debug_flags.net_manager_time;
  debugNTCC = jumperlessConfig.debug.nets_to_chips;
  debugNTCC2 = jumperlessConfig.debug.nets_to_chips_alt;
  debugLEDs = jumperlessConfig.debug.leds;
  debugLA = jumperlessConfig.debug.logic_analyzer;
  debugProbing = jumperlessConfig.debug.probing ? 1 : 0;
  // Sync Arduino debug level to global
  debugArduino = jumperlessConfig.debug.arduino;
  showProbeCurrent = jumperlessConfig.debug.show_probe_current;

  // Logo / building pad bindings (config is the source of truth now)
  readLogoBindings();

  // Display settings
  LEDbrightness = jumperlessConfig.display.led_brightness;
  LEDbrightnessRail = jumperlessConfig.display.rail_brightness;
  LEDbrightnessSpecial = jumperlessConfig.display.special_net_brightness;
  menuBrightnessSetting = jumperlessConfig.display.menu_brightness;
  netColorMode = jumperlessConfig.display.net_color_mode;

#if defined(OG_JUMPERLESS)
  // The OG lights ONE LED per breadboard row; V5 lights five, so the same
  // brightness value reads ~5x dimmer on the OG (and the shipped default
  // led_brightness is only 10). Floor the per-net/rail/special brightness well
  // up so nets, rails and the header are clearly visible. This only raises the
  // runtime variables (not the saved config), so the 'l' command can still set
  // any higher value at runtime and nothing compounds on the next config load.
  if (LEDbrightness < 90) LEDbrightness = 90;
  if (LEDbrightnessRail < 100) LEDbrightnessRail = 100;
  if (LEDbrightnessSpecial < 100) LEDbrightnessSpecial = 100;
#endif

  // // Routing settings
  // pathDuplicates = jumperlessConfig.routing.stack_paths;
  // powerDuplicates = jumperlessConfig.routing.stack_rails;  // powerDuplicates is used for rail stacking
  // dacDuplicates = jumperlessConfig.routing.stack_dacs;    // dacDuplicates is used for DAC stacking
  // dacPriority = jumperlessConfig.routing.rail_priority;

  // DAC calibration
  dacSpread[0] = jumperlessConfig.calibration.dac_0_spread;
  dacSpread[1] = jumperlessConfig.calibration.dac_1_spread;
  dacSpread[2] = jumperlessConfig.calibration.top_rail_spread;
  dacSpread[3] = jumperlessConfig.calibration.bottom_rail_spread;

  dacZero[0] = jumperlessConfig.calibration.dac_0_zero;
  dacZero[1] = jumperlessConfig.calibration.dac_1_zero;
  dacZero[2] = jumperlessConfig.calibration.top_rail_zero;
  dacZero[3] = jumperlessConfig.calibration.bottom_rail_zero;

  // ADC calibration
  adcSpread[0] = jumperlessConfig.calibration.adc_0_spread;
  adcSpread[1] = jumperlessConfig.calibration.adc_1_spread;
  adcSpread[2] = jumperlessConfig.calibration.adc_2_spread;
  adcSpread[3] = jumperlessConfig.calibration.adc_3_spread;
  adcSpread[4] = jumperlessConfig.calibration.adc_4_spread;
  adcSpread[7] = jumperlessConfig.calibration.adc_7_spread;
  adcZero[0] = jumperlessConfig.calibration.adc_0_zero;
  adcZero[1] = jumperlessConfig.calibration.adc_1_zero;
  adcZero[2] = jumperlessConfig.calibration.adc_2_zero;
  adcZero[3] = jumperlessConfig.calibration.adc_3_zero;
  adcZero[4] = jumperlessConfig.calibration.adc_4_zero;
  adcZero[7] = jumperlessConfig.calibration.adc_7_zero;


  // DAC voltages are now stored in globalState.power (loaded from YAML state file)
  // No longer using jumperlessConfig.dacs for voltage state

  // Serial.print("topRail: ");
  // Serial.println(globalState.power.topRail);
  // Serial.print("bottomRail: ");
  // Serial.println(globalState.power.bottomRail);
  // Serial.print("dac0: ");
  // Serial.println(globalState.power.dac0);
  // Serial.print("dac1: ");
  // Serial.println(globalState.power.dac1);

  probePowerDAC = jumperlessConfig.dacs.probe_power_dac;

  //GPIO settings
#if !defined(OG_JUMPERLESS)
  // SKIPPED on OG: the gpioDef bank (pins 20-27) is the V5 routable-GPIO map.
  // On the OG pins 20-23 are the CH446Q chip selects for chips I/J/K/L, 24 is
  // RESET, 25 is the WS2812 LED data line and 26-27 are ADC inputs.
  // readSettingsFromConfig() runs at boot and on every config change, AFTER
  // initCH446Q() has set 20-23 to OUTPUT on core1. The default routable-GPIO
  // config is input+pulldown, so this loop drives pins 20-23 back to inputs
  // (gpio_set_dir false) -- setCSex() can then no longer assert them and every
  // breadboard<->SF (nano/DAC/ADC) connection silently fails to program while
  // A-H (CS 6-13, untouched) keep working. Matches the OG guards in
  // initGPIO()/setGPIO()/updateGPIOConfigFromState()/applyStateToHardware().
  for (int i = 0; i < 10; i++) {  // Changed from 8 to 10 to include UART pins

    // Combine direction and pull settings into a single value
    // 0 = output low, 1 = output high, 2 = input, 3 = input pullup, 4 = input pulldown

    int gpio_pin = gpioDef[i][0];
    // if (i == 8) {
    //   gpio_pin = 0; // UART TX
    //   } else if (i == 9) {
    //     gpio_pin = 1; // UART RX
    //     }

   
   // gpio_init(gpio_pin);

   // if (gpio_get_function(gpio_pin) == GPIO_FUNC_SIO) {
    if (globalState.config.gpioDirection[i] == 0) { // output
      //gpioState[i] = globalState.config.gpioPulls[i] ? 1 : 0; // 1 for high, 0 for low
      gpio_set_dir(gpio_pin, true);
      gpioState[i] = 0;
      // Serial.print("gpio_pin: ");
      // Serial.print(gpio_pin);
      // Serial.print(" gpioState[i]: ");
      // Serial.print(gpioState[i]);
      // Serial.print(" actual: ");
      // Serial.println(gpio_get_dir(gpio_pin));
      //Serial.flush();
      } else if (globalState.config.gpioDirection[i] == 1) { // input
        gpio_set_dir(gpio_pin, false);
        gpioState[i] = 2;
        //         Serial.print("gpio_pin: ");
        // Serial.print(gpio_pin);
        // Serial.print(" gpioState[i]: ");
        // Serial.print(gpioState[i]);
        // Serial.print(" actual: ");
        // Serial.println(gpio_get_dir(gpio_pin));
        // Serial.flush();
        if (globalState.config.gpioPulls[i] == 2) { // no pull
          //  gpioState[i] = 2;
          gpio_set_pulls(gpio_pin, false, false);
          } else if (globalState.config.gpioPulls[i] == 1) { // pullup
            gpioState[i] = 3;
            gpio_set_pulls(gpio_pin, true, false);
            } else if (globalState.config.gpioPulls[i] == 0) { // pulldown
              gpioState[i] = 4;
              gpio_set_pulls(gpio_pin, false, true);
              } else if (globalState.config.gpioPulls[i] == 3) { // bus keeper
              gpioState[i] = 7;
              gpio_set_pulls(gpio_pin, true, true);
              } else {
              gpioState[i] = 5; // unknown
              gpio_set_pulls(gpio_pin, false, false);
              }
        } else {
        gpioState[i] = 5; // unknown
        // gpio_set_dir(gpio_pin, false);
        // gpio_set_pulls(gpio_pin, false, false);
        }
     // }
    }
#endif // !OG_JUMPERLESS

  // Serial
  baudRateUSBSer1 = jumperlessConfig.serial_1.baud_rate;
  baudRateUSBSer2 = jumperlessConfig.serial_2.baud_rate;
  printSerial1Passthrough = jumperlessConfig.serial_1.print_passthrough;
  printSerial2Passthrough = jumperlessConfig.serial_2.print_passthrough;
  connectOnBoot1 = jumperlessConfig.serial_1.connect_on_boot;
  connectOnBoot2 = jumperlessConfig.serial_2.connect_on_boot;
  lockConnection1 = jumperlessConfig.serial_1.lock_connection;
  lockConnection2 = jumperlessConfig.serial_2.lock_connection;





  }
/// 0 = output low, 1 = output high, 2 = input, 3 = input pullup, 4 = input pulldown, 5 = unknown