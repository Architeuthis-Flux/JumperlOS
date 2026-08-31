// SPDX-License-Identifier: MIT
// Part-identification decision tree. See PartClassify.h for provenance.
//
// Junction-map bands (bench, 2N3906 through the real fabric, ADC legs
// attached): forward Si junction 0.55-0.75V, blocked ~3.2V, floating row
// ~2.3V (the ADC input-network bias), BJT E-C floating-base leak 2.6-2.9V.
// The classifier only ever *decides* on the wide bands; the numbers it
// *reports* come from the Kelvin servo, never from the map.

#include "PartClassify.h"
#include "PartMeasure.h"

#include <Arduino.h>

// junction-map decision bands (volts at the pulled-up candidate anode)
static const float KMAP_FWD_MAX = 1.10f;   // below = forward junction
static const float KMAP_BLOCKED_MIN = 3.05f;  // above = no conduction

const char* partTypeName(PartType t) {
    switch (t) {
        case PartType::EMPTY: return "EMPTY";
        case PartType::SHORT_CIRCUIT: return "SHORT";
        case PartType::RESISTOR: return "RESISTOR";
        case PartType::CAPACITOR: return "CAPACITOR";
        case PartType::DIODE: return "DIODE";
        case PartType::LED: return "LED";
        case PartType::ZENER: return "ZENER";
        case PartType::BJT_NPN: return "BJT_NPN";
        case PartType::BJT_PNP: return "BJT_PNP";
        case PartType::NFET: return "NFET";
        case PartType::PFET: return "PFET";
        case PartType::POT: return "POT";
        default: return "UNKNOWN";
    }
}

const char* pinRoleName(PinRole r) {
    switch (r) {
        case PinRole::A: return "A";
        case PinRole::K: return "K";
        case PinRole::B: return "B";
        case PinRole::C: return "C";
        case PinRole::E: return "E";
        case PinRole::G: return "G";
        case PinRole::D: return "D";
        case PinRole::S: return "S";
        case PinRole::W: return "WIPER";
        case PinRole::LEAD: return "LEAD";
        default: return "-";
    }
}

const char* partLedColorGuess(float vf) {
    if (vf < 1.9f) return "infrared";
    if (vf < 2.1f) return "red";
    if (vf < 2.25f) return "orange/yellow";
    if (vf < 2.5f) return "green";           // classic GaP green
    if (vf < 2.9f) return "red-orange or blue/green";  // honest: bands overlap
    if (vf < 3.6f) return "blue/white/green";          // InGaN family
    return "violet/UV";
}

// ---------------------------------------------------------------------------
// two-lead
// ---------------------------------------------------------------------------

