// SPDX-License-Identifier: MIT
#ifndef JUMPERLOS_H
#define JUMPERLOS_H

#include <Arduino.h>
#include "hardware/sync.h" // __dmb() for Service::requestRun()
#include "Jerial.h"

// Forward declarations
class Service;
class jOSmanager;
class ContextManager;
class Probing;
class Highlighting;
class Menus;
class Peripherals;
class SlotManager;

// =============================================================================
// CONTEXT MANAGEMENT SYSTEM
// =============================================================================
// Stack-based context navigation for UI states (Menu -> FileManager -> Ekilo -> Python)
// Provides:
// - Proper cleanup when exiting contexts
// - Zero-copy data passing between contexts via file paths OR SharedBuffer
// - Pre-allocated 24KB SharedBuffer for fast transfers without flash I/O
// - Centralized file handle tracking to prevent leaks
// - Thread-safe operations using existing mutex infrastructure
//
// Data Transfer Priority:
// 1. SharedBuffer (fastest - already in RAM, no file I/O)
// 2. Transfer path (file path - consumer loads from file)
// 3. Transfer data (small inline data buffer, max 256 bytes)

/**
 * @brief Context types - each represents a distinct UI/execution mode
 */
enum class ContextType : uint8_t {
    NONE = 0,           // No context (stack empty or error state)
    MAIN_MENU,          // Main serial menu (SingleCharCommands)
    FILE_MANAGER,       // File browser UI
    EKILO_EDITOR,       // Text editor
    PYTHON_REPL,        // MicroPython REPL
    HELP_DOCS,          // Help documentation viewer
    DEBUG_MENU,         // Debug flags menu
    PROBING,            // Probe mode
    CLICKWHEEL_MENU,    // Rotary encoder menu
    APP_GENERIC,        // Generic app context
    CONTEXT_TYPE_COUNT  // Must be last - used for array sizing
};

/**
 * @brief Get a human-readable name for a context type
 */
const char* getContextTypeName(ContextType type);

/**
 * @brief Callback function type for context lifecycle events
 * @param userData Context-specific data passed during push
 */
typedef void (*ContextCallback)(void* userData);

/**
 * @brief Context entry in the navigation stack
 * 
 * Each entry represents a UI/execution state with lifecycle callbacks.
 * Callbacks are optional (can be nullptr).
 */
struct ContextEntry {
    ContextType type;                   // What kind of context this is
    ContextCallback onEnter;            // Called when context becomes active (pushed)
    ContextCallback onExit;             // Called when context is exited (popped) - cleanup here
    ContextCallback onSuspend;          // Called when a child context is pushed on top
    ContextCallback onResume;           // Called when returning from a child context
    void* userData;                     // Context-specific data (e.g., filename pointer)
    bool isBackground;                  // For future: concurrent background execution
    
    // Default constructor - all null/zero
    ContextEntry() : type(ContextType::NONE), onEnter(nullptr), onExit(nullptr),
                     onSuspend(nullptr), onResume(nullptr), userData(nullptr), 
                     isBackground(false) {}
    
    // Convenience constructor for simple contexts
    ContextEntry(ContextType t, ContextCallback exitCb = nullptr, void* data = nullptr)
        : type(t), onEnter(nullptr), onExit(exitCb), onSuspend(nullptr), 
          onResume(nullptr), userData(data), isBackground(false) {}
};

/**
 * @brief Status of a service after its service() method executes
 */
enum class ServiceStatus {
    IDLE,      // Service has nothing to do this cycle
    BUSY,      // Service is actively working but non-blocking
    BLOCKING,  // Service needs exclusive control (blocks lower priority services)
    ERROR      // Service encountered an error
};

/**
 * @brief Priority level for service execution ordering
 * Higher priority services run first in each cycle
 */
enum class ServicePriority {
    CRITICAL = 0,  // Highest priority - always runs (e.g., menus, user input)
    HIGH = 1,      // High priority - time-sensitive operations (e.g., probing, highlighting)
    NORMAL = 2,    // Normal priority - periodic tasks (e.g., measurements)
    LOW = 3        // Lowest priority - background tasks
};

/**
 * @brief Base interface for all services in JumperlOS
 * 
 * Services are modular subsystems that can be registered with a ServiceManager
 * and executed in priority order. They report their status after each execution.
 * 
 * Design is core-agnostic - services can run on Core 0 or Core 1
 */
