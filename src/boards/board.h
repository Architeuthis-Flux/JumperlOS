// SPDX-License-Identifier: MIT
//
// Board-support contract for JumperlOS.
//
// The shared core (NetManager, router, States, MicroPython API, serial) must
// NEVER branch on board #defines. Instead it asks this contract: "what is the
// crossbar topology / LED layout / capability set of the board I'm running
// on?". Each supported board provides ONE `BoardTopology` descriptor (pure
// data) plus the HAL function implementations declared at the bottom of this
// file.
//
// Adding a new board (e.g. the future V6) is mechanical:
//   1. add `src/boards/<board>/board_<board>.cpp` defining a BoardTopology,
//   2. add a `[env:jumperless_<board>]` to platformio.ini that defines the
//      board's select macro and (if a new MCU) the right platform/board,
//   3. wire the macro into the `currentBoard()` switch below.
// No shared-core file needs to change.
//
// This header is intentionally Arduino-free so the descriptors can be unit
// tested on the host (see test/test_boards/). Keep it that way: only plain
// C++ types here.

#ifndef JUMPERLESS_BOARD_H
#define JUMPERLESS_BOARD_H

#include <stdint.h>
#include <stddef.h>

namespace board {

// ---------------------------------------------------------------------------
// Crossbar topology
// ---------------------------------------------------------------------------

// How Y0 of the breadboard chips (A..H) is wired. This is the single biggest
// hardware difference between board generations and the reason the router must
// be topology-driven rather than hard-coded.
//   BounceNode : Y0 is a virtual internal bus (V5). It is not a physical hole;
//                the pathfinder uses it to hop between chips without consuming
//                a breadboard row connection. Chip L's Y-axis selects which
//                breadboard chip to bridge.
//   ChipL      : Y0 connects directly to chip L (OG). Chip L is the literal
//                central hub; there is no bounce concept.
enum class Y0Rule : uint8_t { BounceNode = 0, ChipL = 1 };

// 12 CH446Q crosspoint chips, 16 X lanes x 8 Y lanes each.
static const int kChipCount = 12;
static const int kXLanes = 16;
static const int kYLanes = 8;
// bbNodesToChip is indexed by breadboard row 0..61 (0 and 61 are sentinels).
static const int kBbNodesToChipLen = 62;

// ---------------------------------------------------------------------------
// Explicit peripheral tables
// ---------------------------------------------------------------------------
// These are ENUMERATED, never counted: the OG's lone RP_GPIO_0 must describe
// itself and must never be aliased to GPIO_1. An LLM/MicroPython tool reads
// these tables (via the USBSer3 capability JSON) to discover exactly what the
// board can do.

struct GpioEntry {
  int16_t node;        // logical node id (RP_GPIO_0, RP_UART_TX, ...)
  int8_t physicalPin;  // RP2040/RP2350 GPIO number
  const char *label;   // human/agent facing name
  bool pwmCapable;
};

// One entry per analog channel (ADC input or DAC output). Range is the real
// electrical range so out-of-range requests can be rejected with a reason.
struct AnalogChannel {
  int16_t node;       // ADC0.. / DAC0..
  const char *label;
  float minV;         // e.g. -8.0 for the +/-8V channels, 0.0 for buffered 0-5V
  float maxV;
  bool buffered;      // OG ADC0-2 are buffered 0-5V; ADC3 is raw +/-8V
};

// ---------------------------------------------------------------------------
// Capability flags (binary features only; anything enumerable is a table)
// ---------------------------------------------------------------------------
struct BoardCaps {
  bool railsFirmwareControlled; // OG rails are a hardware switch -> false
  bool hasProbePads;            // resistive touch pads (V5) -> OG false
  bool scanningProbe;           // OG uses crossbar scan probing -> true
  bool hasRotaryEncoder;        // OG false
  bool hasOled;                 // OG false
  bool hasBreadboardText;       // OG can't render text on the breadboard
  bool hasPsram;                // OG false (RP2040)
  bool hasStartupAnimation;     // V5 plays the boot logo animation; OG skips it
                                // (no compressed frames built, 111-LED strip)
  bool breadboardDisplays;      // DisplayService (soft-I2C panels on routed
                                // GPIOs): V5 only - the service hardcodes the
                                // V5 node map (RP 20-27 -> nodes 131-138)
  bool spiDac;                  // OG MCP4822 over SPI; V5 MCP4728 over I2C
  uint8_t ledsPerRow;           // V5 = 5, OG = 1
  uint16_t ledCount;            // total addressable pixels: V5 = 445, OG = 111
  uint8_t usbCdcCount;          // number of USB CDC interfaces exposed
};

struct BoardTopology {
  const char *name; // "jumperless_v5", "jumperless_og"

