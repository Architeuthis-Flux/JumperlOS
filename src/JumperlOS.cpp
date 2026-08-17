// SPDX-License-Identifier: MIT
#include "JumperlOS.h"
#include "Adafruit_USBD_CDC.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "Highlighting.h"
#include "Menus.h"
#include "Peripherals.h"
#include "States.h"
#include "Jerial.h" // TermControl is now part of Jerial
#include "oled.h"
#include "OledGui.h" // OledGui::tick() for the retained-screen render service
#include "USBfs.h"
#include "AsyncPassthrough.h"
#include "SingleCharCommands.h"
#include "configManager.h"  // For ConfigSaveService
#include "MpRemoteService.h"
#include "Python_Proper.h"   // For isMicroPythonREPLActive()
#include "CH446Q.h"         // For LiveCrossbarService
#include "MatrixState.h"    // For net color access

#ifdef USE_TINYUSB
#include "tusb.h"
#endif

// External debug flags
extern bool debugWaitLoopTiming;

// Static member initialization
jOSmanager* jOSmanager::core1Instance = nullptr;

// Global references for clean syntax (initialized after singletons)
Probing& probing = Probing::getInstance();
Highlighting& highlighting = Highlighting::getInstance();
Menus& menus = Menus::getInstance();
Peripherals& peripherals = Peripherals::getInstance();
SlotManager& slotManager = SlotManager::getInstance();
jOSmanager& jOS = jOSmanager::getInstance();

// System service references
TermSerialService& termSerialService = TermSerialService::getInstance();
InjectedCommandService& injectedCommandService = InjectedCommandService::getInstance();
AsyncPassthroughService& asyncPassthroughService = AsyncPassthroughService::getInstance();
TinyUSBService& tinyUSBService = TinyUSBService::getInstance();
USBPeriodicService& usbPeriodicService = USBPeriodicService::getInstance();
OLEDService& oledService = OLEDService::getInstance();
OledGuiService& oledGuiService = OledGuiService::getInstance();
LiveCrossbarService& liveCrossbarService = LiveCrossbarService::getInstance();
ConfigSaveService& configSaveService = ConfigSaveService::getInstance();

/**
 * @brief Construct a jOSmanager for a specific core
 */
jOSmanager::jOSmanager(uint8_t coreId) 
    : serviceCount(0)
    , coreId(coreId)
    , blockingService(nullptr)
    , loopCounter(0)
    , criticalDivisor(1)
    , highDivisor(1)
    , normalDivisor(3)
    , lowDivisor(20)
{
    // Initialize service array
    for (uint8_t i = 0; i < MAX_SERVICES; i++) {
        services[i].service = nullptr;
        services[i].active = false;
    }
}

jOSmanager::~jOSmanager() {
    // Don't delete services - we don't own them
}

/**
 * @brief Get the Core 1 singleton instance
 */
jOSmanager& jOSmanager::getInstance() {
    if (core1Instance == nullptr) {
        core1Instance = new jOSmanager(1);
    }
    return *core1Instance;
}

/**
 * @brief Register a service with this manager
 */
bool jOSmanager::registerService(Service* service) {
    if (service == nullptr) {
        return false;
    }
    
    if (serviceCount >= MAX_SERVICES) {
        return false;
    }
    
    // Check for duplicates
    for (uint8_t i = 0; i < serviceCount; i++) {
        if (services[i].service == service) {
            return false; // Already registered
        }
    }
    
    // Add to array
    services[serviceCount].service = service;
    services[serviceCount].active = true;
    serviceCount++;
    
    // Re-sort by priority
    sortServicesByPriority();
    
    return true;
}

/**
 * @brief Unregister a service
 */
bool jOSmanager::unregisterService(Service* service) {
    if (service == nullptr) {
        return false;
    }
    
    // Find and remove
    for (uint8_t i = 0; i < serviceCount; i++) {
        if (services[i].service == service) {
            // Shift remaining services down
            for (uint8_t j = i; j < serviceCount - 1; j++) {
                services[j] = services[j + 1];
            }
            serviceCount--;
            
            // Clear blocking if this was the blocking service
            if (blockingService == service) {
                blockingService = nullptr;
            }
            
            return true;
        }
    }
    
    return false;
}

/**
 * @brief Execute all registered services in priority order
 * 
 * Priority-based scheduling:
 * - CRITICAL services run every loop (divisor = 1 typically)
 * - HIGH services run every N loops (configurable, default = 1)
 * - NORMAL services run every M loops (configurable, default = 5)
 * - LOW services run every K loops (configurable, default = 20)
 * 
 * This ensures critical services (button input, menus) are ultra-responsive
 * while less time-sensitive services don't block the main loop.
 * 
 * Service ID mapping (as registered in main.cpp):
 *   0 = probeButton (CRITICAL)
 *   1 = menus (CRITICAL)
 *   2 = slotManager (HIGH)
 *   3 = probing (HIGH)
 *   4 = highlighting (HIGH)
 *   5 = peripherals (NORMAL)
 */