class Service {
public:
    virtual ~Service() {}
    
    /**
     * @brief Main service execution method - called each loop iteration
     * @return ServiceStatus indicating current state
     */
    virtual ServiceStatus service() = 0;
    
    /**
     * @brief Get the service name for debugging/logging
     */
    virtual const char* getName() const = 0;
    
    /**
     * @brief Get the service priority
     */
    virtual ServicePriority getPriority() const = 0;
    
    /**
     * @brief Check if this service is currently active/busy
     */
    virtual bool isActive() const {
        return lastStatus == ServiceStatus::BUSY ||
               lastStatus == ServiceStatus::BLOCKING;
    }

    /**
     * @brief Get the last status returned by service()
     */
    ServiceStatus getLastStatus() const { return lastStatus; }

    // ---- scheduling (jOSmanager reads these; see JumperlOS.cpp serviceAll) ----

    /**
     * @brief How often this service wants to run, in microseconds.
     * 0 (the default) = every scheduler pass, exactly as before periods
     * existed. A non-zero period only ever ENCODES a cadence the service
     * already applies internally (its own millis() gate stays in place), so
     * the walk skips it cheaply between runs; it is not a new rate limit.
     * requestRun() overrides the period for one pass.
     */
    virtual uint32_t periodUs() const { return 0; }

    /**
     * @brief Does this service keep running while a modal loop owns core 0?
     * The "inner set" is what jOS.serviceInner() runs: what probeMode(), the
     * click menus, the pad menus, the apps' input loops and MicroPython
     * delays keep alive, and what serviceAll() still runs while a BLOCKING
     * service holds the loop. Default: CRITICAL priority. AsyncPassthrough
     * (HIGH) opts in - the Arduino UART bridge must not stop while the probe
     * or a menu is in use (B4, T1.5).
     */
    virtual bool inInnerSet() const { return getPriority() == ServicePriority::CRITICAL; }

    /**
     * @brief Ask the scheduler to run this service on its very next pass,
     * whatever its period. Safe from an IRQ or the other core: one aligned
     * byte store, and the scheduler only clears the flag when it has read it
     * set - a request landing between its read and the run is covered by
     * that run, one landing during the run re-pends for the next pass.
     */
    void requestRun() { __dmb(); pending = true; }

    // Managed by jOSmanager - read-only for everyone else (the X table).
    // nextDueUs is 64-bit (time_us_64()) on purpose: a service parked behind
    // a modal loop or a BLOCKING menu for hours is then simply "overdue" when
    // it comes back. A 32-bit deadline compare goes "not due" again 35.8 min
    // after it was last stamped and the service stays dead for another ~35 min.
    uint64_t nextDueUs = 0;          // time_us_64() at which the period next elapses
    volatile bool pending = false;   // requestRun() latch
    uint32_t runs = 0;               // service() calls (scheduler + modal set + force*)
    uint32_t lastUs = 0;             // duration of the last call
    uint32_t maxUs = 0;              // longest call since boot
    uint32_t overruns = 0;           // calls that took longer than periodUs() (period > 0 only)
    uint64_t totalUs = 0;            // sum of all call durations (avg = totalUs / runs)

protected:
    ServiceStatus lastStatus = ServiceStatus::IDLE;
};

/**
 * @brief Manages and coordinates execution of all registered services
 * 
 * Instance-based design allows multiple jOSmanagers (e.g., Core 1 and Core 2)
 * Executes services in priority order, handling blocking scenarios
 */
class jOSmanager {
public:
    jOSmanager(uint8_t coreId = 0);
    ~jOSmanager();
    
    /**
     * @brief Get the Core 1 (main) jOSmanager instance
     * This is a convenience singleton for the primary core
     */
    static jOSmanager& getInstance();
    
    /**
     * @brief Register a service with this manager
     * @param service Pointer to service (must remain valid)
     * @param priority Priority level for execution ordering
     * @return true if registered successfully
     */
    bool registerService(Service* service);
    
    /**
     * @brief Unregister a service
     */
    bool unregisterService(Service* service);
    
