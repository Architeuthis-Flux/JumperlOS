// SPDX-License-Identifier: MIT
// Parts layer: serializer/parser for the slot-YAML `parts:` section plus
// placement expansion and {NAME}_{PIN} net naming. Format and parsing rules
// are documented in States.h; design in CodeDocs/DESIGN_GUIDED_PLACEMENT.md.
//
// ROUND-TRIP IS LOAD-BEARING: toYAML is a wholesale rewrite - the SlotManager
// idle auto-save destroys any section the serializer doesn't emit. Every
// field deserializeParts() accepts MUST be re-emitted by serializeParts().

#include "PartPlacement.h"
#include "States.h"
#include "NetManager.h"   // findNodeInNet
#include "config.h"       // jumperlessConfig (warning gating)

// States.cpp's node-name resolution - the exact helper `bridges:` uses.
extern int parseNodeName(const String& nodeName);
extern String nodeValueToString(int nodeValue);
extern bool parseBoolean(const String& val, bool& success);

// ---------------------------------------------------------------------------
// PartsState / PartDefinition members
// ---------------------------------------------------------------------------

void PartsState::clear() {
    memset(this, 0, sizeof(*this));
    // All-zero is the valid empty state: numParts = 0 means nothing reads
    // pins[] until deserializeParts fills entries with proper -1 sentinels.
}

int PartsState::findByName(const char* n) const {
    if (n == nullptr || n[0] == '\0') return -1;
    for (int i = 0; i < numParts && i < MAX_PARTS; i++) {
        if (strcmp(parts[i].name, n) == 0) return i;
    }
    return -1;
}

int PartDefinition::nodeForPin(int k) const {
    if (k < 1 || k > pinCount) return -1;

    if (footprint == 2) {
        // axial2: a two-leg part straddling the ravine by default (Kevin's
        // bench convention for resistors/diodes - wave 2). pin 1 = baseRow
        // (top half, 1-30), pin 2 = baseRow+30 (same column, across the
        // ravine). No near/far math: the two legs are always exactly 30
        // apart.
        if (baseRow < 1 || baseRow > 30) return -1;
        return (k == 1) ? baseRow : (baseRow + 30);
    }

    if (baseRow < 1 || baseRow > 60) return -1;

    if (footprint == 0) {
        // SIP: legs march along one side, either half.
        int node = baseRow + (k - 1);
        bool top = (baseRow <= 30);
        if (top  && (node < 1  || node > 30)) return -1;
        if (!top && (node < 31 || node > 60)) return -1;
        return node;
    }

    // DIP: U-shaped numbering. Bench verdict (wave 2, photo-confirmed): real
    // chips sit dot/notch at bottom-left, pin 1 on the BOTTOM half - a
    // top-anchored baseRow (<=30) was the mirrored bug and is no longer a
    // valid anchor at all (the old +30 top-anchored branch is gone, not just
    // relabeled). node n-30 is the same column across the ravine.
    if (baseRow <= 30) return -1;
    int half = pinCount / 2;
    if (k <= half) {
        // near side: bottom half, left->right
        int node = baseRow + (k - 1);
        if (node < 31 || node > 60) return -1;
        return node;
    }
    // far side: top half, right->left
    int node = (baseRow - 30) + (pinCount - k);
    if (node < 1 || node > 30) return -1;
    return node;
}

// FOOTPRINT geometry for one pin: `offset` wins when >= 0 (same-side offset
// from baseRow); otherwise the footprint math on the 1-based physical pin
// number. This is what `placement: expanded|custom` uses directly and what a
// compact part's non-eligible legs fall back to.
static int partPinFootprintNode(const PartDefinition& p, const PartPin& pin) {
    if (pin.offset >= 0) {
        int node = p.baseRow + pin.offset;
        if (node < 1 || node > 60) return -1;
        if ((p.baseRow <= 30) != (node <= 30)) return -1;  // must not cross the ravine
        return node;
    }
    return p.nodeForPin(pin.pinNumber);
}

// A leg can only physically go into a HOLE: the 60 breadboard rows plus the
// two rail hole rows. GND (100) and every other fabric-only node (DAC/ADC/
// GPIO/ISENSE) lives in the crossbar and has no holes at all, which is why
// compact is per-pin and not all-or-nothing.
static bool partNodeIsHoleRow(int node) {
    return (node >= 1 && node <= 60) || node == TOP_RAIL || node == BOTTOM_RAIL;
}

