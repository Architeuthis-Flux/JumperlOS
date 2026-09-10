// SPDX-License-Identifier: MIT
//
// Board selection + board-agnostic routing primitives. Host-compilable: no
// Arduino dependency (see test/test_boards/).

#include "board.h"

namespace board {

const BoardTopology &currentBoard() {
#if defined(OG_JUMPERLESS)
  return ogBoardTopology;
#else
  return v5BoardTopology;
#endif
}

int boardY0Node(const BoardTopology &b) { return b.y0Node; }

bool boardRowToChipY(const BoardTopology &b, int row, int *chip, int *y) {
  // Search every chip's Y lanes for this breadboard row. Y0 is skipped because
  // it is the bounce bus / chip-L hub, not a routable hole.
  for (int c = 0; c < kChipCount; c++) {
    for (int yi = 1; yi < kYLanes; yi++) {
      if (b.yMap[c][yi] == row) {
        if (chip) *chip = c;
        if (y) *y = yi;
        return true;
      }
    }
  }
  return false;
}

const GpioEntry *boardFindGpio(const BoardTopology &b, int node) {
  for (uint8_t i = 0; i < b.gpioCount; i++) {
    if (b.gpio[i].node == node) return &b.gpio[i];
  }
  return nullptr;
}

const AnalogChannel *boardFindAdc(const BoardTopology &b, int node) {
  for (uint8_t i = 0; i < b.adcCount; i++) {
    if (b.adc[i].node == node) return &b.adc[i];
  }
  return nullptr;
}

const AnalogChannel *boardFindDac(const BoardTopology &b, int node) {
  for (uint8_t i = 0; i < b.dacCount; i++) {
    if (b.dac[i].node == node) return &b.dac[i];
  }
  return nullptr;
}

bool boardCanSetRailVoltage(const BoardTopology &b) {
  return b.caps.railsFirmwareControlled;
}

bool boardHasNode(const BoardTopology &b, int node) {
  return boardFindGpio(b, node) || boardFindAdc(b, node) || boardFindDac(b, node);
}

// Minimal append helper so this stays printf/Arduino-free and bounds-checked.
namespace {
struct Appender {
  char *buf;
  size_t cap;
  size_t len = 0;
  bool ok = true;
  void s(const char *p) {
    while (*p) {
      if (len + 1 >= cap) { ok = false; return; }
      buf[len++] = *p++;
    }
  }
  void b(const char *key, bool v) {
    s("\"");
    s(key);
    s("\":");
    s(v ? "true" : "false");
  }
  void i(const char *key, long v) {
    s("\"");
    s(key);
    s("\":");
    char tmp[16];
    int n = 0;
    if (v < 0) { s("-"); v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) {
      if (len + 1 >= cap) { ok = false; return; }
      buf[len++] = tmp[n];
    }
  }
};
} // namespace

const char *boardGpioName(const BoardTopology &b, int gpio) {
  if (!b.gpioNames || gpio < 0 || gpio >= (int)b.gpioNameCount) return "";
  const char *n = b.gpioNames[gpio];
  return n ? n : "";
}

size_t boardCapabilitiesJson(const BoardTopology &b, char *buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  Appender a{buf, cap};
  a.s("{\"board\":\"");
  a.s(b.name);
  a.s("\",");
  a.b("rails_firmware_controlled", b.caps.railsFirmwareControlled);
  a.s(",");
  a.b("probe_pads", b.caps.hasProbePads);
  a.s(",");
  a.b("scanning_probe", b.caps.scanningProbe);
  a.s(",");
  a.b("rotary_encoder", b.caps.hasRotaryEncoder);
  a.s(",");
  a.b("oled", b.caps.hasOled);
  a.s(",");
  a.b("breadboard_text", b.caps.hasBreadboardText);
  a.s(",");
  a.b("psram", b.caps.hasPsram);
  a.s(",");
  a.b("startup_animation", b.caps.hasStartupAnimation);
  a.s(",");
  a.b("spi_dac", b.caps.spiDac);
  a.s(",");
  a.i("leds_per_row", b.caps.ledsPerRow);
  a.s(",");
  a.i("led_count", b.caps.ledCount);
  a.s(",");
  a.i("usb_cdc_count", b.caps.usbCdcCount);
  a.s(",");
  a.i("uart_tx_pin", b.caps.uartTxPin);
  a.s(",");
  a.i("uart_rx_pin", b.caps.uartRxPin);
  a.s(",");
  a.i("mp_c_heap_reserve_kb", b.caps.mpCHeapReserveKb);
  a.s(",");
  a.i("gpio_count", b.gpioCount);
  a.s(",");
  a.i("adc_count", b.adcCount);
  a.s(",");
  a.i("dac_count", b.dacCount);
  a.s("}");
  if (!a.ok) {
    buf[0] = '\0';
    return 0;
  }
  buf[a.len] = '\0';
  return a.len;
}

} // namespace board
