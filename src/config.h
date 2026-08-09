#ifndef CONFIG_H
#define CONFIG_H

//Jumperless config
#include "JumperlessDefines.h"


// extern int hwRevision;
// extern int probeRevision;


extern struct config jumperlessConfig;
// Forward declarations of nested structs
struct firmware;
struct hardware;
struct dacs;
struct debug;
struct routing;
struct calibration;
struct logo_pads;
struct display_settings;
struct serial_1;
struct serial_2;
struct top_oled;

struct config {
    struct firmware {
        char last_version[16] = "";  // Last firmware version that was run
        bool files_provisioned = false;  // Whether files have been provisioned for this version
    } firmware;

    struct hardware {
        int generation = 5;
        int revision = 5;
        int probe_revision = 5;
        int psram_installed = 0;
        // KiB of PSRAM reserved for the app arena (file cache, undo log,
        // SharedBuffer, scratch). Remainder goes to the MicroPython GC heap.
        // Default 2048 (2MB app / 6MB MicroPython).
        int psram_app_size_kb = 2048;
        // Use a PIO state machine (shared with the probe LED's WS2812 SM)
        // to do the active-drive/release/sample sequence for probe button
        // reads, instead of the CPU bit-banging the IO pads. PIO path is
        // ~75x faster, keeps PROBE_LED_PIN in PIO mode the whole time
        // (no SIO/PIO function switching), and eliminates the line-cut
        // race that caused WS2812 shifting artifacts. Falls back to the
        // CPU path automatically if PIO program memory can't be claimed.
        bool use_pio_probe_button = true;
        // Drive the probe's WS2812 data (and the shared button sampling)
        // on BUTTON_PIN (GPIO 9) instead of PROBE_LED_PIN (GPIO 2). The
        // two pins land on the same TRRS plug ring, and the most common
        // cable failure is the GPIO2-side jack contact going flaky - with
        // everything on GPIO9 that contact stops mattering entirely.
        bool probe_led_on_button_pin = true;
    } hardware;

    struct dacs {
        // Voltage values moved to activeState.power (topRail, bottomRail, dac0, dac1)
        bool set_dacs_on_boot = true;
        bool set_rails_on_boot = true;
        int probe_power_dac = 0;
        float limit_max = 8.00;
        float limit_min = -8.00;
        int auto_connect_probe = 1; //-1 = off (persistent), 0 = off (until reboot), 1 = on
    } dacs;

    struct debug {
        bool file_parsing = false;
        bool net_manager = false;
        bool nets_to_chips = false;
        bool nets_to_chips_alt = false;
        bool leds = false;
        bool probing = false;
        bool oled = false;
        bool logo_pads = false;
        bool logic_analyzer = true; 
        int  arduino = 0;
        bool  usb_mass_storage = false;
        int  show_probe_current = 0;  // moved out of EEPROM - config is source of truth
        bool show_node_errors = true;
        // Power the probe's measure-mode buffer from an unused routable
        // GPIO driven HIGH instead of DAC0, keeping the DAC free. Switch-
        // position sensing then reads the load current as ADC7 voltage
        // droop against a continuously-tracked unloaded peak V0 (persisted
        // as calibration.probe_droop_v0); the DAC bridge swap survives only
        // as a fallback while no GPIO is claimed. Default ON since 5.7:
        // the droop path got a clamped median-confirmed V0 tracker and no
        // longer freezes when DAC0 is reassigned.
        bool probe_power_gpio = true;
        // Print one line per switch-position evaluation: sensing source
        // (ADC7 droop / DAC swap / INA), estimated current, droop anchor,
        // thresholds, and the resulting position.
        bool probe_switch_stats = false;
        // Print net voltage scan stats once a second: every scanned node's
        // voltage and every routed path's estimated current.
        bool net_voltage_scan = false;
        // Probe engine v2 (src/Probing/): calibration-free physics-table pad
        // decode + non-blocking probe-mode session service. Off = legacy
        // Probing.cpp engine (probe_min/probe_max linear map, blocking
        // probeMode loop). Takes effect at the next probe session entry.
        // Forced off on OG hardware (no ADC7 / measure buffer).
        bool probe_engine_v2 = true;
    } debug;

    struct routing {
        int stack_paths = 2;
        int stack_rails = 3;
        int stack_dacs = 0;
        int rail_priority = 1;
    } routing;

            // USB CDC settings for flow control behavior
            struct usb_cdc {
                // When true, ignore DTR line state - allows communication with hosts
                // that don't set DTR (some industrial software, custom applications)
                // Default: false (normal behavior - require DTR for connection)
                bool ignore_dtr = false;
            } usb_cdc;