    /**
     * @brief Execute all registered services in priority order
     *
     * A service runs on a pass when it is due (its periodUs() has elapsed
     * since its last run start, or its period is 0), or when someone called
     * requestRun() on it since its last run. If a service returns BLOCKING,
     * only CRITICAL priority services (and the blocking one) run until it
     * returns to non-blocking state; a requestRun() on a skipped service
     * survives the skip.
     */
    void serviceAll();

    /**
     * @brief One pass over the inner set (Service::inInnerSet()) - the
     * services a modal loop keeps alive: ProbeButton, MpRemote, Peripherals,
     * TinyUSB (mutex-guarded pump) and AsyncPassthrough. probeMode(), the
     * click/pad menus, the apps' input loops and MicroPython delays call this
     * instead of the scheduler; serviceAll() runs the same set while a
     * BLOCKING service holds the loop. Same due-or-pending gate and stats as
     * serviceAll() (the X table counts these calls). Replaces the old
     * serviceCritical() (= "the CRITICAL priority services"), which named the
     * set by accident of priority; the set is explicit now.
     */
    void serviceInner();

    /**
     * @brief Number of registered services (for the X table)
     */
    uint8_t getServiceCount() const { return serviceCount; }

    /**
     * @brief Registered service by index (nullptr if out of range) - for the
     * X table; the array is priority-sorted, registration order within a
     * priority.
     */
    Service* getServiceAt(uint8_t index) const {
        return (index < serviceCount) ? services[index].service : nullptr;
    }

    /**
     * @brief Passes serviceAll() has made since boot (the X table).
     */
    unsigned long getLoopCount() const { return loopCounter; }
    
    /**
     * @brief Execute services needed during MicroPython REPL execution
     *
     * = serviceInner() (it used to be a hand-rolled subset: the USB pump +
     * Peripherals; the inner set is that plus ProbeButton, MpRemote (its own
     * reentrancy guard defers USBSer2 while a script runs) and
     * AsyncPassthrough). No caller in src/ today - mp_hal_delay_ms in
     * Python_Proper.cpp calls serviceInner() directly; kept as a named entry
     * point for the MicroPython side.
     */
    void servicePython();
    
    /**
     * @brief Force execution of a specific service by name
     * 
     * This allows selective service execution during critical operations
     * (e.g., running ProbeButton during fast Python loops).
     * 
     * @param name Service name (case-sensitive, must match getName())
     * @return true if service was found and executed, false otherwise
     */
    bool forceServiceByName(const char* name);
    
    /**
     * @brief Force execution of a specific service by index
     * 
     * Faster than forceServiceByName if you already have the index.
     * 
     * @param index Service index (0 to serviceCount-1)
     * @return true if index valid and service executed, false otherwise
     */
    bool forceServiceByIndex(uint8_t index);
    
    /**
     * @brief Get service index by name for later use with forceServiceByIndex
     * 
     * @param name Service name (case-sensitive)
     * @return Service index, or -1 if not found
     */
    int getServiceIndex(const char* name) const;
    
    /**
     * @brief Get the currently blocking service (if any)
     * @return Pointer to blocking service, or nullptr if none
     */
    Service* getBlockingService() const { return blockingService; }
    
    /**
     * @brief Check if any service is currently blocking
     */
    bool isBlocked() const { return blockingService != nullptr; }
    
    /**
     * @brief Get core ID this manager is running on
     */
    uint8_t getCoreId() const { return coreId; }
    
private:
    static const uint8_t MAX_SERVICES = 24;  // Increased from 16 to accommodate ConfigSaveService and future services
    
    struct ServiceEntry {
        Service* service;
        bool active;
    };
    
    ServiceEntry services[MAX_SERVICES];
    uint8_t serviceCount;
    uint8_t coreId;
    Service* blockingService;

    // serviceAll() passes since boot (diagnostics; the old per-band loop
    // divisors that hung off it are gone - periods replaced them).
    unsigned long loopCounter;

    // Core 1 singleton instance
    static jOSmanager* core1Instance;

    // Sort services by priority (simple bubble sort - small array)
    void sortServicesByPriority();

    /**
     * @brief The due-or-pending gate. Captures (and, only if set, clears)
     * the service's requestRun() latch, then answers "run it this pass?".
     * @param now time_us_64() taken by the caller
     */
    static bool isDue(Service* svc, uint64_t now);

