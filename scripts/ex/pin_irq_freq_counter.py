"""
Frequency Counter - count PWM edges with a hard GPIO interrupt
Generates a PWM signal on GPIO_1, routes it through the crossbar
to GPIO_2, and measures its frequency by counting rising edges in
an interrupt handler. Turn the click wheel to change the frequency
and watch the measurement track it. Click the wheel to quit.

This is the "two ways" idea in one script:
  - jumperless builtins generate the signal:  pwm(GPIO_1, ...)
  - stock machine.Pin.irq() measures it:      sense.irq(...)

Hardware Setup:
- None! The signal is routed internally. (But you can also probe
  row 20 with a scope - the signal is really there.)
"""

import time
from machine import Pin

FREQ_MIN = 50
FREQ_MAX = 10000
STEP = 50        # Hz per click-wheel detent
GATE_MS = 250    # measurement window

# Route the signal: GPIO_1 (PWM out) -> GPIO_2 (counter in),
# and also to breadboard row 20 so you can see/probe it.
connect(GPIO_1, GPIO_2)
connect(GPIO_1, 20)
time.sleep_ms(50)

# --- signal source: the Jumperless way -----------------------------
freq = 1000
pwm(GPIO_1, freq, 0.5)  # node name, Hz, 50% duty
# (stock alternative: from machine import PWM; PWM(Pin(GPIO_1), freq=1000))

# --- counter: the stock MicroPython way -----------------------------
sense = Pin(GPIO_2, Pin.IN)
count = [0]

def on_rise(pin):
    # hard=True: runs in interrupt context on EVERY edge.
    # Keep it to one line - at 10 kHz this runs 10,000x per second!
    count[0] += 1

sense.irq(handler=on_rise, trigger=Pin.IRQ_RISING, hard=True)

# --- UI loop --------------------------------------------------------
print("=== FREQUENCY COUNTER ===")
print("Turn the click wheel to change frequency, press it to quit.\n")

clickwheel_reset_position()
last_pos = clickwheel_get_position()

try:
    while True:
        # open the gate: zero the counter, wait, read it back
        count[0] = 0
        t0 = time.ticks_ms()
        time.sleep_ms(GATE_MS)
        elapsed = time.ticks_diff(time.ticks_ms(), t0)
        measured = count[0] * 1000 // elapsed

        print("set: %5d Hz   measured: %5d Hz   (%d edges in %d ms)"
              % (freq, measured, count[0], elapsed))
        oled_print("set  %d Hz\nmeas %d Hz" % (freq, measured))

        # click wheel: rotate = tune, press = quit
        pos = clickwheel_get_position()
        if pos != last_pos:
            freq += (last_pos - pos) * STEP
            freq = max(FREQ_MIN, min(FREQ_MAX, freq))
            last_pos = pos
            pwm_set_frequency(GPIO_1, freq)

        if clickwheel_get_button() == CLICKWHEEL_PRESSED:
            break
finally:
    # always clean up, even on Ctrl+Q
    sense.irq(handler=None)
    pwm_stop(GPIO_1)
    disconnect(GPIO_1, GPIO_2)
    disconnect(GPIO_1, 20)
    oled_print("freq counter\nstopped")
    print("\nStopped. Interrupts detached, PWM off, wires removed.")
