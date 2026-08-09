// SPDX-License-Identifier: MIT
//
// PadDecode - calibration-free probe pad decode (probe engine v2).
//
// Replaces the legacy probe_min/probe_max linear map with a physics table:
// each pad's expected ADC5 reading is computed from its real ladder
// resistance (ProbeLadderTable.h, generated from the V5 schematic BOM) and
// a live drive scale, so nothing has to be user-calibrated and nothing
// drifts with USB/rail/DAC voltage:
//
//   expected_i = floor + (scale - floor) * 10K / (10K + R_i + Rsrc)
//
//   scale (SELECT):  4095 * trim. PROBE_PIN's high level and the ADC
//                    reference are the same 3.3V rail, so raw counts are
//                    rail-ratiometric by construction - USB sag cancels.
//   scale (MEASURE): tip counts derived from a fresh ADC7 read (the buffer
//                    output IS the tip) * trim. ADC5 and ADC7 share the
//                    same ADC + reference, so the rail cancels here too;
//                    only the ADC7 front-end divider ratio matters, and
//                    the bounded trim absorbs its tolerance.
//   floor:           continuously tracked dark level (nothing touched),
//                    fed by the 100Hz probe service.
//
// Decode is nearest-band over the expected table (boundaries at midpoints),
// which is exact for the non-linear logo/building pad tail - no more
// "reading < 300" special cases, and BUILDING_PAD_BOTTOM (~40 counts) is
// finally inside the decodable range.
#ifndef PAD_DECODE_H
#define PAD_DECODE_H

#include <Arduino.h>

struct PadDecodeResult {
    int index;      // probeRowMap index 1..101, or -1 for no/invalid touch
    int raw;        // input ADC5 counts
    int expected;   // expected counts for the decoded pad at the live scale
    int distance;   // |raw - expected| in counts
    int band;       // local band width in counts (distance to nearest neighbor boundary * 2)
    bool confident; // raw is well inside the band (feeds the auto-trim)
};

namespace PadDecode {

// (Re)build the expected-ratio table from config (probe_rsrc_ohms). Call at
// startup and whenever probe_rsrc_ohms changes.
void begin();

// Live full-scale counts for the current switch position (select: 4095 *
// trim; measure: ADC7-derived tip counts * trim, cached ~25ms). Returns <= 0
// if the measure tip is unpowered/unreadable this instant.
float scaleCounts(bool forceFresh = false);

// Decode raw ADC5 counts to a probeRowMap index. Returns index -1 when
// below the touch threshold or above full-scale.
PadDecodeResult decode(int adc5counts);

// Dark-floor tracker. Feed every raw service read; readings below the touch
// threshold update the floor estimate.
void trackFloor(int adc5counts);
int floorCounts();
// Minimum counts considered "something touched" (floor + margin).
int touchThresholdCounts();
// One-shot floor seed (boot-time getNothingTouched replacement).
void seedFloor(int counts);

// Feed confident decodes back into the bounded trims (drive scale per
// switch position from shallow pads; shared ladder trim from deep pads,
// with the deep nano tail acting as strong anchors).
void noteConfidentDecode(const PadDecodeResult& r);

// Mark moved trims for the background config save - but only once the
// probe has been quiet for a few seconds, so the flash write can never
// stall active probing. Call from the idle service path.
void persistTrimsIfQuiet();

// Serial diagnostics: floor, scales, trims, and the live expected table.
void printStatus(bool fullTable = false);

} // namespace PadDecode

#endif // PAD_DECODE_H
