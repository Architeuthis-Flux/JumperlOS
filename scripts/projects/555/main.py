"""555 LED Flasher - companion script for /projects/555/wiring.yaml

Watches the 555's OUT pin, counts the rising edges past a 2.5 V threshold, and
reports the blink rate every 3 seconds along with the timing cap's voltage.

THE TAPS ARE THIS SCRIPT'S, NOT THE PROJECT'S. wiring.yaml used to ship
ADC0-37 and ADC1-7 in its `bridges:` section, so opening the project put two
wires on the board that the circuit does not need and the user never asked
for. They belong to the measurement, not to the 555, so they are made here at
start-up and taken back down on the way out - and only the ones this run
actually created, so a tap you wired yourself survives.

  ADC0 -> row 37   the 555's OUT pin (U1 pin 3, dip8 anchored at row 35)
  ADC1 -> row 7    the RC timing junction - THR/TRIG and C1's + leg

Runs standalone from the Files browser too - the launcher injects
_jl_project, but nothing here depends on it.
"""

import time

# The launcher injects this global; default so the script also runs standalone.
_jl_project = globals().get("_jl_project", {})

THRESHOLD = 2.5     # volts - the 555's OUT swings rail to rail
REPORT_MS = 3000    # print a line this often

OUT_ROW = 37        # U1 pin 3
CAP_ROW = 7         # THR / TRIG / C1+

# (node, row) pairs this script may need to bridge for its own measurement.
# The nodes are named as STRINGS on purpose: connect/disconnect/is_connected
# all take a NodeRef, and the string form works from a plain Files-browser run
# where the module's bare ADC0/ADC1 constants may not be in scope.
TAPS = (("ADC0", OUT_ROW), ("ADC1", CAP_ROW))

# Filled in below with only the taps WE made, so the teardown cannot remove a
# connection that was already on the board when we got here.
made = []


def taps_up():
    """Bridge ADC0/ADC1 to the rows we measure. Idempotent and non-destructive."""
    for node, row in TAPS:
        try:
            if is_connected(node, row):
                print("tap %s-%d was already there - leaving it alone"
                      % (str(node), row))
                continue
            connect(node, row)
            made.append((node, row))
        except Exception as e:
            print("could not tap %s-%d: %s" % (str(node), row, str(e)))


def taps_down():
    """Remove exactly the taps taps_up() made. Safe to call twice."""
    while made:
        node, row = made.pop()
        try:
            disconnect(node, row)
        except Exception as e:
            print("could not un-tap %s-%d: %s" % (str(node), row, str(e)))


print("555 astable running...")
print("Hold the clickwheel to exit.")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "555")) +
          "  variant: " + str(_jl_project.get("variant", "default")))

# EVERYTHING FROM taps_up() ON IS INSIDE THE try, so the `finally` below owns
# the taps from the instant they exist. The set-up is not instantaneous - it
# makes two crossbar connections, brings up the OLED and takes a real ADC
# reading - and a Ctrl-C or a clickwheel hold anywhere in that window would
# otherwise strand ADC0-37 and ADC1-7 on the board, which is the exact state
# this script exists to avoid leaving behind.
try:
    taps_up()
    print("measuring: ADC0 on row %d (OUT), ADC1 on row %d (cap)"
          % (OUT_ROW, CAP_ROW))

    try:
        oled_connect()
    except Exception:
        pass

    edges = 0
    above = adc_get(0) > THRESHOLD
    window_start = time.ticks_ms()

    while True:
        out_v = adc_get(0)
        if out_v > THRESHOLD:
            if not above:
                edges += 1     # rising edge
                above = True
        elif out_v < THRESHOLD:
            above = False

        elapsed = time.ticks_diff(time.ticks_ms(), window_start)
        if elapsed >= REPORT_MS:
            freq = (edges * 1000.0) / elapsed
            cap_v = adc_get(1)
            print("freq: %.2f Hz   cap: %.2f V" % (freq, cap_v))
            try:
                oled_print("%.2f Hz" % freq)
            except Exception:
                pass
            edges = 0
            window_start = time.ticks_ms()

        time.sleep(0.002)   # ~500 Hz sampling: plenty for a ~1.4 Hz blink

except KeyboardInterrupt:
    pass
finally:
    # finally, not just the except arm: a break, an unexpected exception and a
    # clean fall-through all have to leave the board the way we found it.
    taps_down()
    try:
        oled_clear()
    except Exception:
        pass
    print("bye")
