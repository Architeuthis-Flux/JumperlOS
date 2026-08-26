// SPDX-License-Identifier: MIT
//
// OG Jumperless (RP2040) board descriptor.
//
// Ported from the OG reference firmware
// (Jumperless/JumperlessBackport/src/MatrixStateRP2040.cpp). The defining
// differences vs V5:
//   - Y0 of breadboard chips A..H connects directly to CHIP_L (no bounce bus).
//   - rows 1, 30, 31, 60 route through chip L.
//   - only 3 routable GPIO (RP_GPIO_0 + UART TX/RX); RP_GPIO_0 is its OWN node,
//     it is NOT GPIO_1.
//   - 4 ADCs (0-2 buffered 0-5V, ADC3 raw +/-8V); 2 DACs (DAC0 0-5V, DAC1 +/-8V).
//   - rails are a hardware switch (not firmware-controllable), no probe pads,
//     no rotary encoder, no OLED, no breadboard text, no PSRAM.
//
// Node id constants come from the shared JumperlessDefines.h; the 100+ id space
// is compatible between OG and V5 (GND=100, DAC0=106, ADC0=110, RP_GPIO_0=114,
// etc.), which is exactly why one descriptor table can serve both.

#include "../board.h"
#include "JumperlessDefines.h"

namespace board {

// X map: from OG MatrixStateRP2040.cpp ch[] (chips A..L). Chip L is the hub.
static const int16_t kOgXMap[kChipCount][kXLanes] = {
    {CHIP_I, CHIP_J, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E, CHIP_K, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_I, CHIP_J, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E, CHIP_E, CHIP_F, CHIP_K, CHIP_G, CHIP_G, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_I, CHIP_J, CHIP_D, CHIP_D, CHIP_E, CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_K, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_I, CHIP_J, CHIP_E, CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_K},
    {CHIP_A, CHIP_K, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_I, CHIP_J, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_K, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E, CHIP_E, CHIP_I, CHIP_J, CHIP_G, CHIP_G, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_K, CHIP_D, CHIP_D, CHIP_E, CHIP_E, CHIP_F, CHIP_F, CHIP_I, CHIP_J, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_K, CHIP_E, CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_I, CHIP_J},
    {NANO_A0, NANO_D1, NANO_A2, NANO_D3, NANO_A4, NANO_D5, NANO_A6, NANO_D7, NANO_D11, NANO_D9, NANO_D13, NANO_RESET, DAC0, ADC0, SUPPLY_3V3, GND},
    {NANO_D0, NANO_A1, NANO_D2, NANO_A3, NANO_D4, NANO_A5, NANO_D6, NANO_A7, NANO_D8, NANO_D10, NANO_D12, NANO_AREF, DAC1, ADC1, SUPPLY_5V, GND},
    {NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9, NANO_D10, NANO_D11, NANO_D12, ADC2},
    {ISENSE_MINUS, ISENSE_PLUS, ADC0, ADC1, ADC2, ADC3, DAC1, DAC0, TOP_1, TOP_30, BOTTOM_1, BOTTOM_30, RP_UART_TX, RP_UART_RX, SUPPLY_5V, RP_GPIO_0},
};

// Y map: breadboard chips A..H have Y0 = CHIP_L (the hub, no bounce node).
// Rows start at TOP_2 / BOTTOM_2 because the corner rows (1/30/31/60) live on L.
static const int16_t kOgYMap[kChipCount][kYLanes] = {
    {CHIP_L, TOP_2, TOP_3, TOP_4, TOP_5, TOP_6, TOP_7, TOP_8},
    {CHIP_L, TOP_9, TOP_10, TOP_11, TOP_12, TOP_13, TOP_14, TOP_15},
    {CHIP_L, TOP_16, TOP_17, TOP_18, TOP_19, TOP_20, TOP_21, TOP_22},
    {CHIP_L, TOP_23, TOP_24, TOP_25, TOP_26, TOP_27, TOP_28, TOP_29},
    {CHIP_L, BOTTOM_2, BOTTOM_3, BOTTOM_4, BOTTOM_5, BOTTOM_6, BOTTOM_7, BOTTOM_8},
    {CHIP_L, BOTTOM_9, BOTTOM_10, BOTTOM_11, BOTTOM_12, BOTTOM_13, BOTTOM_14, BOTTOM_15},
    {CHIP_L, BOTTOM_16, BOTTOM_17, BOTTOM_18, BOTTOM_19, BOTTOM_20, BOTTOM_21, BOTTOM_22},
    {CHIP_L, BOTTOM_23, BOTTOM_24, BOTTOM_25, BOTTOM_26, BOTTOM_27, BOTTOM_28, BOTTOM_29},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
};

// Verbatim from OG MatrixStateRP2040.cpp (row 1 -> chip L; rows 30/31/60 -> L).
static const int8_t kOgBbNodesToChip[kBbNodesToChipLen] = {
    -1, CHIP_L,
    CHIP_A, CHIP_A, CHIP_A, CHIP_A, CHIP_A, CHIP_A, CHIP_A,
    CHIP_B, CHIP_B, CHIP_B, CHIP_B, CHIP_B, CHIP_B, CHIP_B,
    CHIP_C, CHIP_C, CHIP_C, CHIP_C, CHIP_C, CHIP_C, CHIP_C,
    CHIP_D, CHIP_D, CHIP_D, CHIP_D, CHIP_D, CHIP_D, CHIP_D,
    CHIP_L, CHIP_L,
    CHIP_E, CHIP_E, CHIP_E, CHIP_E, CHIP_E, CHIP_E, CHIP_E,
    CHIP_F, CHIP_F, CHIP_F, CHIP_F, CHIP_F, CHIP_F, CHIP_F,
    CHIP_G, CHIP_G, CHIP_G, CHIP_G, CHIP_G, CHIP_G, CHIP_G,
    CHIP_H, CHIP_H, CHIP_H, CHIP_H, CHIP_H, CHIP_H, CHIP_H,
    CHIP_L,
};

// Exactly 3 routable GPIO. RP_GPIO_0 is its OWN node (114), never aliased to
// GPIO_1. RP_GPIO_0 maps to RP2040 GPIO 0; UART TX/RX are GPIO 17/16 on the RP2040.
static const GpioEntry kOgGpio[] = {
    {RP_GPIO_0, 0, "GPIO_0", true},
    {RP_UART_TX, 17, "UART_TX", true},
    {RP_UART_RX, 16, "UART_RX", true},
};

// 4 ADCs: 0-2 buffered 0-5V, ADC3 raw +/-8V.
static const AnalogChannel kOgAdc[] = {
    {ADC0, "ADC0", 0.0f, 5.0f, true},
    {ADC1, "ADC1", 0.0f, 5.0f, true},
    {ADC2, "ADC2", 0.0f, 5.0f, true},
    {ADC3, "ADC3", -8.0f, 8.0f, true},
};

// 2 DACs: DAC0 0-5V, DAC1 +/-8V (SPI MCP4822).
static const AnalogChannel kOgDac[] = {
    {DAC0, "DAC0", 0.0f, 5.0f, true},
    {DAC1, "DAC1", -8.0f, 8.0f,     true},
};

const BoardTopology ogBoardTopology = {
    "jumperless_og",
    Y0Rule::ChipL,
    CHIP_L,
    kOgXMap,
    kOgYMap,
    kOgBbNodesToChip,
    kOgGpio, (uint8_t)(sizeof(kOgGpio) / sizeof(kOgGpio[0])),
    kOgAdc, (uint8_t)(sizeof(kOgAdc) / sizeof(kOgAdc[0])),
    kOgDac, (uint8_t)(sizeof(kOgDac) / sizeof(kOgDac[0])),
    {
        /* railsFirmwareControlled */ false,
        /* hasProbePads           */ false,
        /* scanningProbe          */ true,
        /* hasRotaryEncoder       */ false,
        /* hasOled                */ false,
        /* hasBreadboardText      */ false,
        /* hasPsram               */ false,
        /* hasStartupAnimation    */ false,
        /* breadboardDisplays     */ false,
        /* spiDac                 */ true,
        /* ledsPerRow             */ 1,
        /* ledCount               */ 111,
        /* usbCdcCount            */ 4,
    },
};

} // namespace board
