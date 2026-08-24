// SPDX-License-Identifier: MIT

#ifndef JUMPERLESSDEFINES_H
#define JUMPERLESSDEFINES_H
// #include "SerialWrapper.h"
// #define Serial SerialWrap

// #define OG_JUMPERLESS 0

// MicroPython heap sizes
#if defined(OG_JUMPERLESS)
// RP2040 has only 264 KB SRAM and no PSRAM, shared with the whole firmware.
// MicroPython is lazy-initialized (first REPL use), so this heap is malloc'd at
// runtime FROM THE C HEAP and does NOT affect static RAM/boot. Registering the
// native `jumperless` module permanently consumes ~12 KB of the GC heap, so the
// effective per-script budget is (HEAP - 12 KB).
//
// Sizing history: 16 KB OOM'd on init scripts (<4 KB free → "MemoryError:
// allocating 4168 bytes"); 24 KB once reboot-looped because it left only
// ~6.5 KB C heap — BUT that was measured at 79.9% static RAM. A later reclaim
// pass dropped static to ~63.7% (~16.6 KB more free: uncompressed startup
// frames, undo group off, dead newBridges deleted, rowAnimations + CDC FIFOs
// shrunk), so the C-heap math changed and that 24 KB finding is now stale —
// there is headroom to enlarge the GC heap without starving the C heap.
// Bumped 20 → 28 KB so a SECOND script run (whose module-level globals from the
// first run linger until GC) doesn't blow the ~8 KB script budget and OOM.
// WHY THIS MATTERS AS A CRASH, NOT A TRACEBACK: on OG an unhandled MemoryError
// doesn't always print cleanly — it can cascade through nlr_jump_fail and reset
// core 0 (USB drops), so undersizing this heap looks like a hard disconnect
// mid-script, not a tidy MemoryError. rp2.py provisioning (a ~10 KB C-heap
// spike) is skipped on OG so it can't collide with the larger heap.
//
// If the contiguous malloc ever fails (fragmented C heap), initMicroPythonProper
// prints "[MP] FATAL: failed to malloc" and bails cleanly — it does not crash.
// ponytail: ceiling is total SRAM split between this GC heap and the C heap;
// grow further only by reclaiming more V5-only static RAM (logo palettes, menu
// buffers, globalState - see OG_BACKPORT.md), NOT by taking more from C heap.
#define MICROPY_HEAP_SIZE       (28 * 1024)
#define MICROPY_HEAP_SIZE_PSRAM (28 * 1024)
#else
#define MICROPY_HEAP_SIZE       (64 * 1024)  // SRAM heap when no PSRAM
#define MICROPY_HEAP_SIZE_PSRAM  (64 * 1024)  // Smaller SRAM heap when PSRAM provides extra GC space
#endif

// ===========================================================================
// Feature groups (compile-time subsystem toggles)
// ===========================================================================
// Two distinct gating mechanisms exist in this firmware; use the right one:
//
//   * Feature-group flags (HERE): ONE flag per subsystem. When 0, the WHOLE
//     group is compiled out - its code AND its static buffers. This is the
//     only mechanism that actually frees .bss/heap (the linker drops the
//     symbols), so it is what we use to claw back RAM on the RP2040 OG. The
//     subsystem's header provides no-op stubs when its flag is 0, so call
//     sites compile unchanged with no per-caller #ifdef.
//   * Runtime BoardCaps (src/boards/board.h): gate compiled-IN hardware or
//     behavior at runtime (probe, encoder, PSRAM cache, boot animation). Does
//     NOT free .bss - both code paths stay linked. Use it for the board
//     contract / honest capability advertisement, not for RAM reclaim.
//
// Each flag defaults to 1 (full firmware) and is forced to 0 for the OG, where
// the subsystem is either unaffordable on the ~30 KB free RAM or meaningless on
// the hardware. Adding a new group: define it here, wrap its .cpp body in
// `#if <FLAG>` with an `#else` stub block matching its header API.
#if defined(OG_JUMPERLESS)
// Undo/redo (delta ring + persisted /undo.hist + OLED toast scene graph). Its
// ring buffers are heap-allocated lazily (already skipped on OG), but the OLED
// undo-toast carries a permanent ~3.2 KB static OledScreen; gating the group
// off reclaims it and removes the dead code path entirely. See Undo.cpp/Undo.h.
#define UNDO_ENABLED 0
#else
#define UNDO_ENABLED 1
#endif

