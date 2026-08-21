"""Logic Gates 101 - companion script for /projects/nand00/wiring.yaml

Gate 1 of a 74HC00 is wired so the Jumperless owns all three of its pins:
GPIO_1 -> pin 1 (A), GPIO_2 -> pin 2 (B), pin 3 (Y) -> GPIO_3 and the LED.
So the board can drive the inputs, read the output back, and check the chip
against the truth table it is supposed to obey.

Runs standalone from the Files browser too - the launcher injects
_jl_project, but nothing here depends on it.
"""

import time

# The launcher injects this global; default so the script also runs standalone.
_jl_project = globals().get("_jl_project", {})

IN_A = GPIO_1       # node 131 -> row 5  -> 74HC00 pin 1
IN_B = GPIO_2       # node 132 -> row 6  -> 74HC00 pin 2
OUT = GPIO_3        # node 133 -> row 7  -> 74HC00 pin 3

SETTLE_S = 0.02     # the gate switches in nanoseconds; this is for the LED


def setup():
    gpio_set_dir(IN_A, True)      # True = OUTPUT
    gpio_set_dir(IN_B, True)
    gpio_set_dir(OUT, False)      # False = INPUT
    gpio_set_pull(OUT, 0)         # no pull - the 74HC00 drives this line
    gpio_set(IN_A, False)
    gpio_set(IN_B, False)


def read_out():
    """(state_object, text, bit_or_None). bit is None when the line floats."""
    s = gpio_get(OUT)
    t = str(s)
    if s == 1:
        return s, t, 1
    if s == 0:
        return s, t, 0
    return s, t, None


def apply(a, b):
    gpio_set(IN_A, bool(a))
    gpio_set(IN_B, bool(b))
    time.sleep(SETTLE_S)
    return read_out()


print("Logic Gates 101 - 74HC00 NAND")
if _jl_project:
    print("project: " + str(_jl_project.get("dir", "nand00")) +
          "  variant: " + str(_jl_project.get("variant", "default")))

setup()

print("")
print("  A  B  | Y  expected  LED")
print("  ------+-----------------")

wrong = 0
floated = 0
for a, b in ((0, 0), (0, 1), (1, 0), (1, 1)):
    state, text, bit = apply(a, b)
    expected = 0 if (a and b) else 1
    if bit is None:
        floated += 1
        verdict = "FLOATING"
    elif bit != expected:
        wrong += 1
        verdict = "WRONG"
    else:
        verdict = "ok"
    print("  %d  %d  | %s  %d         %s   %s"
          % (a, b, ("1" if bit == 1 else "0" if bit == 0 else "?"),
             expected, "on " if expected else "off", verdict))

print("")
if floated:
    print("%d of 4 reads floated - is the 74HC00 seated, and is the rail on?"
          % floated)
elif wrong:
    print("%d of 4 rows disagree with NAND - wrong chip, or a leg not in "
          "its hole?" % wrong)
else:
    print("All four rows match NAND. That is a working gate.")

try:
    oled_print("NAND ok" if (not wrong and not floated) else "NAND ??")
except Exception:
    pass

print("")
print("Live mode: type two bits (00, 01, 10, 11) to set the inputs.")
print("Empty line re-reads without changing anything. 'q' quits.")

try:
    while True:
        try:
            typed = input("A B> ").strip()
        except EOFError:
            break
        if typed == "q":
            break
        if typed == "":
            state, text, bit = read_out()
            print("   Y = " + text)
            continue
        digits = [c for c in typed if c in "01"]
        if len(digits) != 2:
            print("   need two bits, like 01")
            continue
        a = int(digits[0])
        b = int(digits[1])
        state, text, bit = apply(a, b)
        expected = 0 if (a and b) else 1
        note = "" if bit == expected else "   <- not what NAND should do"
        print("   A=%d B=%d -> Y = %s (expected %d)%s"
              % (a, b, text, expected, note))

except KeyboardInterrupt:
    pass

# Leave the inputs low: the LED stays on, which is the gate's idle state.
gpio_set(IN_A, False)
gpio_set(IN_B, False)
print("bye")
