// SPDX-License-Identifier: MIT
#ifndef PARTDB_H
#define PARTDB_H

#include <stdint.h>

//==============================================================================
// Parts database - flash-resident part records for ambient labeling, the
// picker, and I2C auto-detect. THE header consumers include; the tables are
// generated into PartDbData.h (by scripts/generate_partdb.py from
// data/partdb/*.yaml), which only PartDb.cpp may include.
//
// Everything lives in rodata (XIP flash): no heap, no parsing, no RAM cost
// beyond what a caller copies out. All lookups are const pointers into the
// tables. Every identifier is PARTDB_-prefixed - consumers transitively
// include Arduino.h, where bare MOSI/MISO/SCK/INT/etc. are pin macros.
//==============================================================================

// Part classes (PartDbRecord.partClass).
enum PartDbClass : uint8_t {
  PARTDB_CLASS_LOGIC = 0,
  PARTDB_CLASS_ANALOG = 1,
  PARTDB_CLASS_DISCRETE = 2,
  PARTDB_CLASS_TRANSISTOR = 3,
  PARTDB_CLASS_DISPLAY = 4,
  PARTDB_CLASS_MODULE = 5,
  PARTDB_NUM_CLASSES = 6,
};

// Subclasses (PartDbRecord.subClass) - meaning depends on partClass.
// PARTDB_MAX_SUBCLASSES rows per class in partdb_subclassRanges.
enum PartDbSubclass : uint8_t {
  // LOGIC
  PARTDB_SUB_LOGIC_7400 = 0,
  PARTDB_SUB_LOGIC_4000 = 1,
  PARTDB_SUB_LOGIC_OTHER = 2,
  // ANALOG
  PARTDB_SUB_ANALOG_OPAMP = 0,
  PARTDB_SUB_ANALOG_CLOCK = 1,
  PARTDB_SUB_ANALOG_AUDIO = 2,
  PARTDB_SUB_ANALOG_POWER = 3,
  PARTDB_SUB_ANALOG_OTHER = 4,
  // DISCRETE
  PARTDB_SUB_DISCRETE_RESISTOR = 0,
  PARTDB_SUB_DISCRETE_CAPACITOR = 1,
  PARTDB_SUB_DISCRETE_LED = 2,
  PARTDB_SUB_DISCRETE_INDUCTOR = 3,
  PARTDB_SUB_DISCRETE_DIODE = 4,
  PARTDB_SUB_DISCRETE_POT = 5,
  // TRANSISTOR
  PARTDB_SUB_TRANSISTOR_BJT = 0,
  PARTDB_SUB_TRANSISTOR_MOSFET = 1,
  PARTDB_SUB_TRANSISTOR_OTHER = 2,
  // DISPLAY
  PARTDB_SUB_DISPLAY_OLED = 0,
  PARTDB_SUB_DISPLAY_LCD = 1,
  PARTDB_SUB_DISPLAY_MIP = 2,
  PARTDB_SUB_DISPLAY_LED_DIRECT = 3,
  PARTDB_SUB_DISPLAY_LED_DRIVER = 4,
  PARTDB_SUB_DISPLAY_LED_ADDR = 5,
  // MODULE
  PARTDB_SUB_MODULE_SENSOR = 0,
  PARTDB_SUB_MODULE_IO = 1,
  PARTDB_SUB_MODULE_MEMORY = 2,
  PARTDB_SUB_MODULE_OTHER = 3,

  PARTDB_MAX_SUBCLASSES = 6,
};

// Machine meaning of a pin (PartDbPin.role). Names stay the human labels
// (a module silk-screened SCL on an SPI bus carries role PARTDB_ROLE_SCK).
enum PartDbRole : uint8_t {
  PARTDB_ROLE_NONE = 0,
  PARTDB_ROLE_VCC = 1,
  PARTDB_ROLE_GND = 2,
  PARTDB_ROLE_SDA = 3,
  PARTDB_ROLE_SCL = 4,
  PARTDB_ROLE_MOSI = 5,
  PARTDB_ROLE_MISO = 6,
  PARTDB_ROLE_SCK = 7,
  PARTDB_ROLE_CS = 8,
  PARTDB_ROLE_DC = 9,
  PARTDB_ROLE_RST = 10,
  PARTDB_ROLE_BL = 11,
  PARTDB_ROLE_DIN = 12,
  PARTDB_ROLE_DOUT = 13,
  PARTDB_ROLE_CLK = 14,
  PARTDB_ROLE_EN = 15,
  PARTDB_ROLE_INT = 16,
  PARTDB_ROLE_ADDR = 17,
};

// Pin classes - same values as PartPin.pinClass (States.h).
enum PartDbPinClass : uint8_t {
  PARTDB_PINCLASS_SIGNAL = 0,
  PARTDB_PINCLASS_POWER = 1,
  PARTDB_PINCLASS_GND = 2,
  PARTDB_PINCLASS_NC = 3,
};

// Footprints - same values as PartDefinition.footprint (States.h).
enum PartDbFootprint : uint8_t {
  PARTDB_FOOT_SIP = 0,
  PARTDB_FOOT_DIP = 1,
  PARTDB_FOOT_AXIAL2 = 2,
};

// PartDbI2cIdent.flags bits.
#define PARTDB_I2C_FLAG_WHOAMI 0x01  // whoAmIReg/Value/Mask are valid
#define PARTDB_I2C_FLAG_RANGE 0x02   // addrs[0] = base of a contiguous run
                                     // of numAddrs (PCF8574's 0x20-0x27
                                     // doesn't fit addrs[4] as a list)

struct PartDbPin {
  const char* name;  // <= 11 chars - copies into PartPin.name[12] (B-M3)
  int8_t pinNumber;  // 1-based physical pin
  int8_t offset;     // -1 = derive from pinNumber + footprint (PartPin law)
  uint8_t pinClass;  // PartDbPinClass
  uint8_t role;      // PartDbRole
};