// Boot-time Serial chatter (PSRAM arena sizing, undo ring init, heap ledger,
// file-cache pass-through notice). Off in release (NDEBUG) builds; on in debug
// builds and when -DDEBUG is set (jumperless_v5_debug env).
#if defined(DEBUG) || !defined(NDEBUG)
#define JL_BOOT_VERBOSE 1
#else
#define JL_BOOT_VERBOSE 0
#endif

#define TERM_SUPPORTS_RGB 0
#define TERM_SUPPORTS_ANSI_COLORS 1

#define TERM_COLOR_PATHS 1
#define TERM_COLOR_CROSSBARS 1
#define TERM_COLOR_CHIP_STATUS 1
#define TERM_COLOR_MAIN_MENU 1
#define TERM_COLOR_NETS 1

#define PICO_RP2350A 0
#define INPUTBUFFERLENGTH 4000

#define PROTOTYPE_VERSION 3 //1 is the hw without the rearranged probe connections



#define REV 5
#define PROBE_REV 5

extern int rev;
extern int probeRev;


#define PIOSTUFF 1 // comment these out to remove them for debugging
#define EEPROMSTUFF 1
#define FSSTUFF 1


#define QUADRATURE_A_PIN 12
#define QUADRATURE_B_PIN 13
#define BUTTON_ENC 11

#define PROBE_PIN 10
#define BUTTON_PIN 9

#define LDAC 8



#define REVISIONADDRESS 24
#define PROBE_REVISIONADDRESS 25
#define SHOW_PROBE_CURRENT_ADDRESS 26
#define MENUBRIGHTNESS_ADDRESS 27
#define NETCOLORMODE_ADDRESS 29
#define DISPLAYMODE_ADDRESS 30
#define ROTARYENCODER_MODE_ADDRESS 31
#define DEBUG_FILEPARSINGADDRESS 32
#define TIME_FILEPARSINGADDRESS 33
#define DEBUG_NETMANAGERADDRESS 34
#define TIME_NETMANAGERADDRESS 35
#define DEBUG_LEDSADDRESS 36
#define DEBUG_NETTOCHIPCONNECTIONSADDRESS 37
#define DEBUG_NETTOCHIPCONNECTIONSALTADDRESS 38
#define LEDBRIGHTNESSADDRESS 39
#define RAILBRIGHTNESSADDRESS 40
#define SPECIALBRIGHTNESSADDRESS 41
#define PROBESWAPADDRESS 42

#define FIRSTSTARTUPADDRESS 43

// Packed identity+calibration store (see EepromStore in PersistentStuff.h).
// Placed well clear of the legacy per-setting byte map above (which now exists
// only so eepromStoreLoadAndApplyIdentity() can migrate the old revision /
// probe-revision bytes). 256..~400 of the 512-byte EEPROM sector.
#define EEPROM_STORE_ADDRESS 256

#define TOP_RAIL_ADDRESS0 44
#define BOTTOM_RAIL_ADDRESS0 52

#define LOGO_TOP_ADDRESS0 60
#define LOGO_TOP_ADDRESS1 64

#define LOGO_BOTTOM_ADDRESS0 68
#define LOGO_BOTTOM_ADDRESS1 72

#define BUILDING_TOP_ADDRESS0 76
#define BUILDING_TOP_ADDRESS1 80

#define BUILDING_BOTTOM_ADDRESS0 84
#define BUILDING_BOTTOM_ADDRESS1 88

#define DAC0_ADDRESS0 94
#define DAC1_ADDRESS0 102



#define DAC0_SPREAD_ADDRESS 110
#define DAC1_SPREAD_ADDRESS 118
#define TOP_RAIL_SPREAD_ADDRESS 126
#define BOTTOM_RAIL_SPREAD_ADDRESS 134

#define DAC0_ZERO_ADDRESS 142
#define DAC1_ZERO_ADDRESS 146
#define TOP_RAIL_ZERO_ADDRESS 150
#define BOTTOM_RAIL_ZERO_ADDRESS 154

