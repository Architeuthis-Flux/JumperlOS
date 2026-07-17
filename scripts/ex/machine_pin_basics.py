"""
Machine Pin Basics - Two ways to control the same GPIO
The Jumperless gives you TWO ways to talk to a GPIO pin:

  1. Jumperless builtins:  gpio_set(), gpio_get(), gpio_set_dir(), ...
     - take breadboard NODE names (GPIO_1..GPIO_8)
     - plays nice with the routing fabric and LED colors

  2. Standard MicroPython: machine.Pin
     - the same API you'd use on any Pico / ESP32 / etc.
     - unlocks stock-MicroPython goodies like pin.irq() callbacks

Both drive the SAME physical pins. GPIO_1..GPIO_8 are RP2350
GPIOs 20..27 under the hood, and Pin() accepts either name.

Hardware Setup:
- None! Everything is wired internally through the crossbar.
"""

import time
from machine import Pin

print("=== Two ways to control a GPIO ===\n")

# Wire GPIO_1 to GPIO_2 through the crossbar so we can
# drive one pin and listen on the other. No jumper wires needed.
connect(GPIO_1, GPIO_2)
time.sleep_ms(50)  # let the crossbar settle

# ---------------------------------------------------------------
# Way 1: Jumperless builtins (node names)
# ---------------------------------------------------------------
print("Way 1: gpio_set() / gpio_get() with node names")

gpio_set_dir(GPIO_1, True)    # True = OUTPUT
gpio_set_dir(GPIO_2, False)   # False = INPUT

for i in range(3):
    gpio_set(GPIO_1, True)
    time.sleep_ms(50)
    print("  drove GPIO_1 HIGH -> gpio_get(GPIO_2) =", gpio_get(GPIO_2))
    gpio_set(GPIO_1, False)
    time.sleep_ms(50)
    print("  drove GPIO_1 LOW  -> gpio_get(GPIO_2) =", gpio_get(GPIO_2))

# ---------------------------------------------------------------
# Way 2: machine.Pin (standard MicroPython)
# ---------------------------------------------------------------
print("\nWay 2: machine.Pin - same pins, stock MicroPython API")

# Pin() accepts a Jumperless node OR the raw RP2350 GPIO number.
# GPIO_1 is node 131 on the breadboard, but RP2350 GPIO 20 inside.
out_pin = Pin(GPIO_1, Pin.OUT)    # same as Pin(20, Pin.OUT)
in_pin = Pin(GPIO_2, Pin.IN)      # same as Pin(21, Pin.IN)
print("  int(GPIO_1) =", int(GPIO_1), " but ", out_pin, "<- RP2350 numbering")

out_pin.high()
time.sleep_ms(50)
print("  out_pin.high() -> in_pin.value() =", in_pin.value())

out_pin.low()
time.sleep_ms(50)
print("  out_pin.low()  -> in_pin.value() =", in_pin.value())

# All the stock aliases work too:
out_pin.on()       # same as .high()
out_pin.off()      # same as .low()
out_pin.toggle()   # flip it
out_pin(0)         # calling the pin sets it, like value(0)
print("  on()/off()/toggle()/value() all available")

# Pull resistors, the stock way:
pulled = Pin(GPIO_3, Pin.IN, Pin.PULL_UP)
print("  GPIO_3 floating with PULL_UP reads:", pulled.value())
connect(GPIO_3, GND)  # a real driver always beats a pull resistor
time.sleep_ms(50)
print("  GPIO_3 wired to GND (pull-up loses):", pulled.value())
disconnect(GPIO_3, GND)
# (Fun fact: we skip the PULL_DOWN demo because of RP2350 erratum E9 -
#  a floating pin with internal pull-down latches at ~2V and reads 1!)

# ---------------------------------------------------------------
# Mix and match - they are literally the same pin
# ---------------------------------------------------------------
print("\nMix them: drive with gpio_set(), read with machine.Pin")
gpio_set(GPIO_1, True)
time.sleep_ms(50)
print("  gpio_set(GPIO_1, True) -> in_pin.value() =", in_pin.value())
gpio_set(GPIO_1, False)

# ---------------------------------------------------------------
# The payoff: pin.irq() - hardware interrupts!
# ---------------------------------------------------------------
# Instead of polling in a loop, ask the chip to call a function
# the instant the pin changes. Only machine.Pin can do this.
print("\npin.irq(): interrupt fires the moment the pin changes")

edges = {"rising": 0, "falling": 0}

def on_edge(pin):
    # .flags() tells you which edge(s) woke us up
    f = irq_handle.flags()
    if f & Pin.IRQ_RISING:
        edges["rising"] += 1
    if f & Pin.IRQ_FALLING:
        edges["falling"] += 1

irq_handle = in_pin.irq(handler=on_edge, trigger=Pin.IRQ_RISING | Pin.IRQ_FALLING)

print("  toggling GPIO_1 six times (= 3 rising + 3 falling edges)...")
for i in range(6):
    out_pin.toggle()
    time.sleep_ms(20)
time.sleep_ms(100)  # let the last callbacks run

print("  IRQ counted:", edges["rising"], "rising,", edges["falling"], "falling edges")
print("  (no polling loop needed - the handler was CALLED by the hardware)")

# Tip: pass hard=True to run the handler in interrupt context for
# microsecond response - see pin_irq_reaction_game.py

# Cleanup: detach the interrupt and remove our crossbar wire
in_pin.irq(handler=None)
disconnect(GPIO_1, GPIO_2)

oled_print("Pin basics done!")
print("\nDone! Try pin_irq_reaction_game.py next.")