    /**
     * @brief Run one service and account for it: nextDueUs (stamped from the
     * run START, so a period that equals a service's own millis() gate never
     * aliases against it), runs / lastUs / maxUs / totalUs / overruns.
     * Shared by serviceAll(), serviceInner() and the force* paths so the
     * X table sees every call, including the modal loops'.
     * @return the status service() returned
     */
    ServiceStatus runService(Service* svc, uint64_t now);
};

/**
 * @brief System service wrappers for integrating existing subsystems into jOSmanager
 * 
 * These lightweight wrappers allow existing service routines (termSerial, tud_task, etc.)
 * to be scheduled via jOSmanager with appropriate priorities.
 */

// Forward declarations for external dependencies
class JerialClass;
class oled;

/**
 * @brief Terminal input service - handles line buffering and input processing
 * CRITICAL priority. NOT REGISTERED any more: its body is commented out
 * (Jerial.service() is called from loop() directly). Class kept for a later
 * cleanup pass.
 */
class TermSerialService : public Service {
public:
    static TermSerialService& getInstance();
    TermSerialService(const TermSerialService&) = delete;
    TermSerialService& operator=(const TermSerialService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "TermSerial"; }
    ServicePriority getPriority() const override { return ServicePriority::CRITICAL; }
    
    void setTermControl(JerialClass* jerial);
    
private:
    TermSerialService();
    ~TermSerialService() = default;
    static TermSerialService* instance;
    JerialClass* jerialInstance;
};

/**
 * @brief Relayed command processor - handles commands from AsyncPassthrough immediately
 * CRITICAL priority. NOT REGISTERED any more: disabled in favour of CommandBuffer
 * processed synchronously in loop(). Class kept for a later cleanup pass.
 * 
 * This service checks for completed relayed commands (from Arduino via <j> tags)
 * and executes them immediately, preventing the buffer from filling up when the
 * main loop is busy. Commands are executed synchronously via singleCharCommands.
 */
class RelayedCommandService : public Service {
public:
    static RelayedCommandService& getInstance();
    RelayedCommandService(const RelayedCommandService&) = delete;
    RelayedCommandService& operator=(const RelayedCommandService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "RelayedCmd"; }
    ServicePriority getPriority() const override { return ServicePriority::CRITICAL; }
    
private:
    RelayedCommandService() = default;
    ~RelayedCommandService() = default;
    static RelayedCommandService* instance;
};

/**
 * @brief AsyncPassthrough service - handles USB CDC1 <-> UART0 bridging
 * HIGH priority - runs every loop pass to prevent data loss and maintain low
 * latency, AND in the inner set: the Arduino UART bridge used to stop the
 * moment the probe or a click menu owned the loop (those ran the CRITICAL
 * set only) - B4 puts it in the modal set explicitly.
 */