PartResult identifyTwoLead(int rowA, int rowB) {
    PartResult r;
    r.nRows = 2;
    r.rows[0] = (uint8_t)rowA;
    r.rows[1] = (uint8_t)rowB;
    r.roles[0] = r.roles[1] = PinRole::LEAD;

    static ScanSession s;
    int rows[2] = { rowA, rowB };
    int rc = partScanBegin(s, rows, 2);
    if (rc < 0) {
        r.status = (int8_t)rc;
        return r;
    }
    r.lifted = s.nLift;

    // conduction screen, both directions, at a gentle 1mA / 5.5V ceiling
    float vAB = 0, iAB = 0, vBA = 0, iBA = 0;
    bool fwdAB = partScanServo(s, 0, 1, 1.0f, 5.5f, &vAB, &iAB);
    bool fwdBA = partScanServo(s, 1, 0, 1.0f, 5.5f, &vBA, &iBA);
    r.screen[0] = vAB; r.screen[1] = iAB;
    r.screen[2] = vBA; r.screen[3] = iBA;

    if (fwdAB && fwdBA) {
        // conducts both ways: resistor family (or antiparallel junctions)
        float ohms = 0, lin = 0;
        if (partScanResistance(s, 0, 1, &ohms, &lin)) {
            float rAB = (iAB > 0.01f) ? vAB / (iAB * 0.001f) : 0;
            float rBA = (iBA > 0.01f) ? vBA / (iBA * 0.001f) : 0;
            float sym = (rAB > 1.0f && rBA > 1.0f)
                            ? ((rAB > rBA) ? rAB / rBA : rBA / rAB) : 99.0f;
            bool linear = (lin > 0.75f && lin < 1.30f) || lin == 1.0f;
            if (ohms < 3.0f) {
                r.type = PartType::SHORT_CIRCUIT;
                r.value = ohms;
                r.confidence = 0.9f;
            } else if (sym < 1.15f && linear) {
                r.type = PartType::RESISTOR;
                r.value = ohms;
                r.value2 = lin;
                r.confidence = 0.9f;
            } else if (vAB > 0.35f && vAB < 1.1f && vBA > 0.35f && vBA < 1.1f) {
                // both directions clamp at a junction voltage: antiparallel
                // diodes (or a transistor's two legs) - report, don't guess
                r.type = PartType::UNKNOWN;
                r.value = vAB;
                r.value2 = vBA;
                r.degraded = true;
            } else {
                r.type = PartType::RESISTOR;
                r.value = ohms;
                r.value2 = lin;
                r.confidence = 0.5f;
                r.degraded = true;
            }
        }
        partScanEnd(s);
        return r;
    }

    if (fwdAB || fwdBA) {
        // one direction: diode family. Anode = the driven-positive row.
        int a = fwdAB ? 0 : 1;
        int k = fwdAB ? 1 : 0;
        r.roles[a] = PinRole::A;
        r.roles[k] = PinRole::K;
        float vfHi = fwdAB ? vAB : vBA;

        // low-current Vf for the junction-vs-resistor ratio law: a diode's
        // Vf barely moves over a 20x current ratio, a resistor's V scales.
        // The low point has to be a real MEASUREMENT to say anything: a
        // 50uA target lives inside this fabric's transient floor, where
        // fixture-build charge draining through the shunt trips the servo
        // at the first DAC step (bench, 2026-08-28 - the same finding that
        // dropped the two-current law from clampProbeDir). A phantom trip
        // reports a tiny vfLo, the ratio law then reads "scales like R",
        // and an unpowered chip's junction chain came out of the Auto scan
        // as "RESISTOR 2" between two of its own pins. Below a junction's
        // own floor the low point proves nothing, so the conservative
        // verdict stands: junction-like.
        float vfLo = 0, iLo = 0;
        bool gotLo = partScanServo(s, a, k, 0.05f, 5.5f, &vfLo, &iLo);
        bool junctionLike = true;
        if (gotLo && vfLo > 0.25f) {
            if (!(vfHi > 1.05f * vfLo)) junctionLike = true;  // very flat: fine
            if (vfHi > 8.0f * vfLo) junctionLike = false;     // scales like R
        }

        // capacitor fake-out: re-read at the same point, a cap keeps rising
        float vf2 = 0, i2 = 0;
        partScanServo(s, a, k, 1.0f, 5.5f, &vf2, &i2);
        if (vf2 > vfHi + 0.10f) {
            // it IS a capacitor (big enough to trip the 1mA screen while
            // charging) - so measure it. This branch used to return
            // conf=0.00 with no value (bench, 2026-08-28: C12 read
            // "CAPACITOR conf=0.00" and the parts list said "capacitor",
            // nothing else).
            r.type = PartType::CAPACITOR;
            r.roles[a] = r.roles[k] = PinRole::LEAD;  // the A/K vote was a
                                                      // charge transient,
                                                      // not polarity
            float farads = 0.0f, tauMs = 0.0f;
            partScanCapMeasure(s, a, k, &farads, &tauMs);
            r.value = farads;
            r.value2 = tauMs;
            if (farads > 0.0f) r.confidence = 0.7f;
            else r.degraded = true;   // present, but the value was refused
            partScanEnd(s);
            return r;
        }
        float vf = vf2;  // the settled 1mA reading

        if (vf < 0.25f) {
            // "Conducts" one way but drops under clampProbeDir's own tie
            // threshold - no junction passes 1mA under 0.25V, and a
            // NEGATIVE drop is a charged capacitor SOURCING current
            // (bench, 2026-08-29: the scan's census poke and pair sweep
            // left the 260uF C12 charged, and this branch called it
            // "DIODE -0.41V"). Ask the cap machinery - its stage 2 only
            // says yes when the current actually DECAYS, so a low-drop
            // schottky lands in the honest UNKNOWN below instead.
            float farads = 0.0f, tauMs = 0.0f;
            if (partScanCapMeasure(s, a, k, &farads, &tauMs)) {
                r.type = PartType::CAPACITOR;
                r.roles[a] = r.roles[k] = PinRole::LEAD;   // A/K was the charge talking
                r.value = farads;
                r.value2 = tauMs;
                r.confidence = (farads > 0.0f) ? 0.7f : 0.4f;
                r.degraded = (farads <= 0.0f);
                partScanEnd(s);
                return r;
            }
            r.type = PartType::UNKNOWN;
            r.value = vf;
            r.degraded = true;
            partScanEnd(s);
            return r;
        }

        if (!junctionLike) {
            // Resistive, but only ONE way - which no two-terminal resistor
            // can be, so this is a series junction (a diode-plus-resistor
            // leg, a chip's internals) and RESISTOR.value would be a
            // fiction. Report the drop and let the caller stay honest.
            // (The old verdict also divided by 1000 on a field every other
            // producer and formatOhms fill in OHMS, so a 1.7V-at-1mA chain
            // printed as "2".)
            r.type = PartType::UNKNOWN;
            r.value = vf;                      // the drop at 1mA
            r.confidence = 0.4f;
            r.degraded = true;
            partScanEnd(s);
            return r;
        }

        // reverse scan for a Zener knee (the servo's false result at vMax)
        float vRev = 0, iRev = 0;
        bool revConducts = partScanServo(s, k, a, 1.0f, 6.0f, &vRev, &iRev);
        if (revConducts && vRev > 1.0f) {
            r.type = PartType::ZENER;
            r.value = vf;
            r.value2 = vRev;     // the reverse knee
            r.confidence = 0.85f;
            partScanEnd(s);
            return r;
        }

        if (vf >= 1.4f) {
            r.type = PartType::LED;
            r.value = vf;
            r.confidence = 0.9f;
            // light it up so the answer is visible on the board itself
            float vLit = 0, iLit = 0;
            partScanServo(s, a, k, 5.0f, 6.0f, &vLit, &iLit);
            if (vLit > 0.5f) r.value = vLit;  // Vf at 5mA is the honest spec point
        } else {
            r.type = PartType::DIODE;
            r.value = vf;
            r.confidence = 0.9f;
        }
        partScanEnd(s);
        return r;
    }

    // Neither direction conducts: capacitor, or nothing. The measurement
    // doubles as the detector and reaches far lower than the old INA
    // decay watch ever did (a 100nF reads EMPTY to a 0.10mA-at-25ms
    // screen; the pull-down decay times it cleanly).
    float farads = 0.0f, tauMs = 0.0f;
    if (partScanCapMeasure(s, 0, 1, &farads, &tauMs)) {
        r.type = PartType::CAPACITOR;
        r.value = farads;              // farads - 0 = detect-only
        r.value2 = tauMs;
        r.confidence = (farads > 0.0f) ? 0.8f : 0.5f;
        r.degraded = (farads <= 0.0f);
        partScanEnd(s);
        return r;
    }
    r.type = PartType::EMPTY;
    r.confidence = 0.8f;
    partScanEnd(s);
    return r;
}