  Y0Rule y0Rule;
  int16_t y0Node; // node value that lives at yMap[chip][0] for BB chips A..H

  // Crossbar maps. Pointers so a descriptor can reference an existing table
  // without copying it. Shapes: xMap[kChipCount][kXLanes], yMap[kChipCount][kYLanes].
  const int16_t (*xMap)[kXLanes];
  const int16_t (*yMap)[kYLanes];
  const int8_t *bbNodesToChip; // length kBbNodesToChipLen

  const GpioEntry *gpio;
  uint8_t gpioCount;

  const AnalogChannel *adc;
  uint8_t adcCount;

  const AnalogChannel *dac;
  uint8_t dacCount;

  BoardCaps caps;
};

// Both descriptors are always linked so host tests can compare them. The build
// selects which one is "current" via the board macro.
extern const BoardTopology v5BoardTopology;
extern const BoardTopology ogBoardTopology;

const BoardTopology &currentBoard();

// ---------------------------------------------------------------------------
// Topology-driven routing primitives (the seam the unified router builds on)
// ---------------------------------------------------------------------------

// The Y0 node for breadboard chips A..H on this board (BOUNCE_NODE or CHIP_L).
int boardY0Node(const BoardTopology &b);

// Resolve a breadboard row (1..60) to its chip + Y lane. Returns false if the
// row is not reachable. This is the board-agnostic kernel the pathfinder uses
// instead of hard-coded chip math.
bool boardRowToChipY(const BoardTopology &b, int row, int *chip, int *y);

// Look up a GPIO entry by logical node; nullptr if this board doesn't have it.
const GpioEntry *boardFindGpio(const BoardTopology &b, int node);
// Same for analog channels.
const AnalogChannel *boardFindAdc(const BoardTopology &b, int node);
const AnalogChannel *boardFindDac(const BoardTopology &b, int node);

// ---------------------------------------------------------------------------
// Capability advertisement
// ---------------------------------------------------------------------------

// True if the board can do the named operation, so the serial/MicroPython
// layers can return an honest structured error instead of silently lying.
bool boardCanSetRailVoltage(const BoardTopology &b); // OG rails are a switch
bool boardHasNode(const BoardTopology &b, int node); // gpio/adc/dac membership

// Emit the board's capability set as compact JSON into buf (the USBSer3 'A'
// status backchannel surfaces this so LLM tools can discover what the board
// supports without trial and error). Returns bytes written (excluding NUL), or
// 0 if buf is too small. Self-contained: no Arduino / printf dependency.
size_t boardCapabilitiesJson(const BoardTopology &b, char *buf, size_t cap);

} // namespace board

// ---------------------------------------------------------------------------
// HAL function contract (implemented per board in board_<board>.cpp / the
// existing Peripherals/LEDs code). Declared here so the shared core can call
// them without seeing a board macro. Phase 1 wires the data above; the analog
// HAL bodies land in Phase 2.
// ---------------------------------------------------------------------------
// NOTE: these are declarations only. V5 keeps its current implementations in
// Peripherals.cpp / LEDs.cpp; OG implementations live in src/boards/og/.

#endif // JUMPERLESS_BOARD_H