    struct calibration {
        int top_rail_zero = 1650;
        float top_rail_spread = 21.5;
        int bottom_rail_zero = 1650;
        float bottom_rail_spread = 21.5;
        int dac_0_zero = 1650;
        float dac_0_spread = 21.5;
        int dac_1_zero = 1650;
        float dac_1_spread = 21.5;
        float adc_0_zero = 9.0;
        float adc_0_spread = 18.28;
        float adc_1_zero = 9.0;
        float adc_1_spread = 18.28;
        float adc_2_zero = 9.0;
        float adc_2_spread = 18.28;
        float adc_3_zero = 9.0;
        float adc_3_spread = 18.28;
        float adc_4_zero = 0.0;
        float adc_4_spread = 5.0;
        float adc_7_zero = 9.0;
        float adc_7_spread = 18.28;
        int probe_max = 4055;
        int probe_min = 10;
        // MEASURE-position pad decode endpoints, stored in the 3.3V frame
        // (seeded from probe_min/probe_max, individually adjustable). The
        // base pair is calibrated with the SELECT feed (PROBE_PIN at ~3.3V)
        // driving the tip; in MEASURE the buffer drives it instead (DAC at
        // measure_mode_output_voltage, or the drooped GPIO level under
        // debug.probe_power_gpio), and the resistive pad ladder scales every
        // reading with that voltage - so the decode multiplies these
        // endpoints by (live ADC7 tip voltage / 3.3V) at runtime.
        int probe_max_measure = 4055;
        int probe_min_measure = 10;
        // Hysteresis thresholds to prevent oscillation between modes
        // Switch to SELECT mode when current > high threshold
        // Switch to MEASURE mode when current < low threshold
        float probe_switch_threshold_high = 1.2;  // mA - switch to SELECT
        float probe_switch_threshold_low = 0.90;   // mA - switch to MEASURE
        float probe_switch_threshold = 0.40;       // DEPRECATED - kept for backward compatibility
        float measure_mode_output_voltage = 3.33;
        float probe_current_zero = 2.0;
        int minimum_probe_reading = 85;
        // GPIO-powered measure buffer: ADC7 tip voltage (volts) that maps to
        // zero droop-current. Persisted so boot works before both switch
        // positions have been visited; seeded from hardware (~3.35V MEASURE
        // unloaded at 12mA GPIO drive). Runtime peak-tracker may raise it.
        float probe_droop_v0 = 3.35f;
        // On-resistance of one CH446Q crosspoint, used by the net voltage
        // scan to turn node voltage deltas into per-connection currents.
        // A single 2-crosspoint path measures ~80 ohms, so ~40 each.
        float crosspoint_resistance = 40.0;
        // ---- Probe engine v2 (calibration-free decode) ----
        // Source resistance of the SELECT tip feed: PROBE_PIN pad Rout plus
        // the two 47R protection resistors in the tip path. Used to build
        // the expected-count table; per-board variation is absorbed by the
        // trim below, so this rarely needs touching.
        float probe_rsrc_ohms = 125.0;
        // Bounded fine-trim on the SELECT decode scale (1.0 = nominal 4095
        // counts full-scale). Auto-adjusted by the residual tracker from
        // high-confidence decodes; also nudgeable in the probe verification
        // app. Clamped to [0.95, 1.05] on load.
        float probe_scale_trim = 1.0;
        // Same, for the MEASURE decode scale (multiplies the ADC7-derived
        // tip counts; absorbs the ADC7 front-end divider tolerance).
        float probe_scale_trim_measure = 1.0;
        // Multiplies every ladder resistance in the physics table. The real
        // boards read ~2.3% more resistive than the schematic nominals
        // relative to the 10K sense divider (sense resistor + ladder batch
        // tolerance); because the ladder is designed for uniform count
        // steps, a proportional R error shifts EVERY mid-chain pad by the
        // same fraction of a band - at 1.0 the decode read one row high
        // from ~row 28 onward (measured on hardware). Auto-trimmed by
        // deep-ladder confident decodes; wheel-adjustable in the probe
        // verification app. Clamped to [0.95, 1.10] on load.
        float probe_ladder_trim = 1.023;
    } calibration;

    struct logo_pads {
        int top_guy = 0; // 0 = uart tx, 1 = uart rx, others as I think of them
        int bottom_guy = 1;
        int building_pad_top = 25;   // default to current sense +
        int building_pad_bottom = 26; // default to current sense -
        int repeat_ms = 100;
    } logo_pads;