#define CALIBRATED_ADDRESS 158

#define PATH_DUPLICATE_ADDRESS 160
#define POWER_DUPLICATE_ADDRESS 161
#define DAC_DUPLICATE_ADDRESS 162
#define POWER_PRIORITY_ADDRESS 163
#define DAC_PRIORITY_ADDRESS 164

// USB Mode Configuration
#define USB_MODE_ADDRESS 165

#define MAX_NETS 60
// JumperlessState is dominated by nets[MAX_NETS] (each holds nodes[MAX_NODES] +
// bridges[MAX_NODES][2]) and paths[MAX_BRIDGES]. On V5 that's ~50 KB. The RP2040
// (OG) has only ~50 KB of TOTAL free RAM (heap+stack) after static, so it cannot
// hold a second copy of the state (the slot-load / migration / preview paths in
// States.cpp copy it). Shrinking MAX_BRIDGES/MAX_NODES on OG cuts globalState
// AND every copy of it, both to free heap and to keep state copies survivable.
// MAX_NETS stays 60 (netNameConstants[] is initialized with 60 entries).
#if defined(OG_JUMPERLESS)
#define MAX_BRIDGES 72
#define MAX_NODES 24 //this is the max number of nodes that can be connected to a net
#else
#define MAX_BRIDGES 128
#define MAX_NODES 40 //this is the max number of nodes that can be connected to a net
#endif
#define MAX_DNI 8 // max number of doNotIntersect rules
#define MAX_DUPLICATE 8 // max number of duplicates

#define MAX_NETS_FOR_WIRES 18
#define MAX_PATHS_FOR_WIRES 38

#define AIRCR_Register (*((volatile uint32_t *)(PPB_BASE + 0x0ED0C)))

#define ENC_A 12
#define ENC_B 13
#define QUADRATURE_A_PIN 12
#define QUADRATURE_B_PIN 13
#define BUTTON_ENC 11

#define BOUNCE_NODE 199
#define CHIP_A 0
#define CHIP_B 1
#define CHIP_C 2
#define CHIP_D 3
#define CHIP_E 4
#define CHIP_F 5
#define CHIP_G 6
#define CHIP_H 7
#define CHIP_I 8
#define CHIP_J 9
#define CHIP_K 10
#define CHIP_L 11

// #define CS_A 6
// #define CS_B 7
// #define CS_C 8
// #define CS_D 9
// #define CS_E 10
// #define CS_F 11
// #define CS_G 12
// #define CS_H 13

// #define CS_I 20
// #define CS_J 21
// #define CS_K 22
// #define CS_L 23

// #define CS_A_EX 0b0000000010000000
// #define CS_B_EX 0b0000000001000000
// #define CS_C_EX 0b0000000000100000
// #define CS_D_EX 0b0000000000010000
// #define CS_E_EX 0b0000000000001000
// #define CS_F_EX 0b0000000000000100
// #define CS_G_EX 0b0000000000000010
// #define CS_H_EX 0b0000000000000001
// #define CS_I_EX 0b1000000000000000
// #define CS_J_EX 0b0100000000000000
// #define CS_K_EX 0b0010000000000000
// #define CS_L_EX 0b0001000000000000

// #define DATAPIN 14
#if defined(OG_JUMPERLESS)
#define RESETPIN 24
#else
#define RESETPIN 16
#endif
// #define CLKPIN 15

#define UART0_TX 0
#define UART0_RX 1

#define NANO_I2C1_SCL 2 
#define NANO_I2C1_SDA 3

#define I2C0_SDA 4
#define I2C0_SCL 5

// I2C0 (GPIO 4/5) is shared by the MCP4728 DAC, both INA219 current sensors
// and, on a rev-7 board, the OLED. The bus has exactly ONE clock and this is
// it: initDAC() sets it at boot, the OLED-on-I2C0 driver drops to its own
// 400 kHz only for the duration of a panel transfer and hands the bus back at
// this rate (clkAfter), and nothing else calls Wire.setClock(). Before this
// (T1.9) the clock had three owners and was dynamic - 1.7 MHz after every
// wavegen_start() (MCP4728::begin), 400 kHz after every OLED frame - so the
// INA219s and the WaveGen stream ran at whichever the last of those left.
// 1 MHz, not 1.7: the INA219s silently failed reads at 1.7 MHz (see initDAC);
// WaveGen's B_PER_SAM sample-rate model was calibrated at ~1 MHz.
#define I2C0_BUS_CLOCK_HZ 1000000

