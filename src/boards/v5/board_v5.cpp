// SPDX-License-Identifier: MIT
//
// Jumperless V5 (RP2350B) board descriptor.
//
// This mirrors the EXISTING V5 topology that currently lives in
// src/MatrixState.cpp (rev5 X map + bounce-node Y map) and src/Peripherals.h
// (gpioDef). It is the reference implementation of the board contract.
//
// IMPORTANT: V5's live routing still runs off src/MatrixState.cpp's ch[] until
// the unified router is hardware-validated (plan Phase 3). This descriptor is
// consumed by the host parity test and by capability advertisement; it does
// NOT change V5 runtime behavior. Keep it in sync with MatrixState.cpp when V5
// cuts over.

#include "../board.h"
#include "JumperlessDefines.h"

namespace board {

// rev5 X map (from src/MatrixState.cpp rev5plusXmap).
static const int16_t kV5XMap[kChipCount][kXLanes] = {
    {CHIP_I, CHIP_J, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E, CHIP_K, CHIP_F, CHIP_F, CHIP_G, CHIP_L, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_I, CHIP_J, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E, CHIP_E, CHIP_F, CHIP_K, CHIP_G, CHIP_G, CHIP_H, CHIP_L},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_I, CHIP_J, CHIP_D, CHIP_D, CHIP_E, CHIP_L, CHIP_F, CHIP_F, CHIP_G, CHIP_K, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_I, CHIP_J, CHIP_E, CHIP_E, CHIP_F, CHIP_L, CHIP_G, CHIP_G, CHIP_H, CHIP_K},
    {CHIP_A, CHIP_K, CHIP_B, CHIP_B, CHIP_C, CHIP_L, CHIP_D, CHIP_D, CHIP_I, CHIP_J, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_K, CHIP_C, CHIP_C, CHIP_D, CHIP_L, CHIP_E, CHIP_E, CHIP_I, CHIP_J, CHIP_G, CHIP_G, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_L, CHIP_B, CHIP_B, CHIP_C, CHIP_K, CHIP_D, CHIP_D, CHIP_E, CHIP_E, CHIP_F, CHIP_F, CHIP_I, CHIP_J, CHIP_H, CHIP_H},
    {CHIP_A, CHIP_A, CHIP_B, CHIP_L, CHIP_C, CHIP_C, CHIP_D, CHIP_K, CHIP_E, CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_I, CHIP_J},
    {NANO_A0, NANO_D1, NANO_A2, NANO_D3, NANO_A4, NANO_D5, NANO_A6, NANO_D7, NANO_D11, NANO_D9, NANO_D13, ISENSE_PLUS, CHIP_L, CHIP_J, CHIP_K, RP_UART_RX},
    {NANO_D0, NANO_A1, NANO_D2, NANO_A3, NANO_D4, NANO_A5, NANO_D6, NANO_A7, NANO_D8, NANO_D10, NANO_D12, ISENSE_MINUS, CHIP_L, CHIP_I, CHIP_K, RP_UART_TX},
    {29, 59, ROUTABLE_BUFFER_IN, NANO_AREF, TOP_RAIL, BOTTOM_RAIL, DAC1, DAC0, ADC0, ADC1, ADC2, ADC3, CHIP_L, CHIP_I, CHIP_J, GND},
    {30, 60, ROUTABLE_BUFFER_OUT, ADC4_5V, RP_GPIO_20, RP_GPIO_21, RP_GPIO_22, RP_GPIO_23, RP_GPIO_24, RP_GPIO_25, RP_GPIO_26, RP_GPIO_27, CHIP_I, CHIP_J, CHIP_K, GND},
};

// Y map: breadboard chips A..H have Y0 = BOUNCE_NODE (the virtual hop bus);
// special-function chips I..L use Y to select breadboard chip A..H.
static const int16_t kV5YMap[kChipCount][kYLanes] = {
    {BOUNCE_NODE, TOP_1, TOP_2, TOP_3, TOP_4, TOP_5, TOP_6, TOP_7},
    {BOUNCE_NODE, TOP_8, TOP_9, TOP_10, TOP_11, TOP_12, TOP_13, TOP_14},
    {BOUNCE_NODE, TOP_15, TOP_16, TOP_17, TOP_18, TOP_19, TOP_20, TOP_21},
    {BOUNCE_NODE, TOP_22, TOP_23, TOP_24, TOP_25, TOP_26, TOP_27, TOP_28},
    {BOUNCE_NODE, BOTTOM_1, BOTTOM_2, BOTTOM_3, BOTTOM_4, BOTTOM_5, BOTTOM_6, BOTTOM_7},
    {BOUNCE_NODE, BOTTOM_8, BOTTOM_9, BOTTOM_10, BOTTOM_11, BOTTOM_12, BOTTOM_13, BOTTOM_14},
    {BOUNCE_NODE, BOTTOM_15, BOTTOM_16, BOTTOM_17, BOTTOM_18, BOTTOM_19, BOTTOM_20, BOTTOM_21},
    {BOUNCE_NODE, BOTTOM_22, BOTTOM_23, BOTTOM_24, BOTTOM_25, BOTTOM_26, BOTTOM_27, BOTTOM_28},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
    {CHIP_A, CHIP_B, CHIP_C, CHIP_D, CHIP_E, CHIP_F, CHIP_G, CHIP_H},
};