// ---------------------------------------------------------------------------
// three-lead
// ---------------------------------------------------------------------------

PartResult identifyThreeLead(int rowA, int rowB, int rowC) {
    PartResult r;
    r.nRows = 3;
    r.rows[0] = (uint8_t)rowA;
    r.rows[1] = (uint8_t)rowB;
    r.rows[2] = (uint8_t)rowC;
    for (int i = 0; i < 3; i++) r.roles[i] = PinRole::LEAD;

    static ScanSession s;
    int rows[3] = { rowA, rowB, rowC };
    int rc = partScanBegin(s, rows, 3);
    if (rc < 0) {
        r.status = (int8_t)rc;
        return r;
    }
    r.lifted = s.nLift;

    float v[3][3];
    partScanJunctionMap(s, v);
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) r.jmap[a][b] = v[a][b];

    // classify the six ordered pairs
    bool fwd[3][3] = { { false } };
    int nFwd = 0;
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
            if (a != b && v[a][b] < KMAP_FWD_MAX) {
                fwd[a][b] = true;
                nFwd++;
            }

    if (nFwd == 6) {
        // Every ordered pair conducts: a resistor mesh, i.e. a pot (its two
        // half-tracks put resistance between every pin pair). Guard first:
        // a real resistor mesh conducts SYMMETRICALLY (v[a][b] ~ v[b][a]);
        // three pins of an IC can also pairwise-conduct through its innards
        // but asymmetrically - and the Kelvin sweeps below cost ~15s, which
        // an Auto-scan span must never spend on a chip (bench: the 7400's
        // pin trios ground the whole scan otherwise).
        bool symmetric = true;
        for (int a = 0; a < 3 && symmetric; a++)
            for (int b = a + 1; b < 3; b++)
                if (fabsf(v[a][b] - v[b][a]) > 0.40f) { symmetric = false; break; }
        if (!symmetric) {
            r.type = PartType::UNKNOWN;
            r.degraded = true;
            partScanEnd(s);
            return r;
        }
        // Kelvin-measure the three pairs; the wiper is the pin whose two
        // halves sum to the end-to-end track: R(a,c) ~= R(a,w) + R(w,c).
        float rp[3];  // rp[0]=R(0,1), rp[1]=R(0,2), rp[2]=R(1,2)
        float lin;
        bool ok = partScanResistance(s, 0, 1, &rp[0], &lin) &&
                  partScanResistance(s, 0, 2, &rp[1], &lin) &&
                  partScanResistance(s, 1, 2, &rp[2], &lin);
        if (ok) {
            // candidate wiper w: the pair NOT containing w is the track
            const int trackOf[3] = { 2, 1, 0 };  // w=0 -> R(1,2), w=1 -> R(0,2), w=2 -> R(0,1)
            const int haloA[3] = { 0, 0, 1 };    // the two half-track pairs per w
            const int haloB[3] = { 1, 2, 2 };
            int wiper = -1;
            for (int w = 0; w < 3; w++) {
                float track = rp[trackOf[w]];
                float sum = rp[haloA[w]] + rp[haloB[w]];
                if (track > 10.0f && sum > 0.0f &&
                    sum > 0.75f * track && sum < 1.25f * track) {
                    wiper = w;
                    break;
                }
            }
            if (wiper >= 0) {
                r.type = PartType::POT;
                r.roles[wiper] = PinRole::W;
                bool first = true;
                for (int i = 0; i < 3; i++) {
                    if (i == wiper) continue;
                    r.roles[i] = first ? PinRole::A : PinRole::B;
                    first = false;
                }
                r.value = rp[trackOf[wiper]];         // end-to-end track ohms
                r.value2 = rp[haloA[wiper]] / rp[trackOf[wiper]];  // wiper position 0..1
                r.confidence = 0.85f;
                partScanEnd(s);
                return r;
            }
        }
        r.type = PartType::UNKNOWN;
        r.degraded = true;
        partScanEnd(s);
        return r;
    }

    if (nFwd == 0) {
        // No junction anywhere. All six pairs cleanly blocked = nothing
        // conducting between any pins - empty rows or a fully open part
        // (bench: the float-bias diagonal can't tell them apart; a
        // discharged row recovers toward the lane bias far too slowly).
        bool allBlocked = true;
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                if (a != b && v[a][b] < KMAP_BLOCKED_MIN) allBlocked = false;
        r.type = allBlocked ? PartType::EMPTY : PartType::UNKNOWN;
        r.confidence = allBlocked ? 0.6f : 0.2f;
        partScanEnd(s);
        return r;
    }

    // BJT: exactly two forward junctions sharing a common cathode (PNP,
    // the base) or a common anode (NPN)
    int base = -1;
    bool pnp = false;
    if (nFwd == 2) {
        for (int x = 0; x < 3; x++) {
            int o1 = (x + 1) % 3, o2 = (x + 2) % 3;
            if (fwd[o1][x] && fwd[o2][x]) { base = x; pnp = true; }
            if (fwd[x][o1] && fwd[x][o2]) { base = x; pnp = false; }
        }
    }

    if (base >= 0) {
        int p1 = (base + 1) % 3, p2 = (base + 2) % 3;
        // sanity: the two junctions must block in reverse
        bool revOk = pnp ? (v[base][p1] > KMAP_FWD_MAX && v[base][p2] > KMAP_FWD_MAX)
                         : (v[p1][base] > KMAP_FWD_MAX && v[p2][base] > KMAP_FWD_MAX);
        // E/C by gain asymmetry: run both orientations, the real one wins.
        // A weak vote (under 2x either way) gets one full re-run and the
        // sums decide - the true asymmetry is ~20x, so anything near 1x is
        // a measurement wobble, not the part.
        float i1 = 0, vb1 = 0, vbe1 = 0, i2 = 0, vb2 = 0, vbe2 = 0;
        partScanHfe(s, p1, base, p2, pnp, &i1, &vb1, &vbe1);
        partScanHfe(s, p2, base, p1, pnp, &i2, &vb2, &vbe2);
        if (i1 < 2.0f * i2 && i2 < 2.0f * i1) {
            float j1 = 0, j2 = 0, vbx, vbex;
            partScanHfe(s, p1, base, p2, pnp, &j1, &vbx, &vbex);
            partScanHfe(s, p2, base, p1, pnp, &j2, &vbx, &vbex);
            i1 += j1;
            i2 += j2;
        }
        bool firstWins = i1 >= i2;
        int e = firstWins ? p1 : p2;
        int c = firstWins ? p2 : p1;
        float icWin = firstWins ? i1 : i2;
        float vbWin = firstWins ? vb1 : vb2;
        float vbeWin = firstWins ? vbe1 : vbe2;
        float ratio = (firstWins ? ((i2 > 0.01f) ? i1 / i2 : 99.0f)
                                 : ((i1 > 0.01f) ? i2 / i1 : 99.0f));

        r.type = pnp ? PartType::BJT_PNP : PartType::BJT_NPN;
        r.roles[base] = PinRole::B;
        r.roles[e] = PinRole::E;
        r.roles[c] = PinRole::C;
        // Vbe from the junction map: the true ~50uA junction voltage, free
        // of the hFE fixture's drive-path drop
        r.value = pnp ? v[e][base] : v[base][e];
        (void)vbeWin;
        // hFE = Ic/Ib with Ib from the calibrated pull on the base pin
        float pullOhms = partScanCalibratePull(s, base, !pnp);
        if (pullOhms > 1000.0f && icWin > 0.05f) {
            float ib_mA = pnp ? (vbWin / pullOhms) * 1000.0f
                              : ((3.3f - vbWin) / pullOhms) * 1000.0f;
            if (ib_mA > 0.001f) {
                float hfe = icWin / ib_mA;
                if (!pnp && hfe > 1.0f) hfe -= 1.0f;  // shunt reads Ie there
                r.value2 = hfe;
            }
        } else {
            r.degraded = true;            // type is solid, the number isn't
        }
        r.confidence = (revOk && ratio > 2.0f) ? 0.9f : 0.6f;
        if (ratio <= 2.0f) r.degraded = true;  // E/C ambiguous (or symmetric part)
        partScanEnd(s);
        return r;
    }

    // FET: one body-diode pair, gate shows no DC conduction to either pin
    if (nFwd == 1) {
        int da = -1, dk = -1;
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                if (fwd[a][b]) { da = a; dk = b; }
        int g = 3 - da - dk;
        // gate must be junction-free against both other pins
        bool gateClean = !fwd[g][da] && !fwd[g][dk] && !fwd[da][g] && !fwd[dk][g];
        if (gateClean) {
            // does the gate modulate the channel? drive current through the
            // body-diode's BLOCKED direction (DAC on dk, shunt on da) with
            // the gate hard high vs hard low
            float idOn = 0, idOff = 0;
            partScanFetProbe(s, g, dk, da, false, &idOff);
            partScanFetProbe(s, g, dk, da, true, &idOn);
            bool modulates = fabsf(idOn - idOff) > 0.5f;
            if (modulates) {
                bool nfet = idOn > idOff;  // conducts when the gate is HIGH
                r.type = nfet ? PartType::NFET : PartType::PFET;
                r.roles[g] = PinRole::G;
                // enhancement FET body diode: anode = source (N) / drain (P)
                r.roles[nfet ? da : dk] = PinRole::S;
                r.roles[nfet ? dk : da] = PinRole::D;
                r.value = v[da][dk];     // body-diode Vf at ~50uA
                r.value2 = nfet ? idOn : idOff;  // channel mA at 0.6V drive
                r.confidence = 0.7f;     // needs-bench: no FET on the rig yet
                r.degraded = true;
                partScanEnd(s);
                return r;
            }
            // one diode, inert third pin: a diode with a bystander row
            r.type = PartType::DIODE;
            r.roles[da] = PinRole::A;
            r.roles[dk] = PinRole::K;
            r.value = v[da][dk];
            r.confidence = 0.5f;
            r.degraded = true;
            partScanEnd(s);
            return r;
        }
    }

    r.type = PartType::UNKNOWN;
    r.confidence = 0.2f;
    r.degraded = true;
    partScanEnd(s);
    return r;
}
