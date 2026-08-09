// SPDX-License-Identifier: MIT
// See PadDecode.h for the model. Everything here is in raw ADC counts of
// the shared RP2350 ADC, so reference/rail variation cancels wherever both
// sides of a ratio come from the same converter.

#include "PadDecode.h"

#include "../Peripherals.h"   // readAdc
#include "../Probing.h"       // switchPosition alias, OG guard context
#include "../config.h"        // jumperlessConfig
#include "../configManager.h" // configChanged

int probeGpioPowerClaimIdx(void); // Probing.cpp (GPIO buffer-power claim)

namespace PadDecode {

static const float kRsense = 10000.0f; // sense-node pull to GND
static const float kFullScale = 4095.0f;

#include "ProbeLadderTable.h" // kProbeLadderOhms[PROBE_LADDER_ENTRIES]

// Expected divider ratio per pad index (unit drive), built by begin().
static float s_ratio[PROBE_LADDER_ENTRIES];
static bool s_begun = false;
static float s_builtRsrc = -1.0f;
static float s_builtLadderTrim = -1.0f;

// ---- dark floor tracker ----------------------------------------------------
// The sense node reads a small nonzero level with nothing touched (ADC
// offset + leakage through the ladder). Track it continuously instead of
// the legacy one-shot getNothingTouched(): every service read below the
// touch threshold nudges the estimate, so slow offset drift can never
// accumulate into a decode error.
static float s_floor = 12.0f;
static bool s_floorSeeded = false;
// Counts above the floor that mean "something is touching". Must stay well
// under BUILDING_PAD_BOTTOM's ~40 counts (1M ohm ladder tail) or that pad
// becomes undecodable - which is exactly what the legacy fixed
// minimum_probe_reading = 85 gate did to it.
static const int kTouchMargin = 14;

void seedFloor(int counts) {
    if (counts >= 0 && counts < 200) {
        s_floor = (float)counts;
        s_floorSeeded = true;
    }
}

// ---- measure-frame anchor (see measureTipCounts) --------------------------
// ADC7 counts (zero-corrected) of the UNLOADED tip in measure position with
// the GPIO-powered buffer. At that moment the tip sits at the 3.3V rail
// (GPIO driven high, no ladder load), which is by definition full-scale in
// the ADC5 frame - both ADCs share the rail reference. Anchoring the
// cross-channel ratio here makes the measure decode frame calibration-free:
// the ADC7 front-end divider tolerance (which shifted measure decode a full
// band on some boards through the volts-frame conversion) cancels in the
// ratio, and ADC zero/nonlinearity errors mostly cancel too because the
// anchor sits at the operating voltage.
static float s_c7Unloaded = -1.0f;
static unsigned long s_c7UnloadedAt = 0;

static inline float zero7Counts() {
    float spread = jumperlessConfig.calibration.adc_7_spread;
    if (spread < 1.0f)
        return 2016.0f; // nominal 9.0V / 18.28V full-scale
    return jumperlessConfig.calibration.adc_7_zero / spread * kFullScale;
}

void trackFloor(int adc5counts) {
    if (adc5counts < 0)
        return;
    if (!s_floorSeeded && adc5counts < 200) {
        s_floor = (float)adc5counts;
        s_floorSeeded = true;
        return;
    }
    if (adc5counts <= (int)(s_floor + 0.5f) + kTouchMargin) {
        // Slow EMA; ~100Hz feed means the floor settles in about a second
        // and a genuine touch (rejected by the gate above) can't drag it.
        s_floor += ((float)adc5counts - s_floor) * 0.01f;
        if (s_floor < 0.0f)
            s_floor = 0.0f;
        if (s_floor > 120.0f)
            s_floor = 120.0f; // runaway guard: something is wrong, not "floor"

#if !defined(OG_JUMPERLESS)
        // Nothing is touching, so if we're in measure position with the
        // GPIO-powered buffer, the tip is unloaded right now - sample the
        // measure-frame anchor (throttled; one short ADC7 read).
        if (switchPosition == 0 && probeGpioPowerClaimIdx() >= 0 &&
            millis() - s_c7UnloadedAt > 250) {
            s_c7UnloadedAt = millis();
            float c7p = (float)readAdc(7, 8) - zero7Counts();
            if (c7p > 100.0f) { // buffer actually powered / sane
                s_c7Unloaded = (s_c7Unloaded < 0.0f)
                                   ? c7p
                                   : s_c7Unloaded + (c7p - s_c7Unloaded) * 0.1f;
            }
        }
#endif
    }
}

int floorCounts() { return (int)(s_floor + 0.5f); }
int touchThresholdCounts() { return (int)(s_floor + 0.5f) + kTouchMargin; }

// last time decode() saw a touching-level reading (quiet-persist gating)
static unsigned long s_lastTouchMs = 0;
// auto-trim moved a value and it hasn't been persisted yet
static bool s_trimDirty = false;

// ---- scale ------------------------------------------------------------------

static inline float clampTrim(float t) {
    if (t < 0.95f)
        return 0.95f;
    if (t > 1.05f)
        return 1.05f;
    return t;
}

// MEASURE-position tip counts in the ADC5 frame, from a fresh ADC7 read.
//
// Preferred path: the unloaded-tip anchor (s_c7Unloaded, tracked while
// nothing is touched). Unloaded tip = rail = 4095 in the ADC5 frame, so
//   S = 4095 * (c7_loaded' / c7_unloaded')
// with all cross-channel gain (divider tolerance, per-board ADC spread)
// cancelling in the ratio - no calibrated constants at all. The live read
// still captures load droop correctly, since only the unloaded level is
// anchored.
//
// Fallback (DAC-powered buffer, or no anchor sampled yet since boot): the
// volts-frame conversion through adc_7_zero/spread and the nominal 3.3,
// with residual board tolerance absorbed by probe_scale_trim_measure.
static float measureTipCounts() {
    int counts7 = readAdc(7, 16);
    float c7p = (float)counts7 - zero7Counts();

    if (s_c7Unloaded > 100.0f && probeGpioPowerClaimIdx() >= 0) {
        if (c7p < 50.0f)
            return -1.0f; // buffer unpowered / mid-glitch
        return kFullScale * (c7p / s_c7Unloaded);
    }

    float spread = jumperlessConfig.calibration.adc_7_spread; // volts full-scale
    float zero = jumperlessConfig.calibration.adc_7_zero;     // volts at count 0
    if (spread < 1.0f)
        return -1.0f;
    float vTip = ((float)counts7 * spread / kFullScale) - zero;
    if (vTip < 0.5f || vTip > 5.5f)
        return -1.0f; // buffer unpowered / mid-glitch: no valid scale
    return (vTip / 3.3f) * kFullScale;
}

float scaleCounts(bool forceFresh) {
#if defined(OG_JUMPERLESS)
    return kFullScale * clampTrim(jumperlessConfig.calibration.probe_scale_trim);
#else
    if (switchPosition != 0) { // select (or unknown): rail-ratiometric
        return kFullScale * clampTrim(jumperlessConfig.calibration.probe_scale_trim);
    }
    // Measure: the tip voltage moves with load (ladder + LED), so the scale
    // must be read nearly simultaneously with the pad burst. Cache for 25ms
    // so one decode pass shares a single ADC7 read.
    static float cached = -1.0f;
    static unsigned long lastRead = 0;
    static int lastSwitch = -2;
    if (forceFresh || lastSwitch != switchPosition || lastRead == 0 ||
        millis() - lastRead > 25) {
        lastRead = millis();
        lastSwitch = switchPosition;
        float tip = measureTipCounts();
        cached = (tip > 0.0f)
                     ? tip * clampTrim(jumperlessConfig.calibration.probe_scale_trim_measure)
                     : -1.0f;
    }
    return cached;
#endif
}

// ---- table ------------------------------------------------------------------

void begin() {
    float rsrc = jumperlessConfig.calibration.probe_rsrc_ohms;
    // Real ladders run a couple percent more resistive than the schematic
    // nominals relative to the 10K sense divider; because R/deltaR is
    // near-constant by design, that shifts every mid-chain pad by the same
    // fraction of a band, so one multiplicative trim corrects the whole
    // curve (see config.h probe_ladder_trim).
    float ladderTrim = jumperlessConfig.calibration.probe_ladder_trim;
    s_ratio[0] = -1.0f;
    for (int i = 1; i < PROBE_LADDER_ENTRIES; i++) {
        s_ratio[i] = kRsense / (kRsense + kProbeLadderOhms[i] * ladderTrim + rsrc);
    }
    s_builtRsrc = rsrc;
    s_builtLadderTrim = ladderTrim;
    s_begun = true;
}

static inline void ensureBegun() {
    if (!s_begun || s_builtRsrc != jumperlessConfig.calibration.probe_rsrc_ohms ||
        s_builtLadderTrim != jumperlessConfig.calibration.probe_ladder_trim) {
        begin();
    }
}

static inline int expectedCounts(int index, float scale) {
    return (int)(s_floor + (scale - s_floor) * s_ratio[index] + 0.5f);
}

// ---- decode -----------------------------------------------------------------

PadDecodeResult decode(int adc5counts) {
    PadDecodeResult res;
    res.index = -1;
    res.raw = adc5counts;
    res.expected = 0;
    res.distance = 0;
    res.band = 0;
    res.confident = false;

    ensureBegun();

    if (adc5counts < touchThresholdCounts())
        return res; // nothing touched

    float scale = scaleCounts();
    if (scale <= 0.0f)
        return res; // measure tip unpowered: no valid frame to decode in

    // Expected counts DESCEND with index. Binary search for the first index
    // whose expected value is <= raw, then pick raw's nearest neighbor.
    // (101 entries; ponytail: linear scan would also be fine, but the
    // search costs nothing and keeps this hot path constant-time.)
    int lo = 1, hi = PROBE_LADDER_ENTRIES - 1;
    int e1 = expectedCounts(1, scale);
    if (adc5counts > e1 + (e1 - expectedCounts(2, scale))) {
        return res; // above row 1 by more than a band: not a pad
    }
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (expectedCounts(mid, scale) > adc5counts)
            lo = mid + 1;
        else
            hi = mid;
    }
    // lo = first index with expected <= raw; candidate is lo or lo-1
    int best = lo;
    if (lo > 1) {
        int dHi = abs(expectedCounts(lo - 1, scale) - adc5counts);
        int dLo = abs(expectedCounts(lo, scale) - adc5counts);
        if (dHi < dLo)
            best = lo - 1;
    }

