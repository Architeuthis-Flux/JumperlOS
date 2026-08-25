// SPDX-License-Identifier: MIT
#ifndef DISPLAY_TYPES_H
#define DISPLAY_TYPES_H

#include <Arduino.h>
#include <stdint.h>

// Breadboard-display driving (Guides-Simplification workstream C).
//
// A display IS a part: its wiring is part bridges (the service writes the
// chosen RP_GPIO nodes into the part's DATA-role pins' `connect` fields and
// reapplies - persistence, teardown and crossbar visibility all ride the
// parts layer), its driver is a const descriptor in rodata (many part names
// -> one descriptor), and its pixels are pushed by DisplayService in bounded
// chunks. VCC/GND pins are NEVER routed: the user wires power, the beacon
// brings the panel alive the moment it answers - "you wire power, the
// Jumperless wires data."

enum class DispBus : uint8_t {
    I2C_SOFT,   // bit-banged on routed GPIOs - the default while the onboard
                // OLED owns hardware I2C1 (Kevin's simplicity ruling: separate
                // buses, no 0x3C collision, no parking)
    I2C_HW,     // Wire1 on (26,27) when the OLED sits on internal I2C0
    // SPI_HW / GPIO_PARALLEL: C-M3+ (JDI MIP, 40RGBX160, HD44780)
};

struct DisplayInstance;

// One vtable per FAMILY, not per model - model differences live in the
// descriptor's init table + quirks (the i2cscrn DRIVERS-tuple shape in C++).
struct DisplayOps {
    bool (*init)(DisplayInstance& d);          // run initSeq + quirks
    // Push the next bounded chunk of the frame. Returns >0 while mid-frame,
    // 0 when the frame completed, <0 on a bus error (-> beacon).
    int (*flushChunk)(DisplayInstance& d);
    void (*testPattern)(DisplayInstance& d, const char* label);  // "See this?"
    void (*sleep)(DisplayInstance& d);         // between Detect-Driver candidates
};

struct DisplayDriverDesc {
    const char* id;          // "ssd1306" | "sh1106" | ... (partdb driverKey)
    DispBus bus;
    uint16_t w, h;
    const uint8_t* initSeq;  // raw command bytes (mono-OLED family: one
                             // command byte per control write)
    uint16_t initLen;
    const DisplayOps* ops;
    struct {
        uint8_t colOffset;       // SH1106: 2 (132-wide RAM)
        bool pagedAddressing;    // SH1106: per-page pointer writes, no 0x20
        bool pageFlip;           // panel shows HALF the controller's RAM
                                 // (128x32 on a 64-row SSD1306): frames render
                                 // into the hidden half and a 1-byte display-
                                 // start-line command flips atomically -
                                 // tear-free at ANY bus speed
    } quirks;
    uint8_t i2cAddrs[3];     // 0-terminated candidate list
    uint16_t fbBytes;        // mono framebuffer size (w*h/8)
    bool safeToProbe;        // eligible for the Detect-Driver cycle
};

enum class DispState : uint8_t {
    EMPTY,      // no display part on the board
    ROUTED,     // bridges + pins acquired, waiting for the panel (beacon)
    ALIVE,      // panel answered + init ran: animating / flushing
    YIELDED,    // a user script claimed our pins - paused, warn-never-block
};

struct DisplayInstance {
    const DisplayDriverDesc* desc = nullptr;
    int8_t partIdx = -1;                // owning parts-table entry
    DispState state = DispState::EMPTY;

    // Bus (one of the two, per desc->bus)
    int8_t sdaPin = -1, sclPin = -1;    // RP2350 pin numbers
    uint8_t i2cAddr = 0;

    // Render
    uint8_t* fb = nullptr;              // heap on attach (fbBytes), freed on detach
    uint8_t* shadow = nullptr;          // what the PANEL currently holds - the
                                        // flush skips chunks that match, so a
                                        // frame costs only its changed bytes.
                                        // pageFlip panels: 2x fbBytes, one
                                        // image per RAM half
    uint8_t shadowValidMask = 0;        // bit per half; cleared on (re)init -
                                        // GDDRAM unknown, next frame flushes
                                        // fully. Non-flip drivers use bit 0.
    uint8_t backHalf = 0;               // pageFlip: the RAM half being WRITTEN
                                        // (the panel shows the other one)
    uint16_t flushCursor = 0;           // byte position within the current frame
    uint8_t chunkBytes = 16;            // per-tick flush budget (service-paced:
                                        // soft bus small, hardware larger,
                                        // halved under a modal load)
    bool midFrame = false;
    bool dirty = false;
    bool userOwnsContent = false;       // MP blit landed: background anim pauses

    // Liveness / pacing
    uint32_t nextBeaconMs = 0;
    uint32_t nextChunkMs = 0;           // soft-bus chunk pacing (~8 ms)
    uint32_t animNextMs = 0;
    int animFrame = 0;
    int animDir = 1;
};

#endif // DISPLAY_TYPES_H
