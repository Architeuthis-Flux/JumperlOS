#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Routing regressions: DAC-to-row-to-ADC, disconnect, and the RouteSafety
driven-pair exemption (GPIO-to-GND and GPIO-to-GPIO loopback must route)."""

from jl import jl_exec, parse_kv, check, finish

# --- 1. DAC voltage through the crossbar to an ADC -------------------------
out = jl_exec("""
import time
nodes_clear()
time.sleep(0.1)
dac_set(DAC0, 2.5)
connect(DAC0, 10)
connect(ADC0, 10)
time.sleep(0.05)
vs = [adc_get(0) for _ in range(8)]
print("v_routed=", sum(vs) / len(vs))
disconnect(DAC0, -1)
time.sleep(0.05)
print("still_connected=", 1 if is_connected(DAC0, 10) else 0)
nodes_clear()
""")
vals = parse_kv(out)
check(abs(vals.get("v_routed", -99) - 2.5) < 0.3,
      f"DAC0 2.5V reads {vals.get('v_routed')}V at row 10 via ADC0")
check(vals.get("still_connected") == 0, "disconnect(DAC0,-1) removes the bridge")

# --- 2. GPIO-to-GND net must route (driven-pair same-net exemption) --------
# GPIOs count as RouteSafety driven sources regardless of direction; before
# the same-net exemption this path was silently skipped. Electrical proof:
# input + pull-up reads LOW only if the row is physically tied to GND.
out = jl_exec("""
import time
nodes_clear()
time.sleep(0.1)
gpio_set_dir(GPIO_1, INPUT)
try:
    gpio_set_pull(GPIO_1, PULL_UP)
except NameError:
    gpio_set_pull(GPIO_1, 1)
connect(GPIO_1, 25)
connect(GND, 25)
time.sleep(0.1)
s = str(gpio_get(GPIO_1)).lower()
print("gnd_read=", 1 if ("high" in s or s == "1" or s == "true") else 0)
try:
    gpio_set_pull(GPIO_1, PULL_NONE)
except NameError:
    gpio_set_pull(GPIO_1, 0)
nodes_clear()
""")
vals = parse_kv(out)
check(vals.get("gnd_read") == 0,
      "GPIO_1 (input, pull-up) reads LOW through a GPIO+GND net")

# --- 3. GPIO loopback (two driven sources in one net) ----------------------
out = jl_exec("""
import time
nodes_clear()
time.sleep(0.1)
connect(GPIO_1, 30)
connect(GPIO_2, 30)
time.sleep(0.1)
gpio_set_dir(GPIO_1, OUTPUT)
gpio_set_dir(GPIO_2, INPUT)
gpio_set(GPIO_1, HIGH)
time.sleep(0.02)
s = str(gpio_get(GPIO_2)).lower()
print("loop_high=", 1 if ("high" in s or s == "1" or s == "true") else 0)
gpio_set(GPIO_1, LOW)
time.sleep(0.02)
s = str(gpio_get(GPIO_2)).lower()
print("loop_low=", 1 if ("high" in s or s == "1" or s == "true") else 0)
gpio_set_dir(GPIO_1, INPUT)
nodes_clear()
""")
vals = parse_kv(out)
check(vals.get("loop_high") == 1, "GPIO_1 high seen at GPIO_2 (loopback net routed)")
check(vals.get("loop_low") == 0, "GPIO_1 low seen at GPIO_2")

finish("test_routing")