class AsyncPassthroughService : public Service {
public:
    static AsyncPassthroughService& getInstance();
    AsyncPassthroughService(const AsyncPassthroughService&) = delete;
    AsyncPassthroughService& operator=(const AsyncPassthroughService&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "AsyncPassthrough"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    bool inInnerSet() const override { return true; }
    
private:
    AsyncPassthroughService() = default;
    ~AsyncPassthroughService() = default;
    static AsyncPassthroughService* instance;
};

/**
 * @brief TinyUSB task service - pumps the USB device stack from the loop
 * CRITICAL priority - every pass, and inside the modal loops' inner set (it
 * used to be NORMAL = every 3rd pass, which is why every hot wait in the tree
 * grew its own raw tud_task()). Mutex-guarded via TinyUSB_Device_Task().
 */
class TinyUSBService : public Service {
public:
    static TinyUSBService& getInstance();
    TinyUSBService(const TinyUSBService&) = delete;
    TinyUSBService& operator=(const TinyUSBService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "TinyUSB"; }
    ServicePriority getPriority() const override { return ServicePriority::CRITICAL; }
    
private:
    TinyUSBService() = default;
    ~TinyUSBService() = default;
    static TinyUSBService* instance;
};

/**
 * @brief Port housekeeping - the 10 ms block that used to sit in loop()
 * after serviceAll() (B6, T1.7): secondSerialHandler() (Arduino DTR-pulse
 * flash detection + UART auto-connect and its active servicing),
 * replyWithSerialInfo() (the ENQ 0x05 port-info reply - USB-CDC I/O, so it
 * lives on core 0) and serviceNetVoltageScanDebug() (1 Hz debug report,
 * self-throttled). NORMAL priority, 10 ms period, not in the inner set (it
 * never ran inside the modal loops before either).
 */
class PortHousekeepingService : public Service {
public:
    static PortHousekeepingService& getInstance();
    PortHousekeepingService(const PortHousekeepingService&) = delete;
    PortHousekeepingService& operator=(const PortHousekeepingService&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "PortHousekeeping"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    uint32_t periodUs() const override { return 10000; }

private:
    PortHousekeepingService() = default;
    ~PortHousekeepingService() = default;
    static PortHousekeepingService* instance;
};

/**
 * @brief LED-dump mode (R!, or serial_1/2.function 5/6): the terminal picture
 * of the LEDs. It used to be drawn from loop1() on core 1 - a USB CDC writer
 * on the non-USB core, the documented wedge family (T1.10). Core 1 now only
 * raises ledDumpFrameReady after a frame is shown; this service on core 0
 * does the dump: at most every dumpLEDrate ms when a fresh frame is there,
 * and at least every 1 s so a static picture still repaints. In the inner set
 * on purpose - the picture used to keep updating through probe mode and the
 * menus (core 1 did not care what core 0 was doing) and still does; it costs
 * nothing while the mode is off (one compare per pass).
 */
class LedDumpService : public Service {
public:
    static LedDumpService& getInstance();
    LedDumpService(const LedDumpService&) = delete;
    LedDumpService& operator=(const LedDumpService&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "LedDump"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    bool inInnerSet() const override { return true; }
    uint32_t periodUs() const override { return 10000; }

private:
    LedDumpService() = default;
    ~LedDumpService() = default;
    static LedDumpService* instance;
};

/**
 * @brief USB periodic service - handles USB mass storage housekeeping
 * NORMAL priority. NOT REGISTERED any more: usbPeriodic() is a debug print.
 * Class kept for a later cleanup pass.
 */
class USBPeriodicService : public Service {
public:
    static USBPeriodicService& getInstance();
    USBPeriodicService(const USBPeriodicService&) = delete;
    USBPeriodicService& operator=(const USBPeriodicService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "USBPeriodic"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    
private:
    USBPeriodicService() = default;
    ~USBPeriodicService() = default;
    static USBPeriodicService* instance;
};

/**
 * @brief OLED display periodic service - handles display updates
 * LOW priority - display updates are not time-critical
 */
class OLEDService : public Service {
public:
    static OLEDService& getInstance();
    OLEDService(const OLEDService&) = delete;
    OLEDService& operator=(const OLEDService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "OLED"; }
    ServicePriority getPriority() const override { return ServicePriority::LOW; }
    // oledPeriodic(): connection pings are self-gated >= 750 ms, but the
    // post-hold and post-wavegen flushes have no gate of their own and would
    // wait a whole period - so 20 ms, not the ping interval.
    uint32_t periodUs() const override { return 20000; }

    void setOledDisplay(class oled* display) { oledDisplay = display; }
    
private:
    OLEDService() : oledDisplay(nullptr) {}
    ~OLEDService() = default;
    static OLEDService* instance;
    class oled* oledDisplay;
};

/**
 * @brief OLED GUI render service - drives retained-screen rendering + live bindings
 * NORMAL priority - re-renders the active OledScreen and re-resolves bound
 * {token} values, dirty-driven with an internal ~30 Hz cap. Completely inert
 * (no work, no display access) until a screen is activated via
 * OledGui::activate(), so it never perturbs existing display behavior on its
 * own. Connection maintenance stays in the LOW-priority OLEDService.
 */
class OledGuiService : public Service {
public:
    static OledGuiService& getInstance();
    OledGuiService(const OledGuiService&) = delete;
    OledGuiService& operator=(const OledGuiService&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "OledGui"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    // OledGui::renderNow()'s own minInterval for a foreground screen is 15 ms
    // (160 ms for the idle stats page); this encodes the faster one.
    uint32_t periodUs() const override { return 15000; }

private:
    OledGuiService() = default;
    ~OledGuiService() = default;
    static OledGuiService* instance;
};

/**
 * @brief Live Crossbar Display service - updates terminal display when enabled
 * LOW priority - display updates are not time-critical
 * Waits for colors to be assigned before updating to avoid rendering without colors
 */
class LiveCrossbarService : public Service {
public:
    static LiveCrossbarService& getInstance();
    LiveCrossbarService(const LiveCrossbarService&) = delete;
    LiveCrossbarService& operator=(const LiveCrossbarService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "LiveXbar"; }
    ServicePriority getPriority() const override { return ServicePriority::LOW; }
    // The periodic refresh is 60 s / 400 ms (probe); 100 ms bounds how late
    // that fires. A crossbar change does not wait for it: requestUpdate()
    // also requestRun()s, so it is drawn on the next pass.
    uint32_t periodUs() const override { return 100000; }