struct PartDbPinout {
  uint8_t footprint;  // PartDbFootprint
  uint8_t pinCount;   // PHYSICAL pins in the package (dip14 -> 14)
  uint8_t numPins;    // listed pins in pins[] (<= 24 = V5 MAX_PART_PINS)
  const PartDbPin* pins;
};

struct PartDbI2cIdent {
  uint8_t numAddrs;
  uint8_t addrs[4];     // list, or {base,0,0,0} with PARTDB_I2C_FLAG_RANGE
  uint8_t whoAmIReg;    // read-only probe - only ever read declared registers
  uint8_t whoAmIValue;
  uint8_t whoAmIMask;   // match: (read & mask) == (value & mask)
  uint8_t flags;        // PARTDB_I2C_FLAG_*
};

struct PartDbRecord {
  const char* id;           // <= 15 chars [A-Za-z0-9_-]; becomes partId[16]
  const char* displayName;  // <= 15 chars, OLED/terminal
  const char* ledName;      // <= 7 breadboard glyphs
  const char* menuName;     // one line <= 7 glyphs, or 7-padded + '\31' + <= 7
                            // (Graphics.cpp printString skips '\31' zero-width;
                            // the break is positional at glyph 7)
  const char* desc;         // one-liner
  uint8_t partClass;        // PartDbClass
  uint8_t subClass;         // PartDbSubclass (within partClass)
  uint8_t typeStrIdx;       // partdb_typeStrs[] - PartDefinition.typeStr source
  uint8_t i2cIdentIdx;      // partdb_i2cIdents[], 0xFF = none
  uint16_t pinoutIdx;       // partdb_pinouts[]
  uint16_t altPinoutIdx;    // 0xFFFF - reserved (alternate packages)
  uint16_t vectorSetIdx;    // 0xFFFF - reserved (phase-2 test vectors)
  const char* driverKey;    // display driver descriptor id, 0 = none
  const char* defaultValue; // "10k" etc for label-only discretes, 0 = none
};

// Name index entry: canonical ids + aliases, sorted by the same ASCII
// case fold partdbFindByName compares with (a-z -> A-Z, bytewise).
struct PartDbName {
  const char* name;
  uint16_t recIdx;
};

struct PartDbRange {
  uint16_t start;  // into partdb_byClass
  uint16_t count;
};

//==============================================================================
// Generated tables (definitions in PartDbData.h; ordering contract in its
// banner: records grouped class -> subclass -> authored popularity).
//==============================================================================

extern const PartDbPin partdb_pins[];
extern const PartDbPinout partdb_pinouts[];
extern const PartDbI2cIdent partdb_i2cIdents[];
extern const char* const partdb_typeStrs[];
extern const PartDbRecord partdb_records[];
extern const PartDbName partdb_names[];
extern const uint16_t partdb_byClass[];
extern const PartDbRange partdb_classRanges[];     // [PARTDB_NUM_CLASSES]
extern const PartDbRange partdb_subclassRanges[];  // [classes * subclasses]
extern const uint16_t partdb_numRecords;
extern const uint16_t partdb_numPinouts;
extern const uint16_t partdb_numPins;
extern const uint16_t partdb_numNames;
extern const uint16_t partdb_numI2cIdents;
extern const uint16_t partdb_numTypeStrs;

//==============================================================================
// API - no allocation on any path.
//==============================================================================

// No-op for rodata tables; kept for init-order symmetry with the other
// subsystems.
void partdbInit(void);

// Case-insensitive exact match over ids + aliases (binary search).
// NULL when absent.
const PartDbRecord* partdbFindByName(const char* name);

const PartDbRecord* partdbRecordAt(uint16_t idx);  // NULL out of range
uint16_t partdbNumRecords(void);

const PartDbPinout* partdbPinoutOf(const PartDbRecord& r);

// Fills out[] with records whose I2C ident covers addr; returns the count
// written (at most maxOut).
int partdbCandidatesForI2cAddr(uint8_t addr, const PartDbRecord** out,
                               int maxOut);

// Can this record be placed on THIS board? Listed pins <= MAX_PART_PINS
// (24 V5 / 16 OG) and the footprint fits the 60-row board (SIP within a
// 30-row half, DIP across it, axial2 always).
bool partdbPlaceableHere(const PartDbRecord& r);

// Picker slices: pointer into partdb_byClass (canonical records only,
// most-common-first) + count. NULL/0 for an empty or invalid class.
const uint16_t* partdbClassSlice(uint8_t partClass, uint16_t* countOut);
const uint16_t* partdbSubclassSlice(uint8_t partClass, uint8_t subClass,
                                    uint16_t* countOut);

// DB record -> PartDefinition (B-M3). connect = -1 on EVERY pin - a placed
// part is pure geometry + labels, never wired, never powered; the user does
// that. baseRow = -1 and placed = false: geometry (tap-to-place, anchor
// mapping, partGeometryOk) is the caller's job, as is deduping the name
// against the live parts table. State-free: reads only the rodata tables.
struct PartDefinition;
void partdbInstantiate(const PartDbRecord& r, PartDefinition& out);

// The ONE display-driver binding authority (B-M5, plan reconciliation 1).
// Precedence: the part's own driverKey when set (Detect-Driver confirm /
// hand edit), else partId -> record (id OR alias, case-fold) -> the
// record's driverKey. NULL when the part binds no driver. Consumers pass
// the returned key to the display layer's own registry (findDisplayDriver)
// - this function stays partdb-pure and never sees driver descriptors.
const char* partdbResolveDriver(const PartDefinition& p);

#endif  // PARTDB_H
