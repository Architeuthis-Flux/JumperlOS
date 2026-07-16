# Self-check for machine.Pin.irq() on Jumperless V5 (run on the device).
#
# Routes GPIO_1 (RP2350 GPIO 20) to GPIO_2 (GPIO 21) through the crossbar,
# drives edges on GPIO_1 and counts IRQ callbacks on GPIO_2.
#
# Host usage (jumperless-v5 skill transport):
#   python scripts/jumperless.py exec --file scripts/test_pin_irq.py
#
# Expected output: "PASS pin irq: rising=3 falling=3"

import time
from machine import Pin

connect(GPIO_1, GPIO_2)
time.sleep(0.05)  # crossbar settle

drv = Pin(20, Pin.OUT, value=0)
sense = Pin(21, Pin.IN, Pin.PULL_DOWN)

counts = {"rise": 0, "fall": 0}

def on_edge(pin):
    f = irq_obj.flags()
    if f & Pin.IRQ_RISING:
        counts["rise"] += 1
    if f & Pin.IRQ_FALLING:
        counts["fall"] += 1

irq_obj = sense.irq(handler=on_edge, trigger=Pin.IRQ_RISING | Pin.IRQ_FALLING)

for _ in range(3):
    drv.high()
    time.sleep_ms(20)
    drv.low()
    time.sleep_ms(20)

time.sleep_ms(100)  # let scheduled (soft) callbacks drain

sense.irq(handler=None)  # detach
disconnect(GPIO_1, GPIO_2)

assert counts["rise"] == 3, "rising edges: got %d, want 3" % counts["rise"]
assert counts["fall"] == 3, "falling edges: got %d, want 3" % counts["fall"]
print("PASS pin irq: rising=%d falling=%d" % (counts["rise"], counts["fall"]))
