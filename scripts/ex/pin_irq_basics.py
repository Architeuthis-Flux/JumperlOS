"""
machine.Pin interrupt (IRQ) basics.

Routes GPIO_1 to GPIO_2 through the crossbar, then uses one pin to
generate edges and the other to catch them with an interrupt handler -
no external wiring needed.

Shows: attaching a Pin.irq() handler, picking triggers, telling rising
from falling edges with irq.flags(), and cleanly detaching when done.
"""

import time
from machine import Pin

print("=" * 46)
print(" machine.Pin IRQ Basics")
print("=" * 46)

# --- 1. Wire it up (in software!) -------------------------------------
# GPIO_1..GPIO_8 are the Jumperless's routable GPIO. GPIO_1 is RP2350
# pin 20, GPIO_2 is pin 21. Connecting them through the crossbar means
# whatever we drive on GPIO_1 shows up on GPIO_2.
print("\nRouting GPIO_1 -> GPIO_2 through the crossbar...")
connect(GPIO_1, GPIO_2)
time.sleep(0.05)                      # let the crossbar settle

drv = Pin(20, Pin.OUT, value=0)       # GPIO_1: we drive this one
sense = Pin(21, Pin.IN, Pin.PULL_DOWN)  # GPIO_2: this one listens

# --- 2. Attach the interrupt handler ----------------------------------
# The handler runs whenever the trigger condition happens - your main
# code doesn't have to poll. Keep handlers SHORT: update a counter or
# set a flag, and do the real work in your main loop.
counts = {"rise": 0, "fall": 0}

def on_edge(pin):
    f = irq_obj.flags()               # which trigger(s) fired?
    if f & Pin.IRQ_RISING:
        counts["rise"] += 1
    if f & Pin.IRQ_FALLING:
        counts["fall"] += 1

irq_obj = sense.irq(handler=on_edge,
                    trigger=Pin.IRQ_RISING | Pin.IRQ_FALLING)
print("IRQ handler attached (rising + falling edges).")

# --- 3. Generate edges and watch them arrive --------------------------
print("\nPulsing GPIO_1 five times:")
for i in range(5):
    drv.high()
    time.sleep_ms(20)
    drv.low()
    time.sleep_ms(20)
    print("  pulse %d -> rising=%d falling=%d"
          % (i + 1, counts["rise"], counts["fall"]))

time.sleep_ms(100)                    # let scheduled callbacks drain

# --- 4. Detach and clean up -------------------------------------------
# Always detach handlers and undo routing when you're done, so the pins
# behave normally for the next script.
sense.irq(handler=None)
disconnect(GPIO_1, GPIO_2)
print("\nHandler detached, crossbar route removed.")

assert counts["rise"] == 5, "rising: got %d, want 5" % counts["rise"]
assert counts["fall"] == 5, "falling: got %d, want 5" % counts["fall"]

print()
print("=" * 46)
print(" PASS - caught %d rising and %d falling edges"
      % (counts["rise"], counts["fall"]))
print("=" * 46)
print()
print("Try it yourself: connect(GPIO_2, 25), attach the IRQ again, and")
print("tap a wire between row 25 and TOP_RAIL to fire real interrupts")
print("(expect bounce - one tap often lands several edges!)")
