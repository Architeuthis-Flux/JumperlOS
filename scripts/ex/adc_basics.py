"""
Basic ADC (Analog-to-Digital Converter) operations.
This example shows how to read analog voltages.
"""

import time

print("ADC Basics Demo")
    
# Read all ADC channels
channels = [0, 1, 2, 3]

print("Reading ADC channels (Ctrl+Q to stop):")
print("Connect voltage sources to ADC inputs")
    
    
while True:
    print("\nADC Readings:")
    for channel in channels:
        voltage = adc_get(channel)
        print("  ADC" + str(channel) + ": " + str(round(voltage, 3)) + "V")
    time.sleep(0.5)
            
