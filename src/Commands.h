#ifndef COMMANDS_H
#define COMMANDS_H

#include <Arduino.h>    
// (sendAllPathsCore2 is gone (T2.2b): see CoreMailbox.h - core1req::REQ_SEND / REQ_BYPASS.
//  showLEDsCore2 is gone (T2.2c): the LED-show request is core1req::REQ_SHOW_LEDS.)
//
// requestLedShow(): ask core 1 to refresh the LEDs. The argument keeps the
// vocabulary every caller already spoke, so the ~270 sites read exactly as
// before:  1 = show the netlist (showNets + measurements + animations +
// overlays), 2 = menu / text-buffer flush (immediate), 3 = staged graphics
// keep the strip (immediate; ownership sticks until another mode is
// requested - ledGraphicsOwned()), negative = clear the breadboard first, +10 =
// blocking show (an atomic frame; e.g. 12), 0 = cancel a pending request.
// A later mode replaces an earlier pending one (last write wins, as the int
// did); clear-first and blocking coalesce. Returns the request generation
// (core1req::isDone(REQ_SHOW_LEDS, gen) - or just waitCore2()).
uint32_t requestLedShow( int legacyValue );
// True when no LED show is pending or in flight and staged graphics do not
// own the strip - what "showLEDsCore2 == 0" used to say.
bool ledShowIdle( void );
// True while staged graphics (mode 3) own the strip.
bool ledGraphicsOwned( void );
// For X: the pending bits of the LED slot.
uint32_t ledShowPendingBits( void );
// For X: the last 16 LED requests core 1 took (main.cpp fills, X prints).
struct LedTakeLog { uint32_t t; uint8_t bits, rails, menu, shown; uint16_t repeats; };   // consecutive identical takes collapse into one entry (+repeats)
extern volatile LedTakeLog ledTakeLog[ 32 ];
extern volatile uint8_t ledTakeLogIdx;

extern volatile int showProbeLEDs;


// Guard flags to prevent auto-save during command processing (prevents deadlock)
extern volatile bool refreshLocalInProgress;
extern volatile bool refreshInProgress;

struct rowLEDs {
  uint32_t color[5];

};
void refreshBlind(int disconnectFirst = -1, int fillUnused = 0, int clean = 0);

unsigned long waitCore2(void);

void refresh(int flashOrLocal = 0, int ledShowOption = -1, int fillUnused = 1, int clean = 0);

void refreshConnections(int ledShowOption = 1,int fillUnused = 1, int clean = 0);
void refreshLocalConnections(int ledShowOption = 1, int fillUnused = 1, int clean = 0);
void fastRefresh(int ledShowOption = 1);

// Check and restore all locked connections from config
// Call after clear, load, or any operation that might remove connections
// Returns number of locked connections added (0 if all were already present)

void updateLEDs(void);

bool checkFloating(int node);
struct rowLEDs getRowLEDdata (int row);
void setRowLEDdata (int row, struct rowLEDs);

void connectNodes(int node1, int node2);

void disconnectNodes(int node1, int node2);

float measureVoltage(int adcNumber, int node, bool checkForFloating = false);
float measureCurrent(int node1, int node2);

void setRailVoltage(int topBottom, float voltage);

void connectGPIO(int gpioNumber, int node);
























#endif