#define RP6_PIN 6
#define RP7_PIN 7

#define LED_DATA_OUT 17

#define ADC0_PIN 40
#define ADC1_PIN 41
#define ADC2_PIN 42
#define ADC3_PIN 43
#define ADC4_PIN 44
#define ADC5_PIN 45
#define ADC6_PIN 46
#define ADC7_PIN 47

#define T_RAIL_POS 31
#define T_RAIL_NEG 0
#define B_RAIL_POS 63
#define B_RAIL_NEG 32

#define TOP_1 1
#define TOP_2 2
#define TOP_3 3
#define TOP_4 4
#define TOP_5 5
#define TOP_6 6
#define TOP_7 7
#define TOP_8 8
#define TOP_9 9
#define TOP_10 10
#define TOP_11 11
#define TOP_12 12
#define TOP_13 13
#define TOP_14 14
#define TOP_15 15
#define TOP_16 16
#define TOP_17 17
#define TOP_18 18
#define TOP_19 19
#define TOP_20 20
#define TOP_21 21
#define TOP_22 22
#define TOP_23 23
#define TOP_24 24
#define TOP_25 25
#define TOP_26 26
#define TOP_27 27
#define TOP_28 28
#define TOP_29 29
#define TOP_30 30

#define BOTTOM_1 31
#define BOTTOM_2 32
#define BOTTOM_3 33
#define BOTTOM_4 34
#define BOTTOM_5 35
#define BOTTOM_6 36
#define BOTTOM_7 37
#define BOTTOM_8 38
#define BOTTOM_9 39
#define BOTTOM_10 40
#define BOTTOM_11 41
#define BOTTOM_12 42
#define BOTTOM_13 43
#define BOTTOM_14 44
#define BOTTOM_15 45
#define BOTTOM_16 46
#define BOTTOM_17 47
#define BOTTOM_18 48
#define BOTTOM_19 49
#define BOTTOM_20 50
#define BOTTOM_21 51
#define BOTTOM_22 52
#define BOTTOM_23 53
#define BOTTOM_24 54
#define BOTTOM_25 55
#define BOTTOM_26 56
#define BOTTOM_27 57
#define BOTTOM_28 58
#define BOTTOM_29 59
#define BOTTOM_30 60

#define NANO_D0 70 // these are completely arbitrary but they should come in handy
#define NANO_D1 71
#define NANO_D2 72
#define NANO_D3 73
#define NANO_D4 74
#define NANO_D5 75
#define NANO_D6 76
#define NANO_D7 77
#define NANO_D8 78
#define NANO_D9 79
#define NANO_D10 80
#define NANO_D11 81
#define NANO_D12 82
#define NANO_D13 83
#define NANO_RESET 84
#define NANO_AREF 85
#define NANO_A0 86
#define NANO_A1 87
#define NANO_A2 88
#define NANO_A3 89
#define NANO_A4 90
#define NANO_A5 91
#define NANO_A6 92
#define NANO_A7 93

#define NANO_RESET_0  94
#define NANO_RESET_1  95

#define NANO_GND_1 96
#define NANO_GND_0 97
#define NANO_3V3 98
#define NANO_5V 99

#define NANO_VIN 69

#define LOGO_PAD_TOP 142
#define LOGO_PAD_BOTTOM 143
#define GPIO_PAD 144
#define DAC_PAD 145
#define ADC_PAD 146
#define BUILDING_PAD_TOP 147
#define BUILDING_PAD_BOTTOM 148

// #define LOGO_PAD_TOP 122
// #define LOGO_PAD_BOTTOM 129
// #define GPIO_PAD 130
// #define DAC_PAD 131
// #define ADC_PAD 132
// #define BUILDING_PAD_TOP 133
// #define BUILDING_PAD_BOTTOM 134


#define GND 100
#define TOP_RAIL 101
#define BOTTOM_RAIL 102
#define SUPPLY_3V3 103
#define SUPPLY_5V 105

