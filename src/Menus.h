#pragma once

#include <Arduino.h>
#include "JumperlOS.h"

// Menu states
#define MENU_MAIN 0
#define MENU_NETS 1
#define MENU_PROBING 2
#define MENU_SETTINGS 3
#define MENU_APPS 4
#define MENU_CONFIG 5  // New menu state for configuration

#ifndef MENUS_H
#define MENUS_H

/**
 * @brief Menu system service - handles click menu and menu rendering
 * 
 * Manages all menu-related functionality including click menus,
 * rotary encoder navigation, and menu actions.
 */
class Menus : public Service {
public:
    // Get singleton instance
    static Menus& getInstance();
    
    // Prevent copying
    Menus(const Menus&) = delete;
    Menus& operator=(const Menus&) = delete;
    
    // Service interface
    ServiceStatus service() override;
    const char* getName() const override { return "Menus"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    
    // Member variables (previously globals)
    volatile int inClickMenu = 0; // read by core 1's render every pass - volatile like the other cross-core mode flags
    int menuState = MENU_MAIN;
    int menuPosition = 0;
    int menuScroll = 0;
    int menuScrollTarget = 0;
    int menuScrollMax = 0;
    int menuPositionMax = 0;
    int menuPositionMin = 0;
    int defconDisplay = -1;
    int selectingRotaryNode = 0;
    
    // Public methods
    void initMenu(void);
    int clickMenu(int menuType = -1, int menuOption = -1, int extraOptions = 0);
    
private:
    Menus();
    ~Menus() = default;
};

// Backward compatibility
extern volatile int& inClickMenu;
extern int& defconDisplay;
extern int& selectingRotaryNode;

// Legacy wrapper functions
inline int clickMenu(int menuType = -1, int menuOption = -1, int extraOptions = 0) {
    return Menus::getInstance().clickMenu(menuType, menuOption, extraOptions);
}

extern int& menuState;
extern int& menuPosition;
extern int& menuScroll;
extern int& menuScrollTarget;
extern int& menuScrollMax;
extern int& menuPositionMax;
extern int& menuPositionMin;
enum actionCategories {
  SHOWACTION,
  RAILSACTION,
  SLOTSACTION,
  OUTPUTACTION,
  ARDUINOACTION,
  PROBEACTION,
  DISPLAYACTION,
  APPSACTION,
  ROUTINGACTION,
  OLEDACTION,
  CONNECTACTION,
  CALIBRATIONACTION,
  HISTORYACTION,

  NOCATEGORY
};

void runHistoryScrubMenu(void);

// True while the History scrub menu is live. Core 2's LED loop skips
// showNets() when inClickMenu==1, so we set this flag to override that
// gate so the user sees real bridge LEDs (matching the hold-disconnect
// gesture's behavior on the main screen).
extern volatile bool g_historyScrubActive;


enum showOptions {
  VOLTAGE,
  CURRENT,
  GPIO5V,
  GPIO3V3,
  SHOWUART,
  SHOWI2C,
  NOSHOW
};
enum railOptions { TOP, BOTTOM, BOTH, NORAIL };
enum slotOptions { SAVETO, LOADFROM, CLEAR, NOSLOT };
enum outputOptions {
  VOLTAGE8V,
  VOLTAGE5V,
  DIGITAL5V,
  DIGITAL3V3,
  OUTPUTUART,
  OUTPUTI2C,
  NOOUTPUT
};
enum arduinoOptions { RESET, ARDUINOUART, NOARDUINO };
enum probeOptions { PROBECONNECT, PROBECLEAR, PROBECALIBRATE, NOPROBE };

// struct action {
//   actionCategories Category;
//   showOptions Show;
//   railOptions Rail;
//   slotOptions Slot;
//   outputOptions Output;
//   arduinoOptions Arduino;
//   probeOptions Probe;
//   float floatValue;
//   int intValues[10];
// };

extern int brightnessOptionMap[];
extern int menuBrightnessOptionMap[];
void readMenuFile(int flashOrLocal);
void parseMenuFile(void);

void initMenu(void);
int getMenuSelection(void);
int selectSubmenuOption(int menuPosition, int menuLevel);
int selectNodeAction(int whichSelection = 0);

void printActionStruct(void);
void clearAction(void);
int doMenuAction(int menuPosition = -1, int selection = -1);
void populateAction(void);

enum actionCategories getActionCategory(int menuPosition);

float getActionFloat(int menuPosition, int rail = -1);
int getActionInt(int min, int max, int currentValue = -1);
String getActionString(int maxLength = 32);
String getActionBitmap();

char* findSubMenuString(int menuPosition, int selection);


char printMainMenu(int extraOptions = 0);
char LEDbrightnessMenu();


int findSubMenu(int level, int index);

void showLoss(void);

/**
 * Yes/No prompt on the wheel, the LED matrix and the serial stream at once:
 * click = the highlighted option, turn to change it, hold = cancel, 'y'/'n'
 * answer directly and ANY other byte cancels. Returns 1 = yes, 0 = no,
 * -1 = cancel or timeout.
 *
 * startOption picks which option is highlighted when the prompt opens, i.e.
 * what a bare CLICK answers. It defaults to 0 (No), which is what every
 * pre-existing caller got; pass 1 for a prompt whose wording promises
 * "click = yes" (the project launcher's load-latest and run-script offers -
 * Kevin's control-surface principle: wheel, probe buttons and serial keys must
 * all reach the same answer, and the text must be true for all three).
 */
int yesNoMenu(unsigned long timeout = 4000, int startOption = 0);









#endif