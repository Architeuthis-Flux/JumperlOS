"""
Voltage Monitor Demo
Monitor voltage on ADC with real-time OLED display.

Hardware Setup:
1. Connect voltage source to breadboard row 20
2. Voltage range: 0V to 3.3V
"""

import time

print("Voltage Monitor Demo")

disconnect(12, -1)
disconnect(ADC0, -1)
connect(ADC0, 12)
print("ADC0 connected to row 12")
print("Connect voltage source to row 12")

oled_print("Voltage Monitor")
time.sleep(1)

while True:
    voltage = adc_get(0)
    oled_print(str(round(voltage, 3)) + " V")
    print("\r                      ", end="\r")
    print("Voltage: " + str(round(voltage, 3)) + "V", end="")
    time.sleep(0.15)

