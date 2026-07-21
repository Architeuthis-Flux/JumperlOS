"""
Crosspoint Resistance Calibration
=================================

Bench tool: connects a few row pairs, prints the firmware's predicted
crossbar resistance (resistance_between, stacking-aware), and asks you to
measure each pair with a multimeter. Then least-squares fits the real
ohms-per-crosspoint against the hardcoded 43.

NOTE: anything else bridging a pair (a physical jumper, a part on the
board) makes the meter read LOWER than predicted - measure bare rows.
"""

# type: ignore
from jumperless import *
import time

PAIRS = [(1, 5), (1, 30), (31, 45), (10, 50)]
K_NOMINAL = 43.0


def fit_k(points):
    """points = [(effective_crosspoints, measured_ohms)]; model R = k * C."""
    num = sum(c * r for c, r in points)
    den = sum(c * c for c, r in points)
    return num / den

assert abs(fit_k([(1.0, 43.0), (2.0, 86.0)]) - 43.0) < 1e-6


def lane_crosspoints(a, b):
    """Crosspoints in the primary lane between a and b (0 if unrouted)."""
    info = get_path_between(a, b)
    if not info:
        return 0
    return sum(1 for j in range(4)
               if info["chips"][j] != -1 and info["x"][j] != -1 and info["y"][j] != -1)


def main():
    points = []  # (a, b, predicted, effective_crosspoints, measured)
    for a, b in PAIRS:
        connect(a, b)
        time.sleep(0.3)
        predicted = resistance_between(a, b)
        if predicted is None or predicted <= 0:
            print("rows %d-%d: no routed path, skipping" % (a, b))
            disconnect(a, b)
            continue
        # ponytail: lane count inferred as primary-lane ohms / parallel total;
        # exact only when all stacked lanes have equal crosspoint counts.
        # Upgrade path: firmware call reporting matching lanes directly.
        lane_ohms = lane_crosspoints(a, b) * K_NOMINAL
        lanes = int(round(lane_ohms / predicted)) if lane_ohms > 0 else 1
        print("measure rows %d-%d with your multimeter, predicted %.1f ohms (%d lanes)"
              % (a, b, predicted, lanes))
        s = input("  measured ohms (blank to skip): ").strip()
        if s:
            points.append((a, b, predicted, predicted / K_NOMINAL, float(s)))
        disconnect(a, b)

    if not points:
        print("no measurements entered, nothing to fit")
        return
    k = fit_k([(c, m) for (_, _, _, c, m) in points])
    print("fitted ohms/crosspoint: %.2f (hardcoded: %.1f)" % (k, K_NOMINAL))
    for a, b, predicted, c, m in points:
        print("  rows %d-%d: measured %.1f, fit predicts %.1f (residual %+.1f)"
              % (a, b, m, k * c, m - k * c))


main()
