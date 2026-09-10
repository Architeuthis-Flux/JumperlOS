// SPDX-License-Identifier: MIT
//
// Host-buildable check for the board-support layer (plan "one runnable check").
//
// Proves the topology-driven primitives give the RIGHT answer for BOTH the OG
// topology (Y0 = CHIP_L) and the V5 topology (Y0 = BOUNCE_NODE), which is what
// lets a single router serve both boards. Also guards the explicit-enumeration
// invariants the plan called out (OG's RP_GPIO_0 must never alias GPIO_1; ADC3
// / DAC1 are the +/-8V channels; capability flags match the hardware).
//
// Build & run (no hardware, no PlatformIO needed):
//   g++ -std=c++17 -I src \
//       test/test_boards/test_boards.cpp \
//       src/boards/board.cpp src/boards/v5/board_v5.cpp src/boards/og/board_og.cpp \
//       -o /tmp/test_boards && /tmp/test_boards

#include "boards/board.h"
#include "JumperlessDefines.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);             \
      failures++;                                                              \
    }                                                                          \
  } while (0)

using namespace board;

static void testY0Rule() {
  CHECK(v5BoardTopology.y0Rule == Y0Rule::BounceNode, "V5 Y0 rule is BounceNode");
  CHECK(ogBoardTopology.y0Rule == Y0Rule::ChipL, "OG Y0 rule is ChipL");
  CHECK(v5BoardTopology.y0Node == BOUNCE_NODE, "V5 y0Node == BOUNCE_NODE");
  CHECK(ogBoardTopology.y0Node == CHIP_L, "OG y0Node == CHIP_L");

  // Every breadboard chip A..H must agree with the rule at Y0.
  for (int c = CHIP_A; c <= CHIP_H; c++) {
    CHECK(v5BoardTopology.yMap[c][0] == BOUNCE_NODE, "V5 BB chip Y0 == BOUNCE_NODE");
    CHECK(ogBoardTopology.yMap[c][0] == CHIP_L, "OG BB chip Y0 == CHIP_L");
  }
  CHECK(boardY0Node(v5BoardTopology) == BOUNCE_NODE, "boardY0Node(V5)");
  CHECK(boardY0Node(ogBoardTopology) == CHIP_L, "boardY0Node(OG)");
}

static void testCornerRows() {
  // Row 1 differs: OG puts it on chip L, V5 on chip A.
  CHECK(ogBoardTopology.bbNodesToChip[1] == CHIP_L, "OG row 1 -> chip L");
  CHECK(v5BoardTopology.bbNodesToChip[1] == CHIP_A, "V5 row 1 -> chip A");
  CHECK(ogBoardTopology.bbNodesToChip[60] == CHIP_L, "OG row 60 -> chip L");
}

static void testRouteKernel() {
  // The board-agnostic kernel must resolve a middle row on each board.
  int chip = -1, y = -1;
  CHECK(boardRowToChipY(ogBoardTopology, TOP_5, &chip, &y), "OG resolves TOP_5");
  CHECK(chip == CHIP_A && y == 4, "OG TOP_5 -> chip A Y4");

  chip = y = -1;
  CHECK(boardRowToChipY(v5BoardTopology, TOP_5, &chip, &y), "V5 resolves TOP_5");
  CHECK(chip == CHIP_A && y == 5, "V5 TOP_5 -> chip A Y5");

  // Y0 nodes must never be returned as routable rows.
  chip = y = -1;
  CHECK(!boardRowToChipY(ogBoardTopology, BOUNCE_NODE, &chip, &y),
        "OG never routes BOUNCE_NODE as a row");
}

