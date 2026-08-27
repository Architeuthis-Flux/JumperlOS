// SPDX-License-Identifier: MIT
#ifndef PART_CLASSIFY_H
#define PART_CLASSIFY_H

// Part-identification decision tree (Layer 1 of
// CodeDocs/DESIGN_PART_ID_FOLLOWUP.md), on top of PartMeasure's primitives.
//
// Measurement method informed by the documented approach of the AVR
// TransistorTester (Kübbeler ttester docs); original implementation, MIT.
// Every threshold is either bench-measured on this hardware (2026-08-26,
// 2N3906 on rows 17/18/19) or a device-physics fact (Vf bands, the
// junction-vs-resistor current-ratio law).

#include <stdint.h>

enum class PartType : uint8_t {
    EMPTY = 0, UNKNOWN, SHORT_CIRCUIT, RESISTOR, CAPACITOR,
    DIODE, LED, ZENER, BJT_NPN, BJT_PNP, NFET, PFET,
};

enum class PinRole : uint8_t { NONE = 0, A, K, B, C, E, G, D, S, LEAD };

struct PartResult {
    PartType type = PartType::UNKNOWN;
    float confidence = 0.0f;
    float value = 0.0f;    // R ohms / Vf volts / Vz volts
    float value2 = 0.0f;   // hFE / linearity / reverse-knee volts
    uint8_t rows[3] = { 0, 0, 0 };
    PinRole roles[3] = { PinRole::NONE, PinRole::NONE, PinRole::NONE };
    uint8_t nRows = 0;
    bool degraded = false; // a fallback path was used; trust the type, not the numbers
    int8_t status = 0;     // 0 ok; <0 = partScanBegin refusal (-2 busy,
                           // -3 too wired to briefly unwire, -4 row
                           // powered, -5 no gpio, -7 fabric refused a leg)
    uint8_t lifted = 0;    // user wires briefly removed for the session
                           // (and put back) - say so in any report
    // raw evidence, for HIL assertions and the terminal's long form:
    // 3-lead: jmap = the junction map; 2-lead: screen = vAB,iAB,vBA,iBA
    float jmap[3][3] = { { 0 } };
    float screen[4] = { 0, 0, 0, 0 };
};

PartResult identifyTwoLead(int rowA, int rowB);
PartResult identifyThreeLead(int rowA, int rowB, int rowC);

const char* partTypeName(PartType t);
const char* pinRoleName(PinRole r);
// Honest LED color guess from Vf (overlapping bands are reported as such).
const char* partLedColorGuess(float vf);

#endif // PART_CLASSIFY_H