bool partPinCompactEligible(const PartDefinition& p, const PartPin& pin) {
    if (p.footprint == 1) return false;   // ICs never compact - legs ARE the footprint
    if (pin.pinClass == 3) return false;  // nc: the leg is deliberately unconnected
    if (pin.connect < 0) return false;    // open leg: follows its partner instead
    return partNodeIsHoleRow(pin.connect);
}

// The compact node of the ONE other compact-eligible pin, for the open-leg
// follow rule. -1 when there is no partner or when there are several (3+ pin
// parts have no unambiguous "the other leg", so an open leg on one just keeps
// its footprint row).
static int partCompactPartnerNode(const PartDefinition& p, const PartPin& self) {
    int found = -1;
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        const PartPin& other = p.pins[j];
        if (&other == &self || strcmp(other.name, self.name) == 0) continue;
        if (!partPinCompactEligible(p, other)) continue;
        if (found >= 0) return -1;   // ambiguous
        found = other.connect;
    }
    return found;
}

// THE geometry authority (PartPlacement.h): the node a leg actually occupies.
// Mode-aware by design, so bridges, removal, overlays, checks, net names and
// list_parts all inherit a placement change without re-deriving anything.
//
// Compact (design §2.2): a compact-eligible leg IS its endpoint, so it
// returns pin.connect - and bridge suppression then comes out FREE, because
// expandOnePart already skips `node == pin.connect`. Everything else falls
// through to the footprint math above.
int partPinNode(const PartDefinition& p, const PartPin& pin) {
    if (p.placement == PART_PLACEMENT_COMPACT) {
        if (partPinCompactEligible(p, pin)) return pin.connect;
        // Open leg (no `connect:`, not nc): it has no endpoint to jump to, so
        // it follows its partner into the adjacent row - the radial default
        // from the photo (LED anode in row 22, cathode in 23). The fallback is
        // -1 at a HALF boundary, not just at row 60: node 30 and node 31 are
        // opposite corners of the board, so partner+1 must stay in the same
        // half. A partner sitting in a RAIL (or no unambiguous partner at all)
        // leaves the open leg on its footprint row.
        if (pin.connect < 0 && pin.pinClass != 3) {
            int partner = partCompactPartnerNode(p, pin);
            if (partner >= 1 && partner <= 60) {
                bool top = (partner <= 30);
                int adj = partner + 1;
                if ((top && adj > 30) || (!top && adj > 60)) adj = partner - 1;
                if (adj >= 1 && ((top && adj <= 30) || (!top && adj >= 31))) return adj;
            }
        }
    }
    return partPinFootprintNode(p, pin);
}

