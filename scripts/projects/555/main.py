"""555 LED Flasher - companion script for /projects/555/wiring.yaml

Watches the 555's OUT pin (bridged to ADC0 by the wiring file), counts the
rising edges past a 2.5 V threshold, and reports the blink rate every 3
seconds along with the timing cap's voltage (ADC1 -> node 7).

Runs standalone from the Files browser too - the launcher injects
_jl_project, but nothing here depends on it.
"""

import time

# The launcher injects this global; default so the script also runs standalone.
_jl_project = globals().get("_jl_project", {})

THRESHOLD = 2.5     # volts - the 555's OUT swings rail to rail
REPORT_MS = 3000    # print a line this often

print("555 astable running...")
print("Hold the clickwheel to exit.")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "555")) +
          "  variant: " + str(_jl_project.get("variant", "default")))

try:
    oled_connect()
except Exception:
    pass

edges = 0
above = adc_get(0) > THRESHOLD
window_start = time.ticks_ms()

try:
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
    try:
        oled_clear()
    except Exception:
        pass
    print("bye")