static void testGpioNoAlias() {
  CHECK(ogBoardTopology.gpioCount == 3, "OG has exactly 3 routable GPIO");
  CHECK(v5BoardTopology.gpioCount == 10, "V5 has 10 routable GPIO");

  // RP_GPIO_0 exists on OG and is its own node, NOT GPIO_1.
  const GpioEntry *g0 = boardFindGpio(ogBoardTopology, RP_GPIO_0);
  CHECK(g0 != nullptr, "OG exposes RP_GPIO_0");
  CHECK(g0 && g0->node == RP_GPIO_0, "OG RP_GPIO_0 node id correct");
  CHECK(boardFindGpio(ogBoardTopology, RP_GPIO_1) == nullptr,
        "OG does NOT expose RP_GPIO_1 (no aliasing)");

  // All OG gpio nodes distinct.
  for (uint8_t i = 0; i < ogBoardTopology.gpioCount; i++)
    for (uint8_t j = i + 1; j < ogBoardTopology.gpioCount; j++)
      CHECK(ogBoardTopology.gpio[i].node != ogBoardTopology.gpio[j].node,
            "OG gpio nodes are distinct");
}

static void testAnalogRanges() {
  CHECK(ogBoardTopology.adcCount == 4, "OG has 4 ADCs");
  CHECK(ogBoardTopology.dacCount == 2, "OG has 2 DACs");

  // ADC0-2 buffered 0-5V; ADC3 is the raw +/-8V channel.
  const AnalogChannel *a0 = boardFindAdc(ogBoardTopology, ADC0);
  const AnalogChannel *a3 = boardFindAdc(ogBoardTopology, ADC3);
  CHECK(a0 && a0->buffered && a0->minV == 0.0f, "OG ADC0 buffered 0-5V");
  CHECK(a3 && a3->minV < 0.0f, "OG ADC3 is +/-8V (minV < 0)");

  // DAC0 0-5V; DAC1 +/-8V.
  const AnalogChannel *d0 = boardFindDac(ogBoardTopology, DAC0);
  const AnalogChannel *d1 = boardFindDac(ogBoardTopology, DAC1);
  CHECK(d0 && d0->minV == 0.0f, "OG DAC0 is 0-5V");
  CHECK(d1 && d1->minV < 0.0f, "OG DAC1 is +/-8V (minV < 0)");
}

static void testCapabilities() {
  CHECK(ogBoardTopology.caps.railsFirmwareControlled == false,
        "OG rails are NOT firmware controlled");
  CHECK(ogBoardTopology.caps.hasOled == false, "OG has no OLED");
  CHECK(ogBoardTopology.caps.hasRotaryEncoder == false, "OG has no encoder");
  CHECK(ogBoardTopology.caps.hasProbePads == false, "OG has no probe pads");
  CHECK(ogBoardTopology.caps.scanningProbe == true, "OG uses scanning probe");
  CHECK(ogBoardTopology.caps.spiDac == true, "OG uses SPI DAC");
  CHECK(ogBoardTopology.caps.hasPsram == false, "OG has no PSRAM");
  CHECK(ogBoardTopology.caps.hasStartupAnimation == false, "OG skips startup animation");
  CHECK(ogBoardTopology.caps.ledsPerRow == 1, "OG 1 LED per row");
  CHECK(ogBoardTopology.caps.ledCount == 111, "OG 111 LEDs");

  CHECK(v5BoardTopology.caps.railsFirmwareControlled == true,
        "V5 rails ARE firmware controlled");
  CHECK(v5BoardTopology.caps.hasStartupAnimation == true, "V5 plays startup animation");
  CHECK(v5BoardTopology.caps.ledsPerRow == 5, "V5 5 LEDs per row");
  CHECK(v5BoardTopology.caps.ledCount == 445, "V5 445 LEDs total");

  // Nano-header UART: the RP2040/RP2350 mux truth, not the node names.
  CHECK(v5BoardTopology.caps.uartTxPin == 0 && v5BoardTopology.caps.uartRxPin == 1,
        "V5 UART0 on GPIO 0/1");
  CHECK(ogBoardTopology.caps.uartTxPin == 16 && ogBoardTopology.caps.uartRxPin == 17,
        "OG UART0 on GPIO 16 (TX) / 17 (RX)");
  CHECK(ogBoardTopology.caps.uartRxPin != 1, "OG UART must not sit on the DAC CS (GPIO 1)");
  CHECK(v5BoardTopology.caps.mpCHeapReserveKb == 24, "V5 keeps the calibrated 24 KB C-heap reserve");
  CHECK(ogBoardTopology.caps.mpCHeapReserveKb == 12, "OG C-heap reserve 12 KB");

  // GPIO name tables.
  CHECK(v5BoardTopology.gpioNameCount == 48, "V5 names 48 GPIOs");
  CHECK(ogBoardTopology.gpioNameCount == 30, "OG names 30 GPIOs");
  CHECK(std::strcmp(boardGpioName(v5BoardTopology, 9), "PROBE_BUTTON") == 0, "V5 GPIO 9 = PROBE_BUTTON");
  CHECK(std::strcmp(boardGpioName(v5BoardTopology, 19), "NANO_RESET_1") == 0, "V5 GPIO 19 = NANO_RESET_1");
  CHECK(std::strcmp(boardGpioName(ogBoardTopology, 18), "PROBE_BUTTON") == 0, "OG GPIO 18 = PROBE_BUTTON");
  CHECK(std::strcmp(boardGpioName(ogBoardTopology, 1), "DAC_CS") == 0, "OG GPIO 1 = DAC_CS");
  CHECK(std::strcmp(boardGpioName(ogBoardTopology, 16), "UART_TX") == 0, "OG GPIO 16 = UART_TX");
  CHECK(std::strcmp(boardGpioName(ogBoardTopology, 47), "") == 0, "OG GPIO 47 unnamed");
}