    // Request an update (called from sendAllPaths - on core 1 - etc.)
    void requestUpdate() { updatePending = true; extraUpdateNeeded = true; requestRun(); }
    
    // Check if colors are assigned for all active nets
    bool colorsReady() const;
    
private:
    LiveCrossbarService() : updatePending(false), extraUpdateNeeded(false), lastUpdateTime(0) {}
    ~LiveCrossbarService() = default;
    static LiveCrossbarService* instance;
    
    bool updatePending;
    bool extraUpdateNeeded;  // Allow one extra update after changes stop (to catch late colors)
    unsigned long lastUpdateTime;
    static const unsigned long REFRESH_INTERVAL_MS = 60000;        // Normal refresh rate
    static const unsigned long PROBE_REFRESH_INTERVAL_MS = 400;  // Faster refresh during probe mode
};

// =============================================================================
// CONTEXT MANAGER
// =============================================================================

/**
 * @brief Manages the navigation stack and resource tracking for UI contexts
 * 
 * This singleton provides:
 * - Stack-based navigation (push to enter context, pop to exit)
 * - Lifecycle callbacks for proper cleanup
 * - Centralized file handle tracking to prevent leaks
 * - Zero-copy data passing between contexts via file paths
 * - Thread-safe operations using existing mutex infrastructure
 * 
 * Usage:
 *   ContextManager& ctx = ContextManager::getInstance();
 *   ctx.pushContext(ContextEntry(ContextType::FILE_MANAGER, cleanupFunc));
 *   // ... run file manager ...
 *   ctx.popContext();  // Automatically calls cleanupFunc
 */
class ContextManager {
public:
    static ContextManager& getInstance();
    
    // Delete copy/move operations (singleton)
    ContextManager(const ContextManager&) = delete;
    ContextManager& operator=(const ContextManager&) = delete;
    
    // =========================================================================
    // Stack Operations
    // =========================================================================
    
    /**
     * @brief Push a new context onto the stack
     * 
     * Calls onSuspend on current context (if any), then onEnter on new context.
     * 
     * @param ctx Context entry to push
     * @param allowDuplicate If true, allows pushing same context type again (for nested REPL etc)
     * @return true if pushed successfully, false if stack full or duplicate (when not allowed)
     */
    bool pushContext(const ContextEntry& ctx, bool allowDuplicate = false);
    
    /**
     * @brief Pop the current context and return to parent
     * 
     * Calls onExit on current context, closes all tracked files,
     * then calls onResume on parent context (if any).
     * 
     * @return true if popped successfully, false if stack empty
     */
    bool popContext();
    
    /**
     * @brief Get the current (topmost) context type
     */
    ContextType currentContext() const;
    
    /**
     * @brief Get the current context entry (read-only)
     * @return Pointer to current context, or nullptr if stack empty
     */
    const ContextEntry* currentContextEntry() const;
    
    /**
     * @brief Get the current stack depth
     * @return Number of contexts on stack (0 = empty)
     */
    int stackDepth() const { return stackTop + 1; }
    
    /**
     * @brief Check if a specific context type is anywhere in the stack
     */
    bool isContextActive(ContextType type) const;
    
    // =========================================================================
    // File Handle Tracking
    // =========================================================================
    // Centralized tracking of open files to prevent leaks when exiting contexts
    
