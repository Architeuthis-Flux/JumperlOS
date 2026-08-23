"""555 LED Flasher - companion for /projects/555/wiring.yaml. See README.md.

Times the blink on OUT and compares it to what the parts actually placed
predict - resistors come back from list_parts()['measured'], so substituting
one moves the answer instead of breaking anything.

  ADC0 -> row 37   OUT (U1 pin 3, dip8 anchored at row 35)
  ADC1 -> row 7    the RC junction - THR/TRIG and C1's + leg

Both taps are made here and removed on the way out, and only the ones this run
created. Runs standalone from the Files browser; _jl_project is optional.

KEEP THIS LEAN - MicroPython compiles it out of one shared heap that never
resets between runs, so every KB here is a KB the next script cannot have.
"""

import time

_jl_project = globals().get("_jl_project", {})

THRESHOLD = 2.5     # volts - OUT swings rail to rail
REPORT_MS = 3000

OUT_ROW = 37
CAP_ROW = 7
TAPS = (("ADC0", OUT_ROW), ("ADC1", CAP_ROW))

_MUL = {"p": 1e-12, "n": 1e-9, "u": 1e-6, "m": 1e-3,
        "": 1.0, "k": 1e3, "K": 1e3, "M": 1e6}


def val(s):
    """'47k'->47000.0, '10uF'->1e-05, '330'->330.0, else 0.0. '4k7' reads as
    4k - close enough, and the guide measures resistors anyway."""
    s = (s or "").strip()
    n = ""
    i = 0
    while i < len(s) and (s[i].isdigit() or s[i] in ".+-"):
        n += s[i]
        i += 1
    if not n:
        return 0.0
    try:
        return float(n) * _MUL.get(s[i:i + 1], 1.0)
    except Exception:
        return 0.0


def eng(x):
    for m, suf in ((1e6, "M"), (1e3, "k"), (1, ""),
                   (1e-3, "m"), (1e-6, "u"), (1e-9, "n")):
        if abs(x) >= m:
            return "%.3g%s" % (x / m, suf)
    return "%.3g" % x


made = []


def taps_up():
    for node, row in TAPS:
        try:
            if is_connected(node, row):
                print("tap %s-%d was already there - left alone" % (node, row))
                continue
            connect(node, row)
            made.append((node, row))
        except Exception as e:
            print("could not tap %s-%d: %s" % (node, row, str(e)))


def taps_down():
    while made:
        node, row = made.pop()
        try:
            disconnect(node, row)
        except Exception as e:
            print("could not un-tap %s-%d: %s" % (node, row, str(e)))


def expected():
    """(f_Hz, duty_pct, R1, R2, C1, how) from the parts on the board.

    A resistor's `measured` ohms beats the file's `value:`. C1 has no such
    reading - capacitance is not measurable here - so it is taken on trust.
    """
    p = {}
    try:
        for e in list_parts():
            p[e["name"]] = e
    except Exception:
        pass

    real = 0

    def ohms(name):
        e = p.get(name)
        if not e:
            return 0.0
        m = e.get("measured", 0.0)
        if m and m > 0:
            return m
        return val(e.get("value", ""))

    for n in ("R1", "R2"):
        e = p.get(n)
        if e and e.get("measured", 0.0) > 0:
            real += 1
    r1, r2 = ohms("R1"), ohms("R2")
    c1 = val(p.get("C1", {}).get("value", ""))
    how = ("measured" if real == 2 else
           "part-measured" if real else "from the project file")
    if r1 > 0 and r2 > 0 and c1 > 0:
        span = r1 + 2 * r2
        return 1.44 / (span * c1), 100.0 * (r1 + r2) / span, r1, r2, c1, how
    return 0.0, 0.0, r1, r2, c1, how


print("555 astable")
print("Hold the clickwheel to exit.")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "555")))

# Everything from taps_up() on is inside the try, so `finally` owns the taps
# from the instant they exist - a Ctrl-C during set-up must not strand them.
try:
    taps_up()

    exp_f, exp_d, R1, R2, C1, how = expected()
    if exp_f > 0:
        print("your parts: R1 %s  R2 %s  C1 %sF   (R %s)"
              % (eng(R1), eng(R2), eng(C1), how))
        print("they predict: %.2f Hz, %.0f%% high" % (exp_f, exp_d))
        if how != "measured":
            print("  (no continuity reading for every resistor - run the")
            print("   guided build to measure them instead of trusting the file)")
    else:
        print("no usable R1/R2/C1 values - reporting the measurement only")

    try:
        oled_connect()
    except Exception:
        pass

    edges = 0
    high = 0
    total = 0
    above = adc_get(0) > THRESHOLD
    window_start = time.ticks_ms()

    while True:
        out_v = adc_get(0)
        total += 1
        if out_v > THRESHOLD:
            high += 1
            if not above:
                edges += 1     # rising edge
                above = True
        else:
            above = False

        elapsed = time.ticks_diff(time.ticks_ms(), window_start)
        if elapsed >= REPORT_MS:
            freq = (edges * 1000.0) / elapsed
            duty = (100.0 * high / total) if total else 0.0
            line = "%.2f Hz, %.0f%% high" % (freq, duty)
            if exp_f > 0:
                # Side by side, never pass/fail - the bench decides.
                line += "   (predicted %.2f Hz, %.0f%%)" % (exp_f, exp_d)
            print("measured: %s   cap %.2f V" % (line, adc_get(1)))
            if freq > 0.05 and R1 > 0 and R2 > 0:
                # The unmeasurable part, solved backwards from the blink.
                print("  the blink implies C1 = %sF" % eng(1.44 / ((R1 + 2 * R2) * freq)))
            try:
                oled_print("%.2f Hz" % freq)
            except Exception:
                pass
            edges = high = total = 0
            window_start = time.ticks_ms()

        time.sleep(0.002)   # ~500 Hz: plenty for a ~1 Hz blink, and it is what
                            # makes the duty-cycle count meaningful

except KeyboardInterrupt:
    pass
finally:
    # finally, not just the except arm: a break, an exception and a clean
    # fall-through all have to leave the board the way we found it.
    taps_down()
    try:
        oled_clear()
    except Exception:
        pass
    print("bye")