void jOSmanager::serviceAll() {
    loopCounter++;
    //debugWaitLoopTiming = true;

    // Deferred startup complete: wait 4s after init before enabling UART/async
    // passthrough so incoming Arduino data doesn't crash the system during boot
    #if ASYNC_PASSTHROUGH_ENABLED == 1
    {
        extern bool startupCompletePending;
        extern unsigned long startupCompleteRequestTime;
        if (startupCompletePending && (millis() - startupCompleteRequestTime >= 3000)) {
            startupCompletePending = false;
            AsyncPassthrough::signalStartupComplete();
        }
    }
    #endif

    // DEBUG: Track current service for crash debugging
    static const char* currentServiceName = nullptr;
    static uint8_t currentServiceIndex = 255;
    
    // DEBUG: Print service index every 5000 calls to track where freeze happens
    static uint32_t serviceAllCounter = 0;
    serviceAllCounter++;
    bool printServiceDebug = false; //(serviceAllCounter % 5000 == 0);
    
    // DEBUG: Mark start of serviceAll
    if (printServiceDebug) {
        Serial.write('{');
        yield();
    }
    
    // Debug: Print service execution order every N loops
    static unsigned long lastDebugLoop = 0;
    bool printServiceOrder = 0;//debugWaitLoopTiming && (loopCounter % 100 == 0);
    
    if (printServiceOrder && loopCounter != lastDebugLoop) {
        lastDebugLoop = loopCounter;
        Serial.printf("\n=== Service Execution Order (Loop #%lu) ===\n", loopCounter);
    }
    
    unsigned long loopStart = micros( );
    for (uint8_t i = 0; i < serviceCount; i++) {
        if (!services[i].active) {
            if (printServiceOrder) {
                Serial.printf("  [%d] (inactive)\n", i);
            }
            continue;
        }
        
        Service* svc = services[i].service;
        if (svc == nullptr) {
            if (printServiceOrder) {
                Serial.printf("  [%d] (null service)\n", i);
            }
            continue;
        }
        
        // If we're blocked, only allow CRITICAL priority services to run
        if (blockingService != nullptr && 
            blockingService != svc && 
            svc->getPriority() != ServicePriority::CRITICAL) {
            if (printServiceOrder) {
                Serial.printf("  [%d] %s - SKIPPED (blocked by %s)\n", i, svc->getName(), 
                             blockingService->getName());
            }
            continue;
        }
        
        // Priority-based scheduling - skip services based on their priority and divisor
        ServicePriority priority = svc->getPriority();
        bool shouldRun = false;
        const char* priorityName = "UNKNOWN";
        
        switch (priority) {
            case ServicePriority::CRITICAL:
                shouldRun = (loopCounter % criticalDivisor == 0);
                priorityName = "CRITICAL";
                break;
            case ServicePriority::HIGH:
                shouldRun = (loopCounter % highDivisor == 0);
                priorityName = "HIGH";
                break;
            case ServicePriority::NORMAL:
                shouldRun = (loopCounter % normalDivisor == 0);
                priorityName = "NORMAL";
                break;
            case ServicePriority::LOW:
                shouldRun = (loopCounter % lowDivisor == 0);
                priorityName = "LOW";
                break;
        }
        
        if (!shouldRun) {
            if (printServiceOrder) {
                Serial.printf("  [%d] %s (%s) - SKIPPED (scheduled)\n", i, svc->getName(), priorityName);
            }
            continue;  // Skip this service this iteration
        }
        
        if (printServiceOrder) {
            Serial.printf("  [%d] %s (%s) - RUNNING...\n", i, svc->getName(), priorityName);
        }
        
        // DEBUG: Print which service is about to run
        if (printServiceDebug) {
            Serial.write('[');
            Serial.print(i);
            yield();
        }
        
        // Execute the service with timing
        unsigned long svcStart = micros();
        ServiceStatus status = svc->service();
        unsigned long svcEnd = micros();
        unsigned long svcTime = svcEnd - svcStart;
        
        // DEBUG: Print completion marker
        if (printServiceDebug) {
            Serial.write(']');
            yield();
        }
        
        // CRITICAL: Report ANY service taking > 100ms (causes command delays!)
        if (debugWaitLoopTiming && svcTime > 100000) {  // > 100ms
            Serial.printf("⏱️  SLOW SERVICE: %s took %lu ms\n", svc->getName(), svcTime / 1000);
            Serial.flush();
        }
        
        if (debugWaitLoopTiming) {
            if (printServiceOrder) {
                Serial.printf("       └─> completed in %lu us (status=%d)\n", svcTime, (int)status);
            } else if (debugWaitLoopTiming && svcTime > 100000) {
                // Debug: Report if any service takes more than 1ms (even when not printing full order)
                Serial.printf("DEBUG:   Service #%d (%s) took %lu us\n", i, svc->getName(), svcTime);
            }
        }
        
        // Update blocking state
        if (status == ServiceStatus::BLOCKING) {
            if (blockingService != svc) {
                // New blocking service
                blockingService = svc;
                if (debugWaitLoopTiming) {
                    Serial.printf("DEBUG:   Service #%d is now BLOCKING\n", i);
                }
            }
        } else if (blockingService == svc) {
            // This service was blocking but is no longer
            blockingService = nullptr;
            if (debugWaitLoopTiming) {
                Serial.printf("DEBUG:   Service #%d released blocking\n", i);
            }
        }
    }
    if (debugWaitLoopTiming && ( micros() - loopStart ) > 20000 ) {
        Serial.printf("DEBUG:   serviceAll() took %lu us (%.2f ms)\n", micros() - loopStart, (micros() - loopStart) / 1000.0);
        Serial.flush();
    }
    
    // DEBUG: Mark end of serviceAll
    if (printServiceDebug) {
        Serial.write('}');
        yield();
    }
    //debugWaitLoopTiming = false;
}

