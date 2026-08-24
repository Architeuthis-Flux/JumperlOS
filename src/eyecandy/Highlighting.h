#ifndef HIGHLIGHTING_H
#define HIGHLIGHTING_H

#include <Arduino.h>
#include "JumperlessDefines.h"
#include "JumperlOS.h"
#include "LEDs.h"
#include "NetManager.h"
#include "RotaryEncoder.h"

/**
 * @brief Highlighting system service - manages visual LED feedback
 * 
 * Handles net highlighting, brightening, and warning animations
 * based on probe readings and user interactions.
 */
class Highlighting : public Service {
public:
    // Get singleton instance
    static Highlighting& getInstance();
    
    // Prevent copying
    Highlighting(const Highlighting&) = delete;
    Highlighting& operator=(const Highlighting&) = delete;
    
    // Service interface
    ServiceStatus service() override;
    const char* getName() const override { return "Highlighting"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    
    // Member variables (previously globals)
    rgbColor highlightedOriginalColor;
    rgbColor brightenedOriginalColor;  
    rgbColor warningOriginalColor;
    
    int firstConnection = -1;
    
    int showReadingRow = -1;
    int showReadingNet = -1;
    int highlightedRow = -1;
    int lastNodeHighlighted = -1;
    int lastPrintedNet = -1;

    int currentHighlightedNode = 0;
    
    int warningRow = -1;
    int warningNet = -1;
    unsigned long warningTimeout = 0;
    unsigned long warningTimer = 0;
    
    unsigned long highlightTimer = 0;
    
    int highlightedNet = -1;
    int probeConnectHighlight = -1;
    int brightenedNode = -1;
    int brightenedNet = -1;
    int brightenedRail = -1;
    int brightenedAmount = 20;
    int brightenedNodeAmount = 400;
    int brightenedNetAmount = 150;
    
    int lastHighlightedNet = -1;
    int lastBrightenedNet = -1;
    int lastWarningNet = -1;
    
    // Public methods
    void clearHighlighting(int updateLEDs = 0);
    void resetReadingState(void);
    int encoderNetHighlight(int print = 1, int mode = 1, int divider = 4);
    int brightenNet(int node, int addBrightness = 5);
    int warnNet(int node);
    void warnNetTimeout(int clearAll = 1);
    int highlightNets(int probeReading, int encoderNetHighlighted = -1, int print = 1);
    int checkForReadingChanges(void);
    
    // Persistent node actions
    bool shouldPersistHighlight(int node);
    bool wantsToHandleButtonPress(void);  // Returns true if button press should go to voltage adjustment
    int handleEncoderButtonPress(void);   // Returns 1 if handled, 0 if not
    
private:
    Highlighting();
    ~Highlighting() = default;
    // Helper for voltage adjustment
    void adjustRailVoltage(int rail);  // 0=both, 1=top, 2=bottom
    void adjustDACVoltage(int dac);    // 0=DAC0, 1=DAC1
};

// Backward compatibility - global references to singleton members
extern rgbColor& highlightedOriginalColor;
extern rgbColor& brightenedOriginalColor;
extern rgbColor& warningOriginalColor;
extern int& firstConnection;
extern int& showReadingRow;
extern int& showReadingNet;
extern int& highlightedRow;
extern int& lastNodeHighlighted;
extern int& highlightedNet;
extern int& probeConnectHighlight;
extern int& brightenedNode;
extern int& brightenedNet;
extern int& brightenedRail;
extern int& brightenedAmount;
extern int& brightenedNodeAmount;
extern int& brightenedNetAmount;
extern int& warningRow;
extern int& warningNet;
extern unsigned long& warningTimeout;
extern unsigned long& warningTimer;
extern unsigned long& highlightTimer;

// Legacy wrapper functions
inline void clearHighlighting(int updateLEDs = 01) {
    Highlighting::getInstance().clearHighlighting(updateLEDs);
}

inline int encoderNetHighlight(int print = 1, int mode = 1, int divider = 4) {
    return Highlighting::getInstance().encoderNetHighlight(print, mode, divider);
}

inline int highlightNets(int probeReading, int encoderNetHighlighted = -1, int print = 1) {
    return Highlighting::getInstance().highlightNets(probeReading, encoderNetHighlighted, print);
}

inline void warnNetTimeout(int clearAll = 1) {
    Highlighting::getInstance().warnNetTimeout(clearAll);
}

inline int checkForReadingChanges(void) {
    return Highlighting::getInstance().checkForReadingChanges();
}

inline int warnNet(int node) {
    return Highlighting::getInstance().warnNet(node);
}

inline int brightenNet(int node, int addBrightness = 5) {
    return Highlighting::getInstance().brightenNet(node, addBrightness);
}

#endif // HIGHLIGHTING_H