    int exp = expectedCounts(best, scale);
    int nUp = (best > 1) ? expectedCounts(best - 1, scale) : exp + 80;
    int nDn = (best < PROBE_LADDER_ENTRIES - 1) ? expectedCounts(best + 1, scale)
                                                : exp - 40;
    int band = min(nUp - exp, exp - nDn); // half-distance to nearest neighbor x2
    if (band < 2)
        band = 2;

    res.index = best;
    res.expected = exp;
    res.distance = abs(adc5counts - exp);
    res.band = band;

    s_lastTouchMs = millis(); // something IS touching (for quiet-persist)

    // Boundary deadzone: a reading close to the band boundary (which sits
    // at 50%) is ambiguous - without this it flickers between the two
    // neighbors, and the consecutive-read confirmation upstream then makes
    // the touch feel dead/laggy. Better to report no decode and let the
    // next (settled) burst land it.
    //
    // Tapered by depth: tip contact resistance (pressure-dependent,
    // 10-50 ohm) moves EARLY pads by a much larger fraction of their band
    // (~0.4 counts/ohm at row 1 vs ~0.14 at row 60, against a ~100 ohm
    // ladder pitch up top), so a wide deadzone there rejects real light
    // touches. Early pads keep almost the whole band; deeper pads keep the
    // stricter gate.
    int deadPermil = (best <= 34) ? 465 : 420; // accept up to X/1000 of band
    if (res.distance * 1000 > band * deadPermil) {
        res.index = -1;
        return res;
    }