static bool contains(const char *hay, const char *needle) {
  for (const char *h = hay; *h; h++) {
    const char *a = h, *b = needle;
    while (*a && *b && *a == *b) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

static void testCapabilityJson() {
  CHECK(boardCanSetRailVoltage(v5BoardTopology) == true, "V5 can set rail voltage");
  CHECK(boardCanSetRailVoltage(ogBoardTopology) == false, "OG cannot set rail voltage");
  CHECK(boardHasNode(ogBoardTopology, RP_GPIO_0), "OG has RP_GPIO_0 node");
  CHECK(!boardHasNode(ogBoardTopology, RP_GPIO_8), "OG lacks RP_GPIO_8 node");
  CHECK(boardHasNode(ogBoardTopology, ADC3), "OG has ADC3 node");

  char buf[512];
  size_t n = boardCapabilitiesJson(ogBoardTopology, buf, sizeof(buf));
  CHECK(n > 0, "OG capability JSON emitted");
  CHECK(contains(buf, "\"board\":\"jumperless_og\""), "JSON names the board");
  CHECK(contains(buf, "\"rails_firmware_controlled\":false"), "JSON reports no firmware rails");
  CHECK(contains(buf, "\"startup_animation\":false"), "JSON reports no startup animation");
  CHECK(contains(buf, "\"gpio_count\":3"), "JSON reports 3 gpio");
  CHECK(contains(buf, "\"led_count\":111"), "JSON reports 111 leds");
  CHECK(contains(buf, "\"uart_tx_pin\":16"), "JSON reports the OG UART TX pin");
  CHECK(contains(buf, "\"mp_c_heap_reserve_kb\":12"), "JSON reports the OG MP C-heap reserve");

  // Bounds safety: a tiny buffer must fail cleanly, not overflow.
  char tiny[8];
  CHECK(boardCapabilitiesJson(ogBoardTopology, tiny, sizeof(tiny)) == 0,
        "capability JSON refuses an undersized buffer");
}

int main() {
  testY0Rule();
  testCornerRows();
  testRouteKernel();
  testGpioNoAlias();
  testAnalogRanges();
  testCapabilities();
  testCapabilityJson();

  if (failures == 0) {
    std::printf("OK: all board-support checks passed\n");
    return 0;
  }
  std::printf("%d board-support check(s) FAILED\n", failures);
  return 1;
}