/**
 * @brief Execute ONLY CRITICAL priority services
 * 
 * This is used within blocking operations (like probeMode) to keep
 * critical services like button checking alive in their inner loops.
 * 
 * Also keeps current sense measurements updating so marching ants
 * visualization stays current during probe mode.
 */
void jOSmanager::serviceCritical() {
    for (uint8_t i = 0; i < serviceCount; i++) {
        if (!services[i].active) {
            continue;
        }
        
        Service* svc = services[i].service;
        if (svc == nullptr) {
            continue;
        }
        
        // Only execute CRITICAL priority services
        if (svc->getPriority() != ServicePriority::CRITICAL) {
            continue;
        }
        
        // Execute the service (ignore status - we're in a blocking context already)
        svc->service();
    }
    
    // (Peripherals is a CRITICAL service, so the loop above already ran its
    // current-sense poll; the explicit second pollCurrentSense() that used
    // to sit here made every serviceCritical() poll twice.)

    // NOTE: MpRemoteService is CRITICAL priority and registered, so the loop
    // above already ran it — the old explicit second call here made it run
    // twice per serviceCritical(). Its own reentrancy guard also defers any
    // USBSer2 REPL processing while a script is executing (Ctrl-C is still
    // caught via mp_hal_check_interrupt's direct stream peek).
}

/**
 * @brief Execute services needed during MicroPython REPL execution
 * 
 * This runs a minimal set of services to keep the system responsive
 * while the main loop is blocked by MicroPython script execution.
 * 
 * Runs:
 * - Peripherals service (for current sense measurements -> marching ants animation)
 * - TinyUSB task (to keep USB communication alive)
 * 
 * This is lighter weight than serviceAll() and doesn't run the full
 * priority-based scheduling - just the essentials to keep measurements
 * and visualization working during Python script execution.
 */
void jOSmanager::servicePython() {
    // Keep USB alive during MicroPython execution (mutex-guarded pump: the
    // Adafruit port also runs tud_task() from its USB IRQ under __usb_mutex,
    // so a raw tud_task() here could re-enter the stack)
    TinyUSB_Device_Task();
    
    // Run peripherals service for current sense measurements
    // This updates currentSenseState.filteredCurrent_mA which drives marching ants
    Peripherals::getInstance().service();
}

/**
 * @brief Force execution of a specific service by name
 * 
 * Allows selective service execution during critical operations where
 * we need just one service to run (e.g., ProbeButton during fast Python loops).
 * 
 * @param name Service name (case-sensitive, must match getName())
 * @return true if service was found and executed, false otherwise
 */
bool jOSmanager::forceServiceByName(const char* name) {
    if (name == nullptr) {
        return false;
    }
    
    for (uint8_t i = 0; i < serviceCount; i++) {
        if (!services[i].active) {
            continue;
        }
        
        Service* svc = services[i].service;
        if (svc == nullptr) {
            continue;
        }
        
        // Check if name matches
        if (strcmp(svc->getName(), name) == 0) {
            svc->service();
            return true;
        }
    }
    
    return false;  // Service not found
}

/**
 * @brief Force execution of a specific service by index
 * 
 * Faster than forceServiceByName if you already have the index.
 * Useful when you cache the index via getServiceIndex().
 * 
 * @param index Service index (0 to serviceCount-1)
 * @return true if index valid and service executed, false otherwise
 */
bool jOSmanager::forceServiceByIndex(uint8_t index) {
    if (index >= serviceCount) {
        return false;
    }
    
    if (!services[index].active) {
        return false;
    }
    
    Service* svc = services[index].service;
    if (svc == nullptr) {
        return false;
    }
    
    svc->service();
    return true;
}

/**
 * @brief Get service index by name for later use with forceServiceByIndex
 * 
 * Allows you to look up a service once, then use the faster index-based
 * call repeatedly (e.g., in a tight loop).
 * 
 * @param name Service name (case-sensitive)
 * @return Service index (0 to serviceCount-1), or -1 if not found
 */
