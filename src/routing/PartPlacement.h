// SPDX-License-Identifier: MIT
#ifndef PART_PLACEMENT_H
#define PART_PLACEMENT_H

#include <Arduino.h>

class JumperlessState;
struct PartDefinition;
struct PartPin;

// Parts layer for the guided-placement / projects branch. Serializer/parser
// pair for the slot-YAML `parts:` section plus placement expansion and net
// naming. Format + parsing rules are documented in States.h; design in
// CodeDocs/DESIGN_GUIDED_PLACEMENT.md §1-§2/§6.
//
// Round-trip is load-bearing: toYAML is a wholesale rewrite, so the pair
// serializeParts/deserializeParts must stay matched or the SlotManager idle
// auto-save silently destroys the section.

void serializeParts(const JumperlessState& st, String& out);               // from toYAML
bool deserializeParts(JumperlessState& st, const char* yaml, String& err); // own section scan (post-loop pass)
int  expandPartsToBridges(JumperlessState& st, String& err);               // slot load: placed==true parts (idempotent)
int  applyPartPlacement(JumperlessState& st, int partIdx, String& err);    // one part (guide commit)
int  removePartPlacement(JumperlessState& st, int partIdx, String& err);   // back/undo
void partsReassertNetNames(JumperlessState& st);                           // after net rebuilds (names survive merges)
void makePinNetName(const PartDefinition& p, const PartPin& pin, char out[32]); // {NAME}_{PIN}, [A-Z0-9_], <=31 chars

// Shared with the MicroPython bindings (place_part / list_parts) so the pins
// grammar and the geometry rules have exactly ONE implementation.
int  partPinNode(const PartDefinition& p, const PartPin& pin);   // resolved node (offset wins), -1 off-board
const char* partPinClassName(uint8_t pinClass);                  // "signal"|"power"|"gnd"|"nc"
int  parsePartPinsSpec(PartDefinition& p, const char* spec, String& err); // JSON/flow pins map -> p.pins[]

#endif // PART_PLACEMENT_H