#define TOP_RAIL_GND 104
#define BOTTOM_RAIL_GND 126

#define DAC0 106
#define DAC1 107

#define ISENSE_PLUS 108
#define ISENSE_MINUS 109

#define ADC0 110
#define ADC1 111
#define ADC2 112
#define ADC3 113
#define ADC4 114
#define ADC4_5V 114
// #define ADC5 115
// #define ADC5_PROBE_SENSE 115
// #define ADC6 115
// #define ADC6_POWER_MONITOR 115
#define ADC7 115
#define ADC7_PROBE 115

#define RP_GPIO_0 114

#define ARDUINO_RESET_0_PIN 18
#define ARDUINO_RESET_1_PIN 19 //! PSRAM

#define PSRAM_CS_PIN 19




#define RP_GPIO_18 118 // these are the reset lines for the arduino
#define RP_GPIO_19 119 // 

#define SUPPLY_8V_P 120 // not actually connected to anything
#define SUPPLY_8V_N 121 // not actually connected to anything


#define RP_UART_TX 116 // also GPIO_16
#define RP_GPIO_16 116 // these are the same as the UART pins

#define RP_UART_RX 117 // also GPIO_17
#define RP_GPIO_17 117 // but if we want to use them as GPIO we should use these names

#define GPIO_TX_PIN 0
#define GPIO_RX_PIN 1

#define RP_GPIO_20 131
#define RP_GPIO_21 132
#define RP_GPIO_22 133
#define RP_GPIO_23 134

#define RP_GPIO_24 135
#define RP_GPIO_25 136
#define RP_GPIO_26 137
#define RP_GPIO_27 138


#define RP_GPIO_1 131
#define RP_GPIO_2 132
#define RP_GPIO_3 133
#define RP_GPIO_4 134

#define RP_GPIO_5 135
#define RP_GPIO_6 136
#define RP_GPIO_7 137
#define RP_GPIO_8 138

#define GPIO_1_PIN 20
#define GPIO_2_PIN 21
#define GPIO_3_PIN 22
#define GPIO_4_PIN 23
#define GPIO_5_PIN 24
#define GPIO_6_PIN 25
#define GPIO_7_PIN 26
#define GPIO_8_PIN 27


// ============================================================================
// Fake GPIO Node Definitions
// ============================================================================

// Fake GPIO Output nodes (8 total) - nodes 150-157
// Used for software-emulated outputs that switch between voltage sources
#define FAKE_GP_OUT_0  150
#define FAKE_GP_OUT_1  151
#define FAKE_GP_OUT_2  152
#define FAKE_GP_OUT_3  153
#define FAKE_GP_OUT_4  154
#define FAKE_GP_OUT_5  155
#define FAKE_GP_OUT_6  156
#define FAKE_GP_OUT_7  157

// Fake GPIO Input nodes (32 total) - nodes 158-189
// Used for software-emulated inputs that read via ADC
#define FAKE_GP_IN_0   158
#define FAKE_GP_IN_1   159
#define FAKE_GP_IN_2   160
#define FAKE_GP_IN_3   161
#define FAKE_GP_IN_4   162
#define FAKE_GP_IN_5   163
#define FAKE_GP_IN_6   164
#define FAKE_GP_IN_7   165
#define FAKE_GP_IN_8   166
#define FAKE_GP_IN_9   167
#define FAKE_GP_IN_10  168
#define FAKE_GP_IN_11  169
#define FAKE_GP_IN_12  170
#define FAKE_GP_IN_13  171
#define FAKE_GP_IN_14  172
#define FAKE_GP_IN_15  173
#define FAKE_GP_IN_16  174
#define FAKE_GP_IN_17  175
#define FAKE_GP_IN_18  176
#define FAKE_GP_IN_19  177
#define FAKE_GP_IN_20  178
#define FAKE_GP_IN_21  179
#define FAKE_GP_IN_22  180
#define FAKE_GP_IN_23  181
#define FAKE_GP_IN_24  182
#define FAKE_GP_IN_25  183
#define FAKE_GP_IN_26  184
#define FAKE_GP_IN_27  185
#define FAKE_GP_IN_28  186
#define FAKE_GP_IN_29  187
#define FAKE_GP_IN_30  188
#define FAKE_GP_IN_31  189