    // Inside 60% of the half-band: unambiguous. Feed the trim only from
    // clearly-decoded touches so a boundary reading can't walk the scale.
    res.confident = (res.distance * 10 < band * 6);
    return res;
}

// ---- residual auto-trim ------------------------------------------------------

// Nudge the model toward the value that would center the observed reading
// on its decoded pad. Only confident decodes feed it, the gain is tiny
// (2%), and the results are clamped - so it can absorb slow real drift
// (temperature, resistor aging, divider tolerance) but a burst of bad
// readings can't run it away. Persisted through the normal config save
// path, rate-limited to one mark per 30s.
//
// Two knobs, separated by ladder depth:
//  - shallow pads (low R): counts are dominated by the drive SCALE, so
//    residuals there update the per-switch-position scale trim.
//  - deep pads (high R): counts are dominated by ladder RESISTANCE, so
//    residuals there update the shared ladder trim.
//
// Board-to-board tolerance recovery: the default probe_ladder_trim is a
// fleet estimate; a board whose resistor batch sits ~a band away decodes
// mid-chain rows consistently one off, and those WRONG decodes look
// confident (centered in the neighboring band), so ordinary weak-gain
// updates can't escape the lock-in. The deep nano tail (indices 78..94,
// 33K-117K) is the way out: its resistance steps are 10-12% of the total,
// so even a several-percent-off board decodes those pads unambiguously -
// they act as strong anchors that pull the ladder trim onto the board's
// true value within a few samples (one deliberate 2-3s touch of a deep
// nano pad, e.g. 5V/GND/VIN, re-anchors a mis-trimmed board). SELECT
// position only, so the measure scale trim can't contaminate the anchor.
// The SF/logo tail (95..101) stays excluded: those pads sit behind their
// own big star resistors, whose tolerance is independent of the chain's.
void noteConfidentDecode(const PadDecodeResult& r) {
    if (!r.confident || r.index < 1 || r.index > 94)
        return;
    ensureBegun();
    float scale = scaleCounts();
    if (scale <= 0.0f || s_ratio[r.index] <= 0.0f)
        return;

    if (r.index <= 20) {
        // Scale implied by this reading landing exactly on its pad.
        float implied = s_floor + ((float)r.raw - s_floor) / s_ratio[r.index];
        float correction = implied / scale; // ~1.0 when perfectly on target
        if (correction < 0.9f || correction > 1.1f)
            return; // implausible: reject rather than average in
        bool measure = (switchPosition == 0);
        float& trim = measure ? jumperlessConfig.calibration.probe_scale_trim_measure
                              : jumperlessConfig.calibration.probe_scale_trim;
        float updated = clampTrim(trim * (1.0f + (correction - 1.0f) * 0.02f));
        if (updated != trim) {
            trim = updated;
            s_trimDirty = true; // persisted later, when the probe goes quiet
        }
    } else if (r.index >= 30) {
        // Effective ladder resistance implied by this reading.
        float rNorm = ((float)r.raw - s_floor) / (scale - s_floor); // divider ratio
        if (rNorm <= 0.01f || rNorm >= 0.99f)
            return;
        float rImplied = kRsense / rNorm - kRsense -
                         jumperlessConfig.calibration.probe_rsrc_ohms;
        float rModel = kProbeLadderOhms[r.index] *
                       jumperlessConfig.calibration.probe_ladder_trim;
        if (rModel <= 0.0f)
            return;
        float correction = rImplied / rModel;

        // Deep-nano anchor zone: unambiguous decode, so accept a wider
        // correction and move fast (see banner above). Elsewhere the decode
        // could itself be one band off, so creep slowly.
        bool anchor = (r.index >= 78) && (switchPosition != 0);
        float lo = anchor ? 0.88f : 0.9f;
        float hi = anchor ? 1.12f : 1.1f;
        float gain = anchor ? 0.10f : 0.02f;
        if (correction < lo || correction > hi)
            return;
        float trim = jumperlessConfig.calibration.probe_ladder_trim *
                     (1.0f + (correction - 1.0f) * gain);
        if (trim < 0.95f)
            trim = 0.95f;
        if (trim > 1.10f)
            trim = 1.10f;
        if (trim != jumperlessConfig.calibration.probe_ladder_trim) {
            jumperlessConfig.calibration.probe_ladder_trim = trim;
            s_trimDirty = true; // persisted later, when the probe goes quiet
        }
    }
}

// Persist auto-trim movement ONLY when the probe has been untouched for a
// few seconds and no session is active. Marking configChanged mid-use let
// the background config save (a ~100ms+ flash write) fire in the middle of
// probing - the source of "sometimes probing hangs for a beat". Called
// from the Pads idle service path.
void persistTrimsIfQuiet() {
    static unsigned long lastMark = 0;
    if (!s_trimDirty || probeActive)
        return;
    if (millis() - s_lastTouchMs < 3000)
        return; // still being used
    if (millis() - lastMark < 60000)
        return; // at most one save marker a minute
    lastMark = millis();
    s_trimDirty = false;
    configChanged = true; // background config save picks it up
}

// ---- diagnostics -------------------------------------------------------------

void printStatus(bool fullTable) {
    ensureBegun();
    float sSel = kFullScale * clampTrim(jumperlessConfig.calibration.probe_scale_trim);
    Serial.printf("[padDecode] floor %d (thresh %d)  Rsrc %.1f ohm  ladder trim %.4f\n\r",
                  floorCounts(), touchThresholdCounts(),
                  (double)jumperlessConfig.calibration.probe_rsrc_ohms,
                  (double)jumperlessConfig.calibration.probe_ladder_trim);
    Serial.printf("            select scale %.0f (trim %.4f)\n\r", (double)sSel,
                  (double)jumperlessConfig.calibration.probe_scale_trim);
#if !defined(OG_JUMPERLESS)
    float tip = measureTipCounts();
    Serial.printf("            measure tip %.0f counts (trim %.4f, anchor %s%.0f)%s\n\r",
                  (double)tip,
                  (double)jumperlessConfig.calibration.probe_scale_trim_measure,
                  s_c7Unloaded > 0.0f ? "" : "none ", (double)s_c7Unloaded,
                  tip <= 0.0f ? "  [buffer unpowered]" : "");
#endif
    if (fullTable) {
        float scale = scaleCounts(true);
        if (scale <= 0.0f)
            scale = sSel;
        Serial.println("            idx  R(ohm)   expected");
        for (int i = 1; i < PROBE_LADDER_ENTRIES; i++) {
            Serial.printf("            %3d  %9.0f  %4d\n\r", i,
                          (double)kProbeLadderOhms[i], expectedCounts(i, scale));
        }
    }
    Serial.flush();
}

} // namespace PadDecode