int jOSmanager::getServiceIndex(const char* name) const {
    if (name == nullptr) {
        return -1;
    }
    
    for (uint8_t i = 0; i < serviceCount; i++) {
        if (!services[i].active) {
            continue;
        }
        
        Service* svc = services[i].service;
        if (svc == nullptr) {
            continue;
        }
        
        if (strcmp(svc->getName(), name) == 0) {
            return i;
        }
    }
    
    return -1;  // Not found
}

/**
 * @brief Sort services by priority using bubble sort
 * Simple algorithm since we have a small number of services
 */
void jOSmanager::sortServicesByPriority() {
    if (serviceCount <= 1) {
        return;
    }
    
    // Bubble sort by priority
    for (uint8_t i = 0; i < serviceCount - 1; i++) {
        for (uint8_t j = 0; j < serviceCount - i - 1; j++) {
            if (services[j].service == nullptr || services[j + 1].service == nullptr) {
                continue;
            }
            
            ServicePriority p1 = services[j].service->getPriority();
            ServicePriority p2 = services[j + 1].service->getPriority();
            
            // Lower enum value = higher priority (CRITICAL=0, LOW=3)
            if (static_cast<int>(p1) > static_cast<int>(p2)) {
                // Swap
                ServiceEntry temp = services[j];
                services[j] = services[j + 1];
                services[j + 1] = temp;
            }
        }
    }
}

/**
 * @brief Set execution divisors for priority-based scheduling
 * 
 * Controls how frequently services at each priority level run.
 * 
 * Example: setExecutionDivisors(1, 2, 10, 50) means:
 * - CRITICAL services run every loop
 * - HIGH services run every 2nd loop
 * - NORMAL services run every 10th loop  
 * - LOW services run every 50th loop
 * 
 * This allows fine-tuning of responsiveness vs. throughput tradeoffs.
 * 
 * @param critical How often to run CRITICAL services (default: 1 = every loop)
 * @param high How often to run HIGH services (default: 1 = every loop)
 * @param normal How often to run NORMAL services (default: 5 = every 5th loop)
 * @param low How often to run LOW services (default: 20 = every 20th loop)
 */
void jOSmanager::setExecutionDivisors(uint8_t critical, uint8_t high, 
                                       uint8_t normal, uint8_t low) {
    criticalDivisor = critical > 0 ? critical : 1;  // Ensure at least 1
    highDivisor = high > 0 ? high : 1;
    normalDivisor = normal > 0 ? normal : 1;
    lowDivisor = low > 0 ? low : 1;
}

// ============================================================================
// System Service Implementations
// ============================================================================

// TermSerialService - Terminal input handling
TermSerialService* TermSerialService::instance = nullptr;

TermSerialService::TermSerialService() : jerialInstance(nullptr) {}

void TermSerialService::setTermControl(JerialClass* jerial) {
    jerialInstance = jerial;
}

TermSerialService& TermSerialService::getInstance() {
    if (instance == nullptr) {
        instance = new TermSerialService();
    }
    return *instance;
}

/**
 * @brief Service method for terminal input
 * CRITICAL priority - user input must be instantly responsive
 * 
 * NOTE: Injected commands are now handled by InjectedCommandService (fast path).
 * This service only handles user-typed input via TermControl line buffering.
 */
ServiceStatus TermSerialService::service() {
    lastStatus = ServiceStatus::IDLE;
    
    if (jerialInstance == nullptr) {
        return lastStatus;
    }
    
    extern struct config jumperlessConfig;
    
    // Only service if line buffering is enabled for user input
    // Injected commands are handled separately by InjectedCommandService (fast path)
    if (jumperlessConfig.display.terminal_line_buffering != 1) {
        return lastStatus;
    }
    
    // CRITICAL: Don't consume Serial input when MicroPython owns stdin.
    // During REPL or script execution (e.g. time.sleep()), TermControl::service()
    // reads from Serial via stream->read(), stealing characters that should go to
    // MicroPython's sys.stdin. This causes select.poll()+read(1) loops to drop
    // characters (every-other-char pattern) because serviceCritical() calls us
    // every 50ms during mp_hal_delay_ms.
    if (isMicroPythonREPLActive()) {
        return lastStatus;
    }
    
    // // Service returns true when line is complete
    // if (jerialInstance->service()) {
    //     lastStatus = ServiceStatus::BUSY;
    // }
    
    return lastStatus;
}

// InjectedCommandService - Immediate command execution from injection buffer
InjectedCommandService* InjectedCommandService::instance = nullptr;

InjectedCommandService& InjectedCommandService::getInstance() {
    if (instance == nullptr) {
        instance = new InjectedCommandService();
    }
    return *instance;
}

