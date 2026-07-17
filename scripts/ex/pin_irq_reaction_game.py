"""
Reaction Timer Game - hardware interrupts with microsecond timing
Wait for the LEDs to flash green, then tap your jumper wire into
the breadboard as fast as you can. A hard GPIO interrupt timestamps
your tap with microsecond precision - something a polling loop
could never do accurately.

This shows off machine.Pin.irq() with hard=True: the handler runs
IN interrupt context, so time.ticks_us() is captured the instant
the pin sees the edge (not whenever a polling loop gets around to
checking).

Hardware Setup:
1. Place a tact switch with one pair of legs in row 28 and the
   other pair in row 30 (it skips row 29). Pressing the button
   connects the two rows.
   - No tact switch handy? A jumper wire works too: one end in
     row 30, tap the other end into row 28 when you see GREEN.
2. When the middle of the board flashes green: SMASH IT!
"""

import time
import random
from machine import Pin

TAP_ROW = 28
GND_ROW = 30
ROUNDS = 5

# Route the game rows through the crossbar:
#  - row 28 -> GPIO_2 (our interrupt pin, pulled up)
#  - row 30 -> GND    (pressing the switch pulls GPIO_2 low)
connect(GPIO_2, TAP_ROW)
connect(GND, GND_ROW)
time.sleep_ms(100)

sense = Pin(GPIO_2, Pin.IN, Pin.PULL_UP)

# --- interrupt plumbing -------------------------------------------
# state: 0=idle  1=waiting (tap now = cheating!)  2=GO!  3=tapped  4=foul
state = [0]
t_tap = [0]

def on_tap(pin):
    # hard=True handler: runs in interrupt context. Keep it tiny,
    # don't allocate memory, just grab the timestamp and a flag.
    if state[0] == 2:
        t_tap[0] = time.ticks_us()
        state[0] = 3
    elif state[0] == 1:
        state[0] = 4  # tapped before GO - foul!

sense.irq(handler=on_tap, trigger=Pin.IRQ_FALLING, hard=True)

# --- helpers ------------------------------------------------------
# One big 8x8 block in the middle of the board. (Don't light the game
# rows themselves - net colors override overlays on rows that are
# wired to the GPIO/GND nets, so it would just get stomped.)
def flash(color):
    # overlay_set(name, x, y, w, h, colors) - named overlay block
    overlay_set("game_flash", 12, 2, 8, 8, [color] * 64)

def unflash():
    overlay_clear("game_flash")

def wait_released():
    # wait until the button is released / wire pulled out (pin back high)
    while sense.value() == 0:
        time.sleep_ms(20)
    time.sleep_ms(200)  # debounce

# --- game ---------------------------------------------------------
print("=== REACTION TIMER ===")
print("Button between rows %d and %d - press when you see GREEN!\n" % (TAP_ROW, GND_ROW))
oled_print("Reaction game!\nbutton rows %d+%d" % (TAP_ROW, GND_ROW))
time.sleep(3)

best_us = None
times = []

r = 1
while r <= ROUNDS:
    wait_released()
    unflash()
    oled_print("Round %d\nwait for it..." % r)
    print("Round %d: wait for it..." % r)

    # random suspense, watching for early taps the whole time
    state[0] = 1
    foul = False
    for _ in range(random.randint(100, 400)):  # 1.0 - 4.0 s
        if state[0] == 4:
            foul = True
            break
        time.sleep_ms(10)

    if foul:
        flash(0x780000)  # red
        oled_print("TOO SOON!\nno cheating")
        print("  too soon! round doesn't count")
        time.sleep(1.5)
        state[0] = 0
        continue

    # GO!
    t_go = time.ticks_us()
    state[0] = 2
    flash(0x008C00)  # green
    oled_print("GO!!!")

    # wait for the interrupt to catch the tap (3 s timeout)
    deadline = time.ticks_add(time.ticks_ms(), 3000)
    while state[0] == 2 and time.ticks_diff(deadline, time.ticks_ms()) > 0:
        time.sleep_ms(1)

    if state[0] != 3:
        unflash()
        oled_print("too slow!\n(3s timeout)")
        print("  no tap within 3 seconds - round counts as a miss")
        state[0] = 0
        time.sleep(1.5)
        r += 1
        continue

    reaction_us = time.ticks_diff(t_tap[0], t_go)
    times.append(reaction_us)
    if best_us is None or reaction_us < best_us:
        best_us = reaction_us

    ms = reaction_us / 1000
    oled_print("%.1f ms!" % ms)
    print("  reaction: %.1f ms  (%d microseconds)" % (ms, reaction_us))
    state[0] = 0
    time.sleep(2)
    r += 1

# --- results ------------------------------------------------------
unflash()
if times:
    avg = sum(times) / len(times) / 1000
    print("\nBest: %.1f ms   Average: %.1f ms" % (best_us / 1000, avg))
    oled_print("best %.1f ms\navg %.1f ms" % (best_us / 1000, avg))
    if best_us < 150000:
        print("Lightning reflexes!")

# cleanup: detach irq, remove our crossbar wires
sense.irq(handler=None)
disconnect(GPIO_2, TAP_ROW)
disconnect(GND, GND_ROW)