    /**
     * @brief Register an open file handle for tracking
     * 
     * Files registered here will be automatically closed when popContext() is called.
     * 
     * @param file Pointer to open File object
     * @return true if registered, false if tracking array full
     */
    bool registerOpenFile(void* file);
    
    /**
     * @brief Unregister a file handle (call when you close it yourself)
     */
    void unregisterFile(void* file);
    
    /**
     * @brief Close and unregister all tracked files
     * 
     * Called automatically by popContext(), but can be called manually.
     */
    void closeAllTrackedFiles();
    
    /**
     * @brief Get count of currently tracked files
     */
    int trackedFileCount() const { return fileCount; }
    
    // =========================================================================
    // Zero-Copy Data Transfer
    // =========================================================================
    // Pass data between contexts without copying large buffers.
    // Prefer passing file paths over file contents.
    
    /**
     * @brief Set a file path for transfer to child/parent context
     * 
     * Use this instead of passing file contents as String.
     * The receiving context can open the file directly.
     * 
     * @param path File path (will be copied into internal buffer, max 127 chars)
     * @return true if set successfully
     */
    bool setTransferPath(const char* path);
    
    /**
     * @brief Get the transfer path set by another context
     * @return Path string, or empty string if none set
     */
    const char* getTransferPath() const { return transferPath; }
    
    /**
     * @brief Check if a transfer path is set
     */
    bool hasTransferPath() const { return transferPath[0] != '\0'; }
    
    /**
     * @brief Clear the transfer path
     */
    void clearTransferPath() { transferPath[0] = '\0'; }
    
    /**
     * @brief Set small data for transfer (copies into internal buffer)
     * 
     * For small essential data like cursor positions, flags, etc.
     * Max 256 bytes.
     * 
     * @param data Pointer to data
     * @param len Length in bytes (max 256)
     * @return true if copied successfully
     */
    bool setTransferData(const void* data, size_t len);
    
    /**
     * @brief Get transfer data set by another context
     * @param outLen Will be set to data length
     * @return Pointer to data, or nullptr if none set
     */
    const void* getTransferData(size_t* outLen) const;
    
    /**
     * @brief Clear transfer data
     */
    void clearTransferData() { transferDataLen = 0; }
    
    /**
     * @brief Clear all transfer state (path and data)
     * 
     * NOTE: This does NOT clear SharedBuffer - use SharedBuffer::getInstance().clear()
     * if you need to clear that too. SharedBuffer is intentionally separate as it
     * may be consumed by a different context than the one calling clearAllTransfers().
     */
    void clearAllTransfers() { clearTransferPath(); clearTransferData(); }
    
    // =========================================================================
    // Debugging
    // =========================================================================
    
    /**
     * @brief Print the current context stack to Serial
     */
    void printStack() const;
    
private:
    ContextManager();
    ~ContextManager() = default;
    
    static ContextManager* instance;
    
    // Context stack
    static const int MAX_STACK_DEPTH = 8;
    ContextEntry stack[MAX_STACK_DEPTH];
    int stackTop;  // -1 = empty, 0 = one item, etc.
    
    // File handle tracking
    static const int MAX_TRACKED_FILES = 8;
    void* openFiles[MAX_TRACKED_FILES];
    int fileCount;
    
    // Transfer buffers (for zero-copy data passing)
    char transferPath[128];          // File path for file-based transfer
    uint8_t transferBuffer[256];     // Small buffer for essential data
    size_t transferDataLen;
};

// Global reference for convenient access
extern ContextManager& contextManager;

// Global references to services for clean syntax (no need for getInstance())
// These are defined in JumperlOS.cpp (or their respective .cpp files)
extern Probing& probing;
extern Highlighting& highlighting;
extern Menus& menus;
extern Peripherals& peripherals;
extern SlotManager& slotManager;
extern jOSmanager& jOS;

// System service references
extern TermSerialService& termSerialService;
extern RelayedCommandService& relayedCommandService;
extern AsyncPassthroughService& asyncPassthroughService;
extern TinyUSBService& tinyUSBService;
extern PortHousekeepingService& portHousekeepingService;
extern LedDumpService& ledDumpService;
extern USBPeriodicService& usbPeriodicService;
extern OLEDService& oledService;
extern OledGuiService& oledGuiService;
extern LiveCrossbarService& liveCrossbarService;

#endif // JUMPERLOS_H