/**
 * @brief Service method for injected command processing
 * CRITICAL priority - executes commands immediately to prevent buffer pile-up
 * 
 * This service uses a FAST PATH that directly reads from the injection buffer
 * without waiting for slow TermControl processing (which takes 800ms+).
 * 
 * Flow:
 * 1. AsyncPassthrough receives data on SerialPIO
 * 2. Tag parser injects command characters + newline into injection_buffer
 * 3. Sets hasInjectedCommand flag
 * 4. This service (runs every loop as CRITICAL) detects flag
 * 5. DIRECTLY extracts line from injection_buffer (bypasses TermControl)
 * 6. Executes via singleCharCommands.executeCommand()
 * 7. Continues processing if more commands are available
 * 
 * Performance: Commands execute in <1ms instead of 800ms+ via TermControl
 * Thread-safe: Uses atomic buffer position updates
 */
ServiceStatus InjectedCommandService::service() {
    // =========================================================================
    // DISABLED: Commands are now handled synchronously in main.cpp via CommandBuffer
    // 
    // The new architecture uses CommandBuffer for all UART-injected commands:
    // 1. AsyncPassthrough parses <j>/<p> tags and sets pending command in CommandBuffer
    // 2. Main loop checks CommandBuffer::hasPendingCommand() and processes synchronously
    // 3. No competing services, no race conditions, no async complexity
    // 
    // This service is kept for backwards compatibility but does nothing.
    // =========================================================================
    lastStatus = ServiceStatus::IDLE;
    
    // Clear legacy flags that may have been set
    Jerial.hasInjectedCommand = 0;
    
    return lastStatus;
    
    // LEGACY CODE BELOW - KEPT FOR REFERENCE
    #if 0
    // Fast check: is there a complete line in injection buffer?
    // This bypasses slow TermControl and reads directly from buffer
    if (!Jerial.hasInjectedCompleteLine()) {
        // No complete lines, clear flag and return
        Jerial.hasInjectedCommand = 0;
        return lastStatus;
    }
    #endif
}

// AsyncPassthroughService - USB CDC1 <-> UART0 bridging
AsyncPassthroughService* AsyncPassthroughService::instance = nullptr;

AsyncPassthroughService& AsyncPassthroughService::getInstance() {
    if (instance == nullptr) {
        instance = new AsyncPassthroughService();
    }
    return *instance;
}



/**
 * @brief Service method for AsyncPassthrough
 * CRITICAL priority - must run every loop to prevent data loss and maintain low latency
 * 
 * Bridges USB CDC1 (Serial1) <-> UART0 for async passthrough communication.
 * Handles USB->UART and UART->USB data transfer, line coding updates, and
 * command tag detection. Must run continuously to prevent buffer overflows
 * and ensure minimal latency.
 */
ServiceStatus AsyncPassthroughService::service() {
    lastStatus = ServiceStatus::IDLE;
    
    extern bool asyncPassthroughEnabled;
    extern bool async_begun;

    // Only run if async passthrough is enabled
    if (asyncPassthroughEnabled && async_begun) {
#if ASYNC_PASSTHROUGH_ENABLED == 1
        AsyncPassthrough::task();
        lastStatus = ServiceStatus::BUSY;
#endif
    }
    
    return lastStatus;
}

// TinyUSBService - USB communication
TinyUSBService* TinyUSBService::instance = nullptr;

TinyUSBService& TinyUSBService::getInstance() {
    if (instance == nullptr) {
        instance = new TinyUSBService();
    }
    return *instance;
}

/**
 * @brief Service method for TinyUSB task
 * CRITICAL priority - runs every pass and inside every modal loop. The pump is
 * TinyUSB_Device_Task(): the Adafruit port already runs tud_task() from its
 * USB soft-IRQ under __usb_mutex, so this is the mutex-guarded (try-enter)
 * thread-context entry - never a raw tud_task(). yield() would also flush
 * every CDC port; the CDC writers flush themselves, so the plain pump is enough here.
 */
ServiceStatus TinyUSBService::service() {
    lastStatus = ServiceStatus::IDLE;
    
#ifdef USE_TINYUSB
    TinyUSB_Device_Task();
    lastStatus = ServiceStatus::BUSY;
#endif
    
    return lastStatus;
}

// USBPeriodicService - USB mass storage housekeeping
USBPeriodicService* USBPeriodicService::instance = nullptr;

USBPeriodicService& USBPeriodicService::getInstance() {
    if (instance == nullptr) {
        instance = new USBPeriodicService();
    }
    return *instance;
}

/**
 * @brief Service method for USB periodic tasks
 * NORMAL priority - periodic USB maintenance
 * Only runs when MSC mode is enabled
 */
ServiceStatus USBPeriodicService::service() {
    lastStatus = ServiceStatus::IDLE;
    
    extern bool mscModeEnabled;
    
    if (mscModeEnabled) {
        usbPeriodic();
        lastStatus = ServiceStatus::BUSY;
    }
    
    return lastStatus;
}

// OLEDService - OLED display updates
OLEDService* OLEDService::instance = nullptr;

