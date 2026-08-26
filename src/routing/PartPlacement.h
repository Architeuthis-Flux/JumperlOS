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
// INVARIANT: never change a part's `baseRow` or `placement` while
// `placed == true` other than as remove -> mutate -> reapply
// (guideMovePart(), GuidedFlow.cpp, is the only such mutator). Both functions
// above recompute their bridge endpoints from partPinNode, so removal MUST
// see the same geometry the apply used; mutating first strands every existing
// bridge on the fabric with nothing left that can find it again.
void partsReassertNetNames(JumperlessState& st);                           // after net rebuilds (names survive merges)
void makePinNetName(const PartDefinition& p, const PartPin& pin, char out[32]); // {NAME}_{PIN}, [A-Z0-9_], <=31 chars

// Shared with the MicroPython bindings (place_part / list_parts) so the pins
// grammar and the geometry rules have exactly ONE implementation.
//
// partPinNode() is THE geometry authority: it consults p.placement first
// (compact legs sit in their `connect:` hole) and falls through to the
// footprint/offset math otherwise. Every consumer - expandOnePart's bridges,
// removePartPlacement, the guide's _GUIDE_FP_/_GUIDE_TGT_ overlays,
// GuideChecks' row resolution, partsReassertNetNames and list_parts - calls
// it, so a mode change propagates everywhere without a second derivation.
int  partPinNode(const PartDefinition& p, const PartPin& pin);   // resolved node, -1 off-board
const char* partPinClassName(uint8_t pinClass);                  // "signal"|"power"|"gnd"|"nc"
int  parsePartPinsSpec(PartDefinition& p, const char* spec, String& err); // JSON/flow pins map -> p.pins[]

// Whole-part geometry predicate at p.baseRow: footprint sanity (pin count,
// even DIP/axial count, row 1-60), the per-footprint SPAN, and every LISTED
// pin resolving on the footprint - `offset:` forms included. ONE
// implementation shared by commitPart() (parse), jl_place_part() (API) and
// the guide's move-legality check, because an entry accepted by one and
// rejected by another is the silent-erasure class: it round-trips into the
// user's file and the next load drops it. Fills `reason` (a message
// fragment like "dip8 at row 58 does not fit the board (...)") when false.
bool partGeometryOk(const PartDefinition& p, char* reason, size_t reasonLen);

// True when `pin` would sit in its `connect:` hole under compact placement -
// i.e. that endpoint is a physical hole row (1-60 / TOP_RAIL / BOTTOM_RAIL),
// the pin is not `class: nc`, and the part is not a DIP. Exported for the
// guide's snap-refusal reasons ("no pin is compact-eligible").
bool partPinCompactEligible(const PartDefinition& p, const PartPin& pin);

#endif // PART_PLACEMENT_H