// Whole-part geometry predicate - see PartPlacement.h for why it is shared.
bool partGeometryOk(const PartDefinition& p, char* reason, size_t reasonLen) {
    if (reason != nullptr && reasonLen > 0) reason[0] = '\0';
    // pinCount is the footprint's PHYSICAL pin count - bound it by the board
    // geometry (60 rows), NOT by MAX_PART_PINS, which only caps how many pins
    // are LISTED (numPins, enforced in parsePinEntry). Bounding it by storage
    // silently dropped a well-formed dip28/dip40 - and the next auto-save then
    // erased it from the user's slot file.
    if (p.pinCount < 1 || p.pinCount > 60) {
        snprintf(reason, reasonLen, "footprint pin count must be 1-60");
        return false;
    }
    if (p.footprint != 0 && (p.pinCount % 2) != 0) {
        snprintf(reason, reasonLen, "a dip/axial footprint needs an even pin count");
        return false;
    }
    if (p.baseRow < 1 || p.baseRow > 60) {
        snprintf(reason, reasonLen, "row: must be 1-60");
        return false;
    }

    if (p.footprint == 1) {
        // DIP pin 1 (row:) must anchor the bottom half - the old top-anchored
        // mapping was the mirrored bug (wave 2, bench-found). nodeForPin()
        // would already return -1 for every pin of a top-anchored DIP, but
        // that alone only fails PLACEMENT (0 bridges); rejecting the entry
        // itself keeps it from round-tripping through an idle auto-save.
        if (p.baseRow < 31 || p.baseRow > 60) {
            snprintf(reason, reasonLen,
                     "dip pin 1 (row:) must be on the bottom half (31-60)");
            return false;
        }
        // Column-fit: the U-shape's far side must land within the top half -
        // (row-30) + N/2 - 1 <= 30 (binding geometry). Below this bound the
        // near side overflows past row 60 at the SAME row the far side
        // overflows past row 30 (both sides share one inequality), so a chip
        // that doesn't fit would otherwise place SOME pins and silently drop
        // the rest - a partial chip is never right, so the whole entry is
        // rejected, not just the pins that don't fit.
        int half = p.pinCount / 2;
        if ((p.baseRow - 30) + (half - 1) > 30) {
            snprintf(reason, reasonLen,
                     "dip%d at row %d does not fit the board (far-side columns run past row 30)",
                     (int)p.pinCount, (int)p.baseRow);
            return false;
        }
    } else if (p.footprint == 0) {
        // SIP: the whole run must stay on baseRow's half - nodeForPin()
        // already refuses a leg that crosses the ravine, so an over-length
        // SIP strip is the same partial-chip failure mode as an oversized DIP.
        bool top = (p.baseRow <= 30);
        int lastNode = p.baseRow + (p.pinCount - 1);
        if ((top && lastNode > 30) || (!top && lastNode > 60)) {
            snprintf(reason, reasonLen,
                     "sip%d at row %d does not fit the %s half (runs past row %d)",
                     (int)p.pinCount, (int)p.baseRow, top ? "top" : "bottom",
                     top ? 30 : 60);
            return false;
        }
    } else if (p.footprint == 2) {
        // axial2: a two-leg part straddling the ravine - pin 1 (row:) must be
        // on the top half so pin 2 (row+30) lands on-board. No separate span
        // check needed: the legs are always exactly 30 apart.
        if (p.pinCount != 2) {
            snprintf(reason, reasonLen, "axial2 must have exactly 2 pins");
            return false;
        }
        if (p.baseRow < 1 || p.baseRow > 30) {
            snprintf(reason, reasonLen,
                     "axial2 pin 1 (row:) must be on the top half (1-30)");
            return false;
        }
    }

    // Every LISTED pin must resolve on the footprint. The span checks above
    // cover the footprint's own run, but a listed pin can still leave the
    // board two ways they never saw: an `offset:` (which bypasses nodeForPin
    // entirely and used to carry only its own private bounds test - wave-2
    // gap, routed here from task 3's re-review) and a `pin:` number beyond
    // pinCount. Both used to be accepted and then placed PARTIALLY, which is
    // the same silent-partial-loss class the span checks exist to stop.
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        const PartPin& pin = p.pins[j];
        if (partPinFootprintNode(p, pin) >= 0) continue;
        if (pin.offset >= 0) {
            snprintf(reason, reasonLen,
                     "pin %s (offset: %d) does not land on the board from row %d",
                     pin.name, (int)pin.offset, (int)p.baseRow);
        } else {
            snprintf(reason, reasonLen,
                     "pin %s (pin: %d) does not land on the board from row %d",
                     pin.name, (int)pin.pinNumber, (int)p.baseRow);
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

void makePinNetName(const PartDefinition& p, const PartPin& pin, char out[32]) {
    // {NAME}_{PIN} uppercased, sanitized to [A-Z0-9_], <= 31 chars
    // (NetNameEntry::name[32]).
    char raw[32];
    snprintf(raw, sizeof(raw), "%s_%s", p.name, pin.name);
    int w = 0;
    for (int r = 0; raw[r] != '\0' && w < 31; r++) {
        char c = raw[r];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
        out[w++] = c;
    }
    out[w] = '\0';
}

// Is `name` one of the auto names this state's parts table would generate?
// (Custom names carry no origin flag, so "came from parts" is decided by
// pattern-matching against the parts table - an explicit nets:-section name
// that happens to differ from every auto name outranks auto naming.)
static bool isPartsAutoName(const JumperlessState& st, const char* name) {
    char buf[32];
    for (int i = 0; i < st.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = st.parts.parts[i];
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            makePinNetName(p, p.pins[j], buf);
            if (strcmp(buf, name) == 0) return true;
        }
    }
    return false;
}

void partsReassertNetNames(JumperlessState& st) {
    if (st.parts.numParts <= 0) return;
    // Lowest part index wins per net, per pass.
    bool claimed[MAX_NETS] = { false };
    for (int i = 0; i < st.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = st.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            int node = partPinNode(p, pin);
            if (node < 0) continue;
            int netNum = findNodeInNet(node);
            if (netNum <= 0 || netNum >= MAX_NETS) continue;
            // findNodeInNet's gpio/adc fallbacks return NODE values, not net
            // numbers - only accept a real net at that index.
            if (st.connections.nets[netNum].number != netNum) continue;
            // Never rename GND / rails / DACs - their names are authoritative.
            if (st.connections.nets[netNum].specialFunction > 0) continue;
            if (claimed[netNum]) continue;
            char autoName[32];
            makePinNetName(p, pin, autoName);
            const char* existing = st.display.getNetName(netNum);
            if (existing != nullptr && existing[0] != '\0' &&
                strcmp(existing, autoName) != 0 && !isPartsAutoName(st, existing)) {
                // An explicit nets:-section custom name outranks auto names.
                claimed[netNum] = true;
                continue;
            }
            st.display.setNetName(netNum, autoName);  // also refreshes firstNode
            claimed[netNum] = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Expansion (bridges)
// ---------------------------------------------------------------------------

static int expandOnePart(JumperlessState& st, const PartDefinition& p, String& err) {
    int added = 0;
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        const PartPin& pin = p.pins[j];
        if (pin.connect < 0) continue;   // leg occupies the hole, no bridge
        int node = partPinNode(p, pin);
        if (node < 0) {
            err += "part " + String(p.name) + " pin " + String(pin.name) + ": off-board; ";
            continue;
        }
        if (node == pin.connect) continue;
        // Idempotency lives HERE: bare addConnection on an existing bridge
        // bumps its duplicate count (and re-dirties the state), so skip
        // exact duplicates up front via hasConnection.
        if (st.hasConnection(node, pin.connect)) continue;
        String aerr;
        if (st.addConnection(node, pin.connect, aerr)) {
            added++;
        } else {
            err += "part " + String(p.name) + " pin " + String(pin.name) + ": " + aerr + "; ";
        }
    }
    return added;
}

int expandPartsToBridges(JumperlessState& st, String& err) {
    int added = 0;
    for (int i = 0; i < st.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = st.parts.parts[i];
        if (!p.placed) continue;
        added += expandOnePart(st, p, err);
    }
    return added;
}

int applyPartPlacement(JumperlessState& st, int partIdx, String& err) {
    if (partIdx < 0 || partIdx >= st.parts.numParts) {
        err = "bad part index " + String(partIdx);
        return -1;
    }
    PartDefinition& p = st.parts.parts[partIdx];
    int added = expandOnePart(st, p, err);
    if (!p.placed) {
        p.placed = true;      // serialized field - dirty even when 0 bridges
        st.markDirty();
    }
    return added;
}

int removePartPlacement(JumperlessState& st, int partIdx, String& err) {
    if (partIdx < 0 || partIdx >= st.parts.numParts) {
        err = "bad part index " + String(partIdx);
        return -1;
    }
    PartDefinition& p = st.parts.parts[partIdx];
    int removed = 0;
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        const PartPin& pin = p.pins[j];
        if (pin.connect < 0) continue;
        int node = partPinNode(p, pin);
        if (node < 0) continue;
        if (!st.hasConnection(node, pin.connect)) continue;
        String rerr;
        if (st.removeConnection(node, pin.connect, rerr)) {
            removed++;
        } else {
            err += "part " + String(p.name) + " pin " + String(pin.name) + ": " + rerr + "; ";
        }
    }
    if (p.placed) {
        p.placed = false;
        st.markDirty();
    }
    return removed;
}

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

const char* partPinClassName(uint8_t pinClass) {
    switch (pinClass) {
        case 1: return "power";
        case 2: return "gnd";
        case 3: return "nc";
        default: return "signal";
    }
}

static uint8_t pinClassFromString(const String& s) {
    if (s == "power") return 1;
    if (s == "gnd") return 2;
    if (s == "nc") return 3;
    return 0;  // signal (also the tolerant default for unknown classes)
}

void serializeParts(const JumperlessState& st, String& out) {
    const PartsState& ps = st.parts;
    if (ps.numParts <= 0) {
        return;  // omit the section entirely when empty (existing files unchanged)
    }
    out += "parts:\n";
    for (int i = 0; i < ps.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = ps.parts[i];
        out += "  - name: \"" + String(p.name) + "\"\n";
        if (p.typeStr[0] != '\0') out += "    type: " + String(p.typeStr) + "\n";
        if (p.value[0] != '\0')   out += "    value: \"" + String(p.value) + "\"\n";
        if (p.partId[0] != '\0')  out += "    part_id: \"" + String(p.partId) + "\"\n";
        const char* fpName = (p.footprint == 1) ? "dip" : (p.footprint == 2) ? "axial" : "sip";
        out += "    footprint: " + String(fpName) + String(p.pinCount) + "\n";
        out += "    row: " + String(p.baseRow) + "\n";
        out += "    placed: " + String(p.placed ? "true" : "false") + "\n";
        // Emitted only when non-default (the defaultVerify/color precedent),
        // so every pre-wave-2 file is byte-identical after a rewrite. The
        // parser below is its matched half and lands in the SAME commit - a
        // parsed-but-not-emitted field is erased by the next idle auto-save.
        if (p.placement == PART_PLACEMENT_COMPACT) {
            out += "    placement: compact\n";
        } else if (p.placement == PART_PLACEMENT_CUSTOM) {
            out += "    placement: custom\n";
        }
        if (p.defaultVerify != 0) out += "    verify: " + String(p.defaultVerify) + "\n";
        if (p.outlineColor != 0) {
            char hexBuf[12];
            snprintf(hexBuf, sizeof(hexBuf), "0x%06lX", (unsigned long)(p.outlineColor & 0xFFFFFFul));
            out += "    color: " + String(hexBuf) + "\n";
        }
        if (p.numPins > 0) {
            out += "    pins:\n";
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                const PartPin& pin = p.pins[j];
                out += "      " + String(pin.name) + ": {";
                bool first = true;
                if (pin.pinNumber >= 0) {
                    out += "pin: " + String(pin.pinNumber);
                    first = false;
                }
                if (pin.offset >= 0) {
                    if (!first) out += ", ";
                    out += "offset: " + String(pin.offset);
                    first = false;
                }
                if (pin.connect >= 0) {
                    if (!first) out += ", ";
                    out += "connect: " + nodeValueToString(pin.connect);
                    first = false;
                }
                if (!first) out += ", ";
                out += "class: " + String(partPinClassName(pin.pinClass));
                out += "}\n";
            }
        }
    }
    out += "\n";
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Extract "key: value" from a flow-map body ("{pin: 1, connect: GND}").
// Returns trimmed value up to the next ',' or '}', or "" when absent.
// The key must start a field (preceded by '{', ',' or whitespace) so that
// "pin:" can never match inside an unknown key like "spin:".
static String extractFlowField(const String& body, const char* key) {
    int idx = -1;
    int from = 0;
    while (true) {
        int cand = body.indexOf(key, from);
        if (cand < 0) return String("");
        char before = (cand == 0) ? '{' : body.charAt(cand - 1);
        if (before == '{' || before == ',' || before == ' ' || before == '\t') {
            idx = cand;
            break;
        }
        from = cand + 1;
    }
    int start = idx + strlen(key);
    int commaIdx = body.indexOf(',', start);
    int braceIdx = body.indexOf('}', start);
    int endIdx = body.length();
    if (commaIdx >= 0 && (braceIdx < 0 || commaIdx < braceIdx)) endIdx = commaIdx;
    else if (braceIdx >= 0) endIdx = braceIdx;
    String val = body.substring(start, endIdx);
    val.trim();
    return val;
}

// Parse a quoted-or-bare scalar value.
static String parseScalar(const String& rest) {
    String v = rest;
    v.trim();
    if (v.length() >= 2 && v.charAt(0) == '"') {
        int endQuote = v.indexOf('"', 1);
        if (endQuote > 0) return v.substring(1, endQuote);
    }
    return v;
}

// One pin from its name + flow-map body. Malformed pins are skipped (warn).
static void parsePinEntry(PartDefinition& p, const String& pinName, const String& body, String& err) {
    if (p.numPins >= MAX_PART_PINS) {
        err += "part " + String(p.name) + ": too many pins; ";
        return;
    }
    PartPin pin;
    memset(&pin, 0, sizeof(pin));
    pin.pinNumber = -1;
    pin.offset = -1;
    pin.connect = -1;
    pin.pinClass = 0;

    String nm = pinName;
    nm.trim();
    if (nm.length() == 0 || nm.length() > 11) {
        err += "part " + String(p.name) + ": bad pin name '" + nm + "'; ";
        return;
    }
    // A leading '-' or '#' can never survive serializeParts: pins are emitted
    // UNQUOTED as `      <NAME>: {...}`, so "- X" comes back as a parts-list
    // entry marker and "#X" as a comment. Refuse it HERE too (the inline
    // `pins: {...}` form and place_part's pins_json both land in this
    // function) so a name that cannot round-trip never enters the table -
    // the same store-only-what-reloads rule commitPart() enforces for the
    // part itself. This is the FIRST line of defence on every path - the
    // API's own leading-char check (jl_place_part) sits downstream of
    // parsePartPinsSpec and is the matched second copy, kept so the two
    // predicates can't drift.
    if (nm.charAt(0) == '-' || nm.charAt(0) == '#') {
        err += "part " + String(p.name) + ": pin name '" + nm +
               "' may not start with '-' or '#'; ";
        return;
    }
    strncpy(pin.name, nm.c_str(), sizeof(pin.name) - 1);

    String v = extractFlowField(body, "pin:");
    if (v.length() > 0) pin.pinNumber = (int8_t)v.toInt();
    v = extractFlowField(body, "offset:");
    if (v.length() > 0) pin.offset = (int8_t)v.toInt();
    v = extractFlowField(body, "connect:");
    if (v.length() > 0) {
        int node = parseNodeName(v);   // same resolution bridges: uses
        if (node >= 0) {
            pin.connect = (int16_t)node;
        } else {
            err += "part " + String(p.name) + " pin " + nm + ": unknown node '" + v + "'; ";
        }
    }
    v = extractFlowField(body, "class:");
    if (v.length() > 0) pin.pinClass = pinClassFromString(v);

    if (pin.pinNumber < 0 && pin.offset < 0) {
        err += "part " + String(p.name) + " pin " + nm + ": needs pin: or offset:; ";
        return;
    }
    p.pins[p.numPins++] = pin;
}

// Inline pins map: pins: {A: {pin: 1, connect: 7}, B: {pin: 2}}
static void parseInlinePins(PartDefinition& p, const String& rest, String& err) {
    int outer = rest.indexOf('{');
    if (outer < 0) return;
    int i = outer + 1;
    int len = rest.length();
    while (i < len) {
        // skip separators
        while (i < len && (rest.charAt(i) == ' ' || rest.charAt(i) == ',' || rest.charAt(i) == '\t')) i++;
        if (i >= len || rest.charAt(i) == '}') break;
        int colon = rest.indexOf(':', i);
        if (colon < 0) break;
        String pinName = rest.substring(i, colon);
        int braceStart = rest.indexOf('{', colon);
        if (braceStart < 0) break;
        int braceEnd = rest.indexOf('}', braceStart);
        if (braceEnd < 0) braceEnd = len - 1;
        parsePinEntry(p, pinName, rest.substring(braceStart, braceEnd + 1), err);
        i = braceEnd + 1;
    }
}

// Pins map handed in from MicroPython (place_part's `pins_json`):
//   {"A": {"pin": 1, "connect": "GND"}, "B": {"pin": 2, "connect": 7}}
// Quotes are stripped (both kinds - a bare Python dict repr uses ') and the
// rest goes through the SAME inline-flow-map parser the YAML
// `pins: {A: {...}}` form uses, so the two entry points can never grow
// different grammars. Returns the number of pins appended to p.
int parsePartPinsSpec(PartDefinition& p, const char* spec, String& err) {
    if (spec == nullptr) return 0;
    String flow;
    flow.reserve(strlen(spec));
    for (const char* c = spec; *c != '\0'; c++) {
        if (*c == '"' || *c == '\'') continue;
        flow += *c;
    }
    int before = p.numPins;
    parseInlinePins(p, flow, err);
    return p.numPins - before;
}

static void partInit(PartDefinition& p) {
    memset(&p, 0, sizeof(p));
    p.baseRow = -1;
}

// Commit the in-progress part. Malformed entries are skipped with a warning,
// like bridges.
static void commitPart(JumperlessState& st, PartDefinition& p, bool& open, bool& bad, String& err) {
    if (!open) return;
    open = false;
    // Geometry rejections are UNCONDITIONAL prints, matching the MAX_PARTS
    // branch further down (and unlike it, these are the failure mode every
    // pre-wave-2 user file hits): the only other surfacing point, States.cpp's
    // "parts: parse warnings", sits behind debug.show_node_errors, so with
    // that off a stale top-anchored DIP (or an off-board footprint span, or an
    // out-of-bounds `offset:`) was dropped with zero output, and the next idle
    // auto-save erased it from the file for good.
    //
    // The predicate itself is partGeometryOk() - the SAME one jl_place_part()
    // applies on the API path and the guide's move check applies before a
    // move. `bad` and the name are parse state, not geometry, so they stay
    // here.
    const char* pname = p.name[0] ? p.name : "(unnamed)";
    bool valid = !bad && p.name[0] != '\0';
    if (valid) {
        char reason[128];
        if (!partGeometryOk(p, reason, sizeof(reason))) {
            valid = false;
            Serial.print("part '"); Serial.print(pname); Serial.print("': ");
            Serial.print(reason); Serial.println("; dropped.");
            err += "part " + String(pname) + ": " + String(reason) + "; ";
        }
    }
    // A hand-written `placement: compact` on a DIP is a lying byte: compact is
    // refused for ICs everywhere else, so normalize it HERE rather than only
    // guarding it in partPinNode - the serializer then heals the file on the
    // next save instead of carrying the contradiction forever.
    if (valid && p.placement == PART_PLACEMENT_COMPACT && p.footprint == 1) {
        p.placement = PART_PLACEMENT_EXPANDED;
        Serial.print("part '"); Serial.print(pname);
        Serial.println("': ICs don't compact - placement reset to expanded.");
        err += "part " + String(pname) +
               ": ICs don't compact - placement reset to expanded; ";
    }
    if (!valid) {
        err += "skipped malformed part entry '" + String(p.name) + "'; ";
        if (jumperlessConfig.debug.show_node_errors) {
            Serial.print("Skipping malformed parts: entry ");
            Serial.println(p.name[0] ? p.name : "(unnamed)");
        }
        bad = false;
        return;
    }
    if (st.parts.numParts >= MAX_PARTS) {
        err += "too many parts (max " + String(MAX_PARTS) + "); ";
        // UNCONDITIONAL, unlike the malformed-entry warning above. This is the
        // same erasure class as the dip28 bounds bug: the part parsed fine, it
        // just didn't fit, and because toYAML is a wholesale rewrite the next
        // idle auto-save deletes it from the user's slot file for good. The
        // only other surfacing point (States.cpp's "parts: parse warnings")
        // sits behind debug.show_node_errors, so with that off the loss was
        // completely silent - a V5-authored file opened on an OG (MAX_PARTS 6)
        // drops everything from part 7 on without a word.
        Serial.print("Dropping part '");
        Serial.print(p.name[0] ? p.name : "(unnamed)");
        Serial.print("' - too many parts (max ");
        Serial.print(MAX_PARTS);
        Serial.println("); it will be lost on the next save.");
        return;
    }
    st.parts.parts[st.parts.numParts++] = p;
}

// Field line inside a part entry (already trimmed, "- " prefix stripped).
// Unknown keys are skipped without error (part-ID branch hook).
static void parsePartLine(PartDefinition& p, const String& line, bool& inPins, bool& bad, String& err) {
    int colon = line.indexOf(':');
    if (colon < 0) return;   // not a key: line - ignore
    String key = line.substring(0, colon);
    key.trim();
    String rest = line.substring(colon + 1);
    rest.trim();

    if (key == "name") {
        String v = parseScalar(rest);
        if (v.length() == 0 || v.length() > 15) { bad = true; return; }
        strncpy(p.name, v.c_str(), sizeof(p.name) - 1);
        inPins = false;
    } else if (key == "type") {
        String v = parseScalar(rest);
        strncpy(p.typeStr, v.c_str(), sizeof(p.typeStr) - 1);
        inPins = false;
    } else if (key == "value") {
        String v = parseScalar(rest);
        strncpy(p.value, v.c_str(), sizeof(p.value) - 1);
        inPins = false;
    } else if (key == "part_id") {
        String v = parseScalar(rest);
        strncpy(p.partId, v.c_str(), sizeof(p.partId) - 1);
        inPins = false;
    } else if (key == "footprint") {
        String v = parseScalar(rest);
        v.toLowerCase();
        if (v.startsWith("dip")) {
            p.footprint = 1;
            p.pinCount = (uint8_t)v.substring(3).toInt();
        } else if (v.startsWith("sip")) {
            p.footprint = 0;
            p.pinCount = (uint8_t)v.substring(3).toInt();
        } else if (v.startsWith("axial")) {
            // axial2 only (Kevin's binding geometry: a fixed 2-leg footprint
            // straddling the ravine) - commitPart() rejects any other pin
            // count with a clear message rather than silently truncating it.
            p.footprint = 2;
            p.pinCount = (uint8_t)v.substring(5).toInt();
        } else {
            bad = true;
            err += "unknown footprint '" + v + "'; ";
        }
        inPins = false;
    } else if (key == "row") {
        // through parseScalar like every other scalar: a quoted `row: "5"`
        // must not toInt() to 0 and get the whole part dropped
        p.baseRow = (int16_t)parseScalar(rest).toInt();
        inPins = false;
    } else if (key == "placed") {
        bool ok;
        bool v = parseBoolean(parseScalar(rest), ok);
        if (ok) {
            p.placed = v;
        } else {
            // keep the default (false) but WARN - a silent false would
            // persist through the next auto-save
            err += "part " + String(p.name) + ": unparseable placed '" + rest + "' (kept false); ";
        }
        inPins = false;
    } else if (key == "placement") {
        // Matched half of the serializer's non-default emit. An unknown value
        // falls back to the default AND warns - a silent reinterpretation
        // would be written back on the next save as if the user had asked
        // for it.
        String v = parseScalar(rest);
        v.toLowerCase();
        if (v == "compact")       p.placement = PART_PLACEMENT_COMPACT;
        else if (v == "custom")   p.placement = PART_PLACEMENT_CUSTOM;
        else if (v == "expanded") p.placement = PART_PLACEMENT_EXPANDED;
        else {
            p.placement = PART_PLACEMENT_EXPANDED;
            err += "part " + String(p.name) + ": unknown placement '" + v +
                   "' (kept expanded); ";
        }
        inPins = false;
    } else if (key == "verify") {
        p.defaultVerify = (uint8_t)parseScalar(rest).toInt();
        inPins = false;
    } else if (key == "color") {
        String v = parseScalar(rest);
        if (v.startsWith("0x") || v.startsWith("0X")) {
            p.outlineColor = strtoul(v.c_str() + 2, nullptr, 16);
        } else {
            p.outlineColor = strtoul(v.c_str(), nullptr, 10);
        }
        inPins = false;
    } else if (key == "pins") {
        if (rest.indexOf('{') >= 0) {
            parseInlinePins(p, rest, err);   // inline one-line form
            inPins = false;
        } else {
            inPins = true;                   // nested block form follows
        }
    } else if (inPins && rest.startsWith("{")) {
        parsePinEntry(p, key, rest, err);    // nested pin line: NAME: {...}
    }
    // else: unknown key (e.g. frobnicate: 7) - skipped without error
}

bool deserializeParts(JumperlessState& st, const char* yaml, String& err) {
    st.parts.numParts = 0;   // clear() already ran in fromYAML; be safe
    if (yaml == nullptr) return true;

    const char* pos = yaml;
    bool inSection = false;
    bool inPins = false;
    bool curOpen = false;
    bool curBad = false;
    // ~500 B scratch part; static so OG's small stacks never see it (parsing
    // is single-threaded through fromYAML).
    static PartDefinition cur;

    while (*pos) {
        const char* lineEnd = strchr(pos, '\n');
        size_t rawLen = lineEnd ? (size_t)(lineEnd - pos) : strlen(pos);
        const char* nextPos = lineEnd ? lineEnd + 1 : pos + rawLen;

        // Indentation captured BEFORE trimming - the section header only
        // counts on an un-indented line, and any un-indented line ends it.
        bool indented = (rawLen > 0 && (pos[0] == ' ' || pos[0] == '\t'));

        String line;
        line.reserve(rawLen);
        for (size_t i = 0; i < rawLen; i++) {
            char c = pos[i];
            if (c == '\r') continue;
            line += c;
        }
        line.trim();

        if (!inSection) {
            // Same recognition rule as fromYAML's main loop: un-indented
            // line starting with "parts:".
            if (!indented && line.startsWith("parts:")) inSection = true;
            pos = nextPos;
            continue;
        }

        if (line.length() == 0 || line.startsWith("#")) {
            pos = nextPos;
            continue;
        }
        if (!indented) {
            break;   // next top-level section - parts: is over
        }
        if (line.startsWith("- ")) {
            commitPart(st, cur, curOpen, curBad, err);
            partInit(cur);
            curOpen = true;
            curBad = false;
            inPins = false;
            line = line.substring(2);
            line.trim();
            if (line.length() == 0) {
                pos = nextPos;
                continue;
            }
            // fall through: "- name: ..." carries the first field
        }
        if (curOpen) {
            parsePartLine(cur, line, inPins, curBad, err);
        }
        pos = nextPos;
    }
    commitPart(st, cur, curOpen, curBad, err);
    return true;
}