OLEDService& OLEDService::getInstance() {
    if (instance == nullptr) {
        instance = new OLEDService();
    }
    return *instance;
}

/**
 * @brief Service method for OLED periodic updates
 * LOW priority - display updates are not time-critical
 */
ServiceStatus OLEDService::service() {
    lastStatus = ServiceStatus::IDLE;

    if (oledDisplay != nullptr) {
        oledDisplay->oledPeriodic();
        lastStatus = ServiceStatus::BUSY;
    }
    
    // if (oledDisplay != nullptr) {
    //     //Serial.println("OLEDService::oledPeriodic");
    //     oledDisplay->oledPeriodic();
    //     lastStatus = ServiceStatus::BUSY;
    // } else {
    //     //Serial.println("OLEDService::oledPeriodic oledDisplay is nullptr");
    // }
    
    return lastStatus;
}

// OledGuiService - retained-screen rendering + live binding refresh
OledGuiService* OledGuiService::instance = nullptr;

OledGuiService& OledGuiService::getInstance() {
    if (instance == nullptr) {
        instance = new OledGuiService();
    }
    return *instance;
}

/**
 * @brief Service method for the retained OLED GUI
 * NORMAL priority - dirty-driven, internally rate-limited (~30 Hz). Does
 * nothing until a screen is activated, so it's free when unused.
 */
ServiceStatus OledGuiService::service() {
    lastStatus = ServiceStatus::IDLE;
    if (OledGui::getInstance().active() != nullptr) {
        OledGui::getInstance().tick();
        lastStatus = ServiceStatus::BUSY;
    }
    return lastStatus;
}

// LiveCrossbarService - Live crossbar terminal display
LiveCrossbarService* LiveCrossbarService::instance = nullptr;

// Access probeActive to use faster refresh during probe mode
extern volatile int probeActive;

LiveCrossbarService& LiveCrossbarService::getInstance() {
    if (instance == nullptr) {
        instance = new LiveCrossbarService();
    }
    return *instance;
}

/**
 * @brief Check if colors are assigned for all active nets
 * Returns true if all nets with paths have a termColor assigned
 */
bool LiveCrossbarService::colorsReady() const {
   return true;
    // Check if the last net with paths has a color assigned
    // This is a quick heuristic - if the last one has color, earlier ones should too
    for (int i = MAX_NETS - 1; i >= 0; i--) {
        if (globalState.connections.nets[i].number > 0) {
            // Found an active net - check if it has a color
            // termColor of 0 typically means unassigned (white/default)
            // We consider color "ready" if termColor > 0
            if (globalState.connections.nets[i].termColor == 0 || globalState.connections.nets[i].termColor == 255) {
                return false;  // No color assigned yet
            }
            return true;  // Color is assigned
        }
    }
    return true;  // No active nets, colors are "ready"
}

/**
 * @brief Service method for live crossbar display updates
 * LOW priority - display updates are not time-critical (except in probe mode)
 * Only updates when enabled and colors are ready
 * Uses faster refresh rate (100ms) during probe mode for responsive feedback
 * After changes stop, does one extra update to catch late color assignments, then stops
 */
ServiceStatus LiveCrossbarService::service() {
    lastStatus = ServiceStatus::IDLE;
    
    // Skip if not enabled
    if (!liveCrossbarEnabled) {
        return lastStatus;
    }
    
    unsigned long now = millis();
    // Use faster refresh during probe mode for responsive updates
    unsigned long refreshInterval = probeActive ? LiveCrossbarService::PROBE_REFRESH_INTERVAL_MS 
                                                : LiveCrossbarService::REFRESH_INTERVAL_MS;
    bool timeForRefresh = (now - lastUpdateTime >= refreshInterval);
    
    // Determine if we should update:
    // 1. If there's a pending change request
    // 2. If time for refresh AND we haven't done our extra update yet
    bool shouldUpdate = updatePending || (timeForRefresh && extraUpdateNeeded);
    
    if (shouldUpdate) {
        // Check if colors are ready before updating
        if (colorsReady()) {
            updateLiveCrossbarDisplay();
            lastUpdateTime = now;
            
            if (updatePending) {
                // New change came in - reset extra update flag
                extraUpdateNeeded = true;
                updatePending = false;
            } else {
                // This was the extra update (no pending change)
                extraUpdateNeeded = false;
            }
            lastStatus = ServiceStatus::BUSY;
        }
        // If colors not ready, we'll try again on next service call
    }
    
    return lastStatus;
}

// ============================================================================
// CONTEXT MANAGER IMPLEMENTATION
// ============================================================================

#include "externVars.h"  // For fs_mutex, core_sync_acquire/release
#include "FileParsing.h"  // For closeAllFiles()

// Static instance pointer
ContextManager* ContextManager::instance = nullptr;

// Global reference for convenient access
ContextManager& contextManager = ContextManager::getInstance();

/**
 * @brief Get human-readable name for context type
 */