    struct display {
#if defined(OG_JUMPERLESS)
        // OG has 1 LED per breadboard row; "wires" mode paints the 5-LEDs-per-row
        // fill, which garbles the single-pixel strip. Default to lines so a freshly
        // flashed OG renders correctly (parseLinesWires() also forces this on load).
        volatile int lines_wires = 0;
#else
        volatile int lines_wires = 1;
#endif
        int menu_brightness = -10;
        int led_brightness = 10;
        int rail_brightness = 55;
        int special_net_brightness = 20;
        int net_color_mode = 0;
        // Scan node voltages in the background and show per-connection
        // current as faint voltage-colored marching ants (V5 only).
        int net_currents = 1;
        // Direction the current ants march: 0 = conventional (+ to -),
        // 1 = electron (- to +). Only flips the LED animation; reported
        // current values stay conventional-signed.
        int current_flow = 0;
        int dump_leds = -1;
        int dump_format = 0;
        int terminal_line_buffering = 1;
    } display;

    
        struct serial_1 {
            int function = 1; 
            int baud_rate = 115200;
            int print_passthrough = 0;
            int connect_on_boot = 0;
            int lock_connection = 0;
            int autoconnect_flashing = 1;
            bool async_passthrough = true;
            int tag_parsing = 1; // 0 = disabled, 1 = passthrough + tag parsing, 2 = parsing + strip tags
            int flash_reset_type = 1; // 0 = no reset, 1 = AVR, 2 = ESP32
        } serial_1;

        
        struct serial_2 {
            int function = 3; // 0 = off 3 = USB MSC enabled
            int baud_rate = 115200;
            int print_passthrough = 0;
            int connect_on_boot = 0;
            int lock_connection = 0;
            int autoconnect_flashing = 0;
        } serial_2;

        struct top_oled {
            int enabled = 0;
            int i2c_address = 0x3C;
            const char* display_type = "SSD1306";
           
            int width = 128;
            int height = 32;
            int rotation = 0; // 0 = 0 degrees, 1 = 90 degrees, 2 = 180 degrees, 3 = 270 degrees

            int connection_type = 0; // 0 = GPIO 7/8, 1 = RP6/RP7, 2 = internal I2C0, 3 = custom (use sda_pin and scl_pin to set the pins)
            int sda_pin = 26;//the actual hardware pin number
            int scl_pin = 27;//the actual hardware pin number
            int gpio_sda = RP_GPIO_7; //the define number for the hardware pin
            int gpio_scl = RP_GPIO_8; //the define number for the hardware pin
            int sda_row = NANO_D2; //the row number
            int scl_row = NANO_D3; //the row number
            int connect_on_boot = 1;
            int lock_connection = 0;
            int autoconnect_check_interval = -1;
            int font = 0;
            int show_in_terminal = 0;
            char startup_message[33] = ""; // Startup message for OLED (max 32 chars + null terminator)
        } top_oled;


    
};

#endif // CONFIG_H

/*
 if (EEPROM.read(FIRSTSTARTUPADDRESS) != 0xAA || forceDefaults == 1) {
   EEPROM.write(FIRSTSTARTUPADDRESS, 0xAA);
   
   EEPROM.write(REVISIONADDRESS, REV);

   EEPROM.write(DEBUG_FILEPARSINGADDRESS, 0);
   EEPROM.write(TIME_FILEPARSINGADDRESS, 0);
   EEPROM.write(DEBUG_NETMANAGERADDRESS, 0);
   EEPROM.write(TIME_NETMANAGERADDRESS, 0);
   EEPROM.write(DEBUG_NETTOCHIPCONNECTIONSADDRESS, 0);
   EEPROM.write(DEBUG_NETTOCHIPCONNECTIONSALTADDRESS, 0);
   EEPROM.write(DEBUG_LEDSADDRESS, 0);
   EEPROM.write(LEDBRIGHTNESSADDRESS, DEFAULTBRIGHTNESS);
   EEPROM.write(RAILBRIGHTNESSADDRESS, DEFAULTRAILBRIGHTNESS);
   EEPROM.write(SPECIALBRIGHTNESSADDRESS, DEFAULTSPECIALNETBRIGHTNESS);
   EEPROM.write(PROBESWAPADDRESS, 0);
   EEPROM.write(ROTARYENCODER_MODE_ADDRESS, 0);
   EEPROM.write(DISPLAYMODE_ADDRESS, 1);
   EEPROM.write(NETCOLORMODE_ADDRESS, 0);
   EEPROM.write(MENUBRIGHTNESS_ADDRESS, 100);
   EEPROM.write(PATH_DUPLICATE_ADDRESS, 2);
   EEPROM.write(DAC_DUPLICATE_ADDRESS, 0);
   EEPROM.write(POWER_DUPLICATE_ADDRESS, 2);
   EEPROM.write(DAC_PRIORITY_ADDRESS, 1);
   EEPROM.write(POWER_PRIORITY_ADDRESS, 1);
   EEPROM.write(SHOW_PROBE_CURRENT_ADDRESS, 0);

   saveVoltages(0.0f, 0.0f, 3.33f, 0.0f);

*/