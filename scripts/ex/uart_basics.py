"""
Basic UART operations.
This example shows how to use UART.
"""

from machine import UART
import time

uart = UART(0, 115200)
uart.init(115200, 8, None, 1)

connect(UART_TX, D0, 0)
connect(UART_RX, D1, 0)

print("UART Basics Demo")
print("This example will send a message to the Arduino Nano over UART")

time.sleep(1)

buffer = "Sup Arduino"
while True:
    uart.write(buffer)
    time.sleep(1.5)