const char* getContextTypeName(ContextType type) {
    switch (type) {
        case ContextType::NONE:           return "NONE";
        case ContextType::MAIN_MENU:      return "MAIN_MENU";
        case ContextType::FILE_MANAGER:   return "FILE_MANAGER";
        case ContextType::EKILO_EDITOR:   return "EKILO_EDITOR";
        case ContextType::PYTHON_REPL:    return "PYTHON_REPL";
        case ContextType::HELP_DOCS:      return "HELP_DOCS";
        case ContextType::DEBUG_MENU:     return "DEBUG_MENU";
        case ContextType::PROBING:        return "PROBING";
        case ContextType::CLICKWHEEL_MENU: return "CLICKWHEEL_MENU";
        case ContextType::APP_GENERIC:    return "APP_GENERIC";
        default:                          return "UNKNOWN";
    }
}

/**
 * @brief Private constructor - initializes all state
 */
ContextManager::ContextManager()
    : stackTop(-1)
    , fileCount(0)
    , transferDataLen(0)
{
    // Initialize arrays
    for (int i = 0; i < MAX_STACK_DEPTH; i++) {
        stack[i] = ContextEntry();
    }
    for (int i = 0; i < MAX_TRACKED_FILES; i++) {
        openFiles[i] = nullptr;
    }
    transferPath[0] = '\0';
}

/**
 * @brief Get the singleton instance
 */
ContextManager& ContextManager::getInstance() {
    if (instance == nullptr) {
        instance = new ContextManager();
    }
    return *instance;
}

/**
 * @brief Push a new context onto the stack
 * 
 * If the same context type already exists on the stack, this will:
 * - Reject the push and return false (no stack modification)
 * - Caller should use popContext() explicitly to navigate back
 * 
 * This ensures stack order is always preserved and prevents unexpected navigation.
 */
bool ContextManager::pushContext(const ContextEntry& ctx, bool /*unused*/) {
    // Check if this context type already exists on the stack
    // If so, reject the push - caller should use popContext() to navigate back
    for (int i = 0; i <= stackTop; i++) {
        if (stack[i].type == ctx.type) {
            Serial.print("WARNING: Context type ");
            Serial.print(getContextTypeName(ctx.type));
            Serial.println(" already on stack - push rejected. Use popContext() to navigate back.");
            printStack();
            return false;  // Reject duplicate - don't modify stack
        }
    }
    
    // Thread safety: acquire mutex during stack modification
    core_sync_acquire();
    
    // Check if stack is full
    if (stackTop >= MAX_STACK_DEPTH - 1) {
        core_sync_release();
        Serial.println("ERROR: Context stack full!");
        printStack();
        return false;
    }
    
    // If there's a current context, call its onSuspend callback
    if (stackTop >= 0 && stack[stackTop].onSuspend != nullptr) {
        stack[stackTop].onSuspend(stack[stackTop].userData);
    }
    
    // Push the new context
    stackTop++;
    stack[stackTop] = ctx;
    
    core_sync_release();
    
    // Call onEnter callback (outside mutex to allow nested operations)
    if (ctx.onEnter != nullptr) {
        ctx.onEnter(ctx.userData);
    }
    
    return true;
}

/**
 * @brief Pop the current context and return to parent
 */
bool ContextManager::popContext() {
    // Thread safety

    // Serial.println("ContextManager::popContext called");
    // Serial.flush();

    core_sync_acquire();
    
    // Check if stack is empty
    if (stackTop < 0) {
        core_sync_release();
        Serial.println("WARNING: Attempted to pop empty context stack");
        return false;
    }
    
    // Get current context info before modifying stack
    ContextEntry current = stack[stackTop];
    
    // Close all tracked files for this context BEFORE calling exit callback
    // This ensures files are closed even if exit callback doesn't do it
    closeAllTrackedFiles();
    
    // Pop the context
    stack[stackTop] = ContextEntry();  // Clear entry
    stackTop--;
    
    core_sync_release();
    
    // Call onExit callback for the popped context (outside mutex)
    if (current.onExit != nullptr) {
        current.onExit(current.userData);
    }
    
    // If there's a parent context, call its onResume callback
    if (stackTop >= 0 && stack[stackTop].onResume != nullptr) {
        stack[stackTop].onResume(stack[stackTop].userData);
    }

    // Discard any probe-button press latched while we were inside an
    // interactive child context. Those loops (menus, the MicroPython REPL,
    // etc.) don't consume probe presses, so a press registered in there
    // would otherwise be picked up by the idle loop on return and drop the
    // user straight into probe mode. getButtonPress() also rejects stale
    // latches by age; this clears even a freshly-latched one on the exit
    // edge as a belt-and-suspenders guard.
    switch (current.type) {
        case ContextType::PYTHON_REPL:
        case ContextType::MAIN_MENU:
        case ContextType::CLICKWHEEL_MENU:
        case ContextType::DEBUG_MENU:
            ProbeButton::getInstance().clearButtonState();
            break;
        default:
            break;
    }
    
    // NOTE: Transfer path is NOT cleared here - it's meant to be read by the
    // parent context after the child exits. The parent should clear it after reading.
    // This allows zero-copy file path passing between contexts.
    // Only clear transfer DATA (not path) as it's typically consumed immediately.
    clearTransferData();
    
    return true;
}