// Verbatim from src/MatrixState.cpp (row 1 -> chip A; rows 30/31/60 -> K/L).
static const int8_t kV5BbNodesToChip[kBbNodesToChipLen] = {
    0,
    CHIP_A, CHIP_A, CHIP_A, CHIP_A, CHIP_A, CHIP_A, CHIP_A,
    CHIP_B, CHIP_B, CHIP_B, CHIP_B, CHIP_B, CHIP_B, CHIP_B,
    CHIP_C, CHIP_C, CHIP_C, CHIP_C, CHIP_C, CHIP_C, CHIP_C,
    CHIP_D, CHIP_D, CHIP_D, CHIP_D, CHIP_D, CHIP_D, CHIP_D,
    CHIP_K, CHIP_L,
    CHIP_E, CHIP_E, CHIP_E, CHIP_E, CHIP_E, CHIP_E, CHIP_E,
    CHIP_F, CHIP_F, CHIP_F, CHIP_F, CHIP_F, CHIP_F, CHIP_F,
    CHIP_G, CHIP_G, CHIP_G, CHIP_G, CHIP_G, CHIP_G, CHIP_G,
    CHIP_H, CHIP_H, CHIP_H, CHIP_H, CHIP_H, CHIP_H, CHIP_H,
    CHIP_K, CHIP_L,
};

// 8 routable GPIO + 2 UART-as-GPIO (from src/Peripherals.h gpioDef).
static const GpioEntry kV5Gpio[] = {
    {RP_GPIO_1, GPIO_1_PIN, "GPIO_1", true},
    {RP_GPIO_2, GPIO_2_PIN, "GPIO_2", true},
    {RP_GPIO_3, GPIO_3_PIN, "GPIO_3", true},
    {RP_GPIO_4, GPIO_4_PIN, "GPIO_4", true},
    {RP_GPIO_5, GPIO_5_PIN, "GPIO_5", true},
    {RP_GPIO_6, GPIO_6_PIN, "GPIO_6", true},
    {RP_GPIO_7, GPIO_7_PIN, "GPIO_7", true},
    {RP_GPIO_8, GPIO_8_PIN, "GPIO_8", true},
    {RP_UART_TX, GPIO_TX_PIN, "UART_TX", true},
    {RP_UART_RX, GPIO_RX_PIN, "UART_RX", true},
};

// V5 routable ADC channels (programmable +/-8V routable sense lines).
static const AnalogChannel kV5Adc[] = {
    {ADC0, "ADC0", -8.0f, 8.0f, true},
    {ADC1, "ADC1", -8.0f, 8.0f, true},
    {ADC2, "ADC2", -8.0f, 8.0f, true},
    {ADC3, "ADC3", -8.0f, 8.0f, true},
    {ADC4, "ADC4", 0.0f, 5.0f, true},
    {ADC7, "ADC7",-8.0f, 8.0f, true},
};

// V5 DACs: both programmable +/-8V via MCP4728 (I2C).
static const AnalogChannel kV5Dac[] = {
    {DAC0, "DAC0", -8.0f, 8.0f, true},
    {DAC1, "DAC1", -8.0f, 8.0f, true},
};

const BoardTopology v5BoardTopology = {
    "jumperless_v5",
    Y0Rule::BounceNode,
    BOUNCE_NODE,
    kV5XMap,
    kV5YMap,
    kV5BbNodesToChip,
    kV5Gpio, (uint8_t)(sizeof(kV5Gpio) / sizeof(kV5Gpio[0])),
    kV5Adc, (uint8_t)(sizeof(kV5Adc) / sizeof(kV5Adc[0])),
    kV5Dac, (uint8_t)(sizeof(kV5Dac) / sizeof(kV5Dac[0])),
    {
        /* railsFirmwareControlled */ true,
        /* hasProbePads           */ true,
        /* scanningProbe          */ false,
        /* hasRotaryEncoder       */ true,
        /* hasOled                */ true,
        /* hasBreadboardText      */ true,
        /* hasPsram               */ true,
        /* hasStartupAnimation    */ true,
        /* breadboardDisplays     */ true,
        /* spiDac                 */ false,
        /* ledsPerRow             */ 5,
        /* ledCount               */ 445,
        /* usbCdcCount            */ 4,
    },
};

} // namespace board