// Fake GPIO helper macros
#define MAX_FAKE_GP_OUT  8
#define MAX_FAKE_GP_IN   32
#define FAKE_GP_OUT_BASE 150
#define FAKE_GP_IN_BASE  158

#define IS_FAKE_GP_OUT(n) ((n) >= FAKE_GP_OUT_0 && (n) <= FAKE_GP_OUT_7)
#define IS_FAKE_GP_IN(n)  ((n) >= FAKE_GP_IN_0 && (n) <= FAKE_GP_IN_31)
#define FAKE_GP_OUT_SLOT(n) ((n) - FAKE_GP_OUT_BASE)
#define FAKE_GP_IN_SLOT(n)  ((n) - FAKE_GP_IN_BASE)

// Special net number for TDM-merged fake GPIO inputs during routing
// All FAKE_GP_IN paths temporarily use this net so they can share paths,
// then get restored to their individual nets after routing completes.
// Must be < MAX_NETS to fit in int8_t yStatus and pass bounds checks.
#define FAKE_GPIO_TDM_NET  (MAX_NETS - 2)  // 58

// Legacy compatibility - map old names to new (will be removed after migration)
#define FAKE_GPIO_1  FAKE_GP_OUT_0
#define FAKE_GPIO_2  FAKE_GP_OUT_1
#define FAKE_GPIO_3  FAKE_GP_OUT_2
#define FAKE_GPIO_4  FAKE_GP_OUT_3
#define FAKE_GPIO_5  FAKE_GP_OUT_4
#define FAKE_GPIO_6  FAKE_GP_OUT_5
#define FAKE_GPIO_7  FAKE_GP_OUT_6
#define FAKE_GPIO_8  FAKE_GP_OUT_7
#define FAKE_GPIO_9  FAKE_GP_IN_0
#define FAKE_GPIO_10 FAKE_GP_IN_1
#define FAKE_GPIO_11 FAKE_GP_IN_2
#define FAKE_GPIO_12 FAKE_GP_IN_3
#define FAKE_GPIO_13 FAKE_GP_IN_4
#define FAKE_GPIO_14 FAKE_GP_IN_5
#define FAKE_GPIO_15 FAKE_GP_IN_6
#define FAKE_GPIO_16 FAKE_GP_IN_7
#define FAKE_GPIO_17 FAKE_GP_IN_8
#define FAKE_GPIO_18 FAKE_GP_IN_9
#define FAKE_GPIO_19 FAKE_GP_IN_10
#define FAKE_GPIO_20 FAKE_GP_IN_11
#define FAKE_GPIO_21 FAKE_GP_IN_12
#define FAKE_GPIO_22 FAKE_GP_IN_13
#define FAKE_GPIO_23 FAKE_GP_IN_14
#define FAKE_GPIO_24 FAKE_GP_IN_15
#define FAKE_GPIO_25 FAKE_GP_IN_16
#define FAKE_GPIO_26 FAKE_GP_IN_17
#define FAKE_GPIO_27 FAKE_GP_IN_18
#define FAKE_GPIO_28 FAKE_GP_IN_19
#define FAKE_GPIO_29 FAKE_GP_IN_20
#define FAKE_GPIO_30 FAKE_GP_IN_21
#define FAKE_GPIO_31 FAKE_GP_IN_22
#define FAKE_GPIO_32 FAKE_GP_IN_23




#define ROUTABLE_BUFFER_IN 139
#define ROUTABLE_BUFFER_OUT 140


#define USER_FUNCTION_WAIT 60000
#define MAX_TOKENS  20
#define MAX_OPTS  20

/*

#define GND  100
#define 3V3  103
#define 5V  105

#define 8V  120

#define DAC0 106
#define DAC1  107

#define I_P  108
#define I_N  109

#define ADC0 110
#define ADC1 111
#define ADC2 112
#define ADC3 113

#define GPIO_0 114

#define UART_TX 116
#define GPIO_16 116  //these are the same as the UART pins

#define UART_RX 117
#define GPIO_17 117  //but if we want to use them as GPIO we should use these names


#define GPIO_18 118  //these aren't actually connected to anything
#define GPIO_19 119  //but we might as well define names for them





*/

#define EMPTY_NET 127

#endif
