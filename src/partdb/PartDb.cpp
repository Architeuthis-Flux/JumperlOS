// SPDX-License-Identifier: MIT
// Parts database lookups. The ONLY translation unit that may include the
// generated PartDbData.h (definitions, not declarations - projectFiles.h
// rule). No allocation anywhere: every result is a const pointer into the
// rodata tables.

#include "PartDb.h"

#include <string.h>  // memset/strncpy (partdbInstantiate)

#include "States.h"  // MAX_PART_PINS (24 V5 / 16 OG), PartDefinition

#include "PartDbData.h"

// Case fold for name compare - MUST stay in lockstep with fold() in
// scripts/generate_partdb.py: ASCII a-z -> A-Z, every other byte
// unchanged. partdb_names[] is sorted by exactly this fold.
static inline unsigned char pdbFold(char c) {
  return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : (unsigned char)c;
}

static int pdbFoldCmp(const char* a, const char* b) {
  while (*a && *b) {
    unsigned char fa = pdbFold(*a);
    unsigned char fb = pdbFold(*b);
    if (fa != fb) {
      return (int)fa - (int)fb;
    }
    a++;
    b++;
  }
  return (int)pdbFold(*a) - (int)pdbFold(*b);
}

void partdbInit(void) {
  // Tables are rodata in flash (XIP) - nothing to build, nothing to load.
}

const PartDbRecord* partdbFindByName(const char* name) {
  if (name == 0 || name[0] == '\0') {
    return 0;
  }
  int lo = 0;
  int hi = (int)partdb_numNames - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int cmp = pdbFoldCmp(name, partdb_names[mid].name);
    if (cmp == 0) {
      return &partdb_records[partdb_names[mid].recIdx];
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

const PartDbRecord* partdbRecordAt(uint16_t idx) {
  return (idx < partdb_numRecords) ? &partdb_records[idx] : 0;
}

uint16_t partdbNumRecords(void) {
  return partdb_numRecords;
}

const PartDbPinout* partdbPinoutOf(const PartDbRecord& r) {
  return &partdb_pinouts[r.pinoutIdx];
}

int partdbCandidatesForI2cAddr(uint8_t addr, const PartDbRecord** out,
                               int maxOut) {
  int n = 0;
  for (uint16_t i = 0; i < partdb_numRecords && n < maxOut; i++) {
    const PartDbRecord& r = partdb_records[i];
    if (r.i2cIdentIdx == 0xFF) {
      continue;
    }
    const PartDbI2cIdent& ident = partdb_i2cIdents[r.i2cIdentIdx];
    bool hit = false;
    if (ident.flags & PARTDB_I2C_FLAG_RANGE) {
      hit = (addr >= ident.addrs[0]) &&
            (addr < (uint8_t)(ident.addrs[0] + ident.numAddrs));
    } else {
      for (uint8_t a = 0; a < ident.numAddrs && a < 4; a++) {
        if (ident.addrs[a] == addr) {
          hit = true;
          break;
        }
      }
    }
    if (hit) {
      out[n++] = &r;
    }
  }
  return n;
}

bool partdbPlaceableHere(const PartDbRecord& r) {
  const PartDbPinout& po = partdb_pinouts[r.pinoutIdx];
  if (po.numPins > MAX_PART_PINS) {
    return false;
  }
  // Footprint fit mirrors PartDefinition::nodeForPin's geometry (States.h):
  // SIP runs down one 30-row half, DIP straddles both, axial2 is one column.
  switch (po.footprint) {
  case PARTDB_FOOT_SIP:
    return po.pinCount <= 30;
  case PARTDB_FOOT_DIP:
    return po.pinCount <= 60;
  case PARTDB_FOOT_AXIAL2:
    return po.pinCount == 2;
  }
  return false;
}

const uint16_t* partdbClassSlice(uint8_t partClass, uint16_t* countOut) {
  if (partClass >= PARTDB_NUM_CLASSES) {
    if (countOut) {
      *countOut = 0;
    }
    return 0;
  }
  const PartDbRange& rg = partdb_classRanges[partClass];
  if (countOut) {
    *countOut = rg.count;
  }
  return (rg.count > 0) ? &partdb_byClass[rg.start] : 0;
}

const uint16_t* partdbSubclassSlice(uint8_t partClass, uint8_t subClass,
                                    uint16_t* countOut) {
  if (partClass >= PARTDB_NUM_CLASSES || subClass >= PARTDB_MAX_SUBCLASSES) {
    if (countOut) {
      *countOut = 0;
    }
    return 0;
  }
  const PartDbRange& rg =
      partdb_subclassRanges[partClass * PARTDB_MAX_SUBCLASSES + subClass];
  if (countOut) {
    *countOut = rg.count;
  }
  return (rg.count > 0) ? &partdb_byClass[rg.start] : 0;
}

const char* partdbResolveDriver(const PartDefinition& p) {
  if (p.driverKey[0] != '\0') {
    return p.driverKey;
  }
  if (p.partId[0] == '\0') {
    return 0;
  }
  const PartDbRecord* r = partdbFindByName(p.partId);
  if (r == 0 || r->driverKey == 0 || r->driverKey[0] == '\0') {
    return 0;
  }
  return r->driverKey;
}

void partdbInstantiate(const PartDbRecord& r, PartDefinition& out) {
  memset(&out, 0, sizeof(out));

  // Name = the record id uppercased with '-' -> '_' (net names are
  // {NAME}_{PIN}, alphabet [A-Z0-9_] - makePinNetName's contract). The
  // caller dedupes against the live parts table (this function cannot see
  // it and must stay state-free).
  int i = 0;
  for (; r.id[i] != '\0' && i < (int)sizeof(out.name) - 1; i++) {
    char c = r.id[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (c == '-') {
      c = '_';
    }
    out.name[i] = c;
  }
  out.name[i] = '\0';

  strncpy(out.partId, r.id, sizeof(out.partId) - 1);
  strncpy(out.typeStr, partdb_typeStrs[r.typeStrIdx], sizeof(out.typeStr) - 1);
  if (r.defaultValue != 0) {
    strncpy(out.value, r.defaultValue, sizeof(out.value) - 1);
  }

  const PartDbPinout& po = partdb_pinouts[r.pinoutIdx];
  out.footprint = po.footprint;
  out.pinCount = po.pinCount;
  out.baseRow = -1;   // geometry is the caller's job (tap-to-place)
  out.placed = false;

  int n = po.numPins;
  if (n > MAX_PART_PINS) {
    n = MAX_PART_PINS;   // partdbPlaceableHere gates this before any placement
  }
  out.numPins = (uint8_t)n;
  for (int j = 0; j < n; j++) {
    const PartDbPin& sp = po.pins[j];
    strncpy(out.pins[j].name, sp.name, sizeof(out.pins[j].name) - 1);
    out.pins[j].pinNumber = sp.pinNumber;
    out.pins[j].offset = sp.offset;
    out.pins[j].connect = -1;   // NEVER wired, NEVER powered - the user does that
    out.pins[j].pinClass = sp.pinClass;
  }
}