/**
 * @brief Get the current context type
 */
ContextType ContextManager::currentContext() const {
    if (stackTop < 0) {
        return ContextType::NONE;
    }
    return stack[stackTop].type;
}

/**
 * @brief Get the current context entry
 */
const ContextEntry* ContextManager::currentContextEntry() const {
    if (stackTop < 0) {
        return nullptr;
    }
    return &stack[stackTop];
}

/**
 * @brief Check if a context type is anywhere in the stack
 */
bool ContextManager::isContextActive(ContextType type) const {
    for (int i = 0; i <= stackTop; i++) {
        if (stack[i].type == type) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// File Handle Tracking
// ============================================================================

/**
 * @brief Register an open file for tracking
 */
bool ContextManager::registerOpenFile(void* file) {
    if (file == nullptr) {
        return false;
    }
    
    // Check if already registered
    for (int i = 0; i < fileCount; i++) {
        if (openFiles[i] == file) {
            return true;  // Already tracked
        }
    }
    
    // Check if tracking array is full
    if (fileCount >= MAX_TRACKED_FILES) {
        Serial.println("WARNING: File tracking array full");
        return false;
    }
    
    // Register the file
    openFiles[fileCount++] = file;
    return true;
}

/**
 * @brief Unregister a file handle
 */
void ContextManager::unregisterFile(void* file) {
    if (file == nullptr) {
        return;
    }
    
    // Find and remove
    for (int i = 0; i < fileCount; i++) {
        if (openFiles[i] == file) {
            // Shift remaining entries down
            for (int j = i; j < fileCount - 1; j++) {
                openFiles[j] = openFiles[j + 1];
            }
            openFiles[fileCount - 1] = nullptr;
            fileCount--;
            return;
        }
    }
}

/**
 * @brief Close all tracked files
 * 
 * This is called automatically by popContext() to ensure files are closed
 * even if the context's exit callback forgets to do so.
 */
void ContextManager::closeAllTrackedFiles() {
    // Use the existing closeAllFiles() function from FilesystemStuff
    // which properly handles FatFS file closure
    extern void closeAllFiles();
    closeAllFiles();
    
    // Clear our tracking array
    for (int i = 0; i < MAX_TRACKED_FILES; i++) {
        openFiles[i] = nullptr;
    }
    fileCount = 0;
}

// ============================================================================
// Zero-Copy Data Transfer
// ============================================================================

/**
 * @brief Set a file path for transfer between contexts
 */
bool ContextManager::setTransferPath(const char* path) {
    if (path == nullptr) {
        transferPath[0] = '\0';
        return true;
    }
    
    size_t len = strlen(path);
    if (len >= sizeof(transferPath)) {
        Serial.println("WARNING: Transfer path too long, truncating");
        len = sizeof(transferPath) - 1;
    }
    
    memcpy(transferPath, path, len);
    transferPath[len] = '\0';
    return true;
}

/**
 * @brief Set small data for transfer
 */
bool ContextManager::setTransferData(const void* data, size_t len) {
    if (data == nullptr || len == 0) {
        transferDataLen = 0;
        return true;
    }
    
    if (len > sizeof(transferBuffer)) {
        Serial.println("WARNING: Transfer data too large");
        return false;
    }
    
    memcpy(transferBuffer, data, len);
    transferDataLen = len;
    return true;
}

/**
 * @brief Get transfer data
 */
const void* ContextManager::getTransferData(size_t* outLen) const {
    if (outLen != nullptr) {
        *outLen = transferDataLen;
    }
    
    if (transferDataLen == 0) {
        return nullptr;
    }
    
    return transferBuffer;
}

// ============================================================================
// Debugging
// ============================================================================

/**
 * @brief Print the current context stack
 */
void ContextManager::printStack() const {
    Serial.println("\n=== Context Stack ===");
    if (stackTop < 0) {
        Serial.println("  (empty)");
    } else {
        for (int i = stackTop; i >= 0; i--) {
            Serial.print("  [");
            Serial.print(i);
            Serial.print("] ");
            Serial.print(getContextTypeName(stack[i].type));
            if (i == stackTop) {
                Serial.print(" <- current");
            }
            if (stack[i].isBackground) {
                Serial.print(" (background)");
            }
            Serial.println();
        }
    }
    
    Serial.print("Tracked files: ");
    Serial.println(fileCount);
    
    if (hasTransferPath()) {
        Serial.print("Transfer path: ");
        Serial.println(transferPath);
    }
    if (transferDataLen > 0) {
        Serial.print("Transfer data: ");
        Serial.print(transferDataLen);
        Serial.println(" bytes");
    }
    Serial.println("=====================\n");
}

