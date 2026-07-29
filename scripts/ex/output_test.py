
import time


fakeGPIOtime = 0
regularConnectTime = 0
fastConnectTime = 0

nodes_clear()
set_dac(TOP_RAIL, 8.0)
set_dac(BOTTOM_RAIL, -8.0)

pin = FakeGpioPin(10, OUTPUT, TOP_RAIL, BOTTOM_RAIL)
pin2 = FakeGpioPin(19, OUTPUT, TOP_RAIL, BOTTOM_RAIL)

pause_core2 (True)
time.sleep (0.1)

startTime = time.ticks_us ()

for i in range (5000):
    
    pin.value(1) # Same thing as pin. on()
    pin2.off()
    pin. value (0)
    pin2.on()

    
pause_core2(False)
endTime = time.ticks_us()
fakeGPIOtime = (endTime - startTime)



nodes_clear()
time.sleep(0.5)

# pause_core2 (True)

time.sleep (0.1)
startTime = time.ticks_us ()

for i in range (50):
    connect(21, TOP_RAIL)
    disconnect(21, TOP_RAIL)

pause_core2(False)
endTime = time.ticks_us()
regularConnectTime = (endTime - startTime)

nodes_clear()

# pause_core2 (True)
time.sleep (0.1)
startTime = time.ticks_us ()

for i in range (50):
    fast_connect(21, TOP_RAIL)
    fast_disconnect(21, TOP_RAIL)

pause_core2(False)
endTime = time.ticks_us()
fastConnectTime = (endTime - startTime)



print (f"Took {fakeGPIOtime} us for 5000 toggles with fake gpio")

freq = (5000 / (fakeGPIOtime)) * 1000
print(f"Frequency = {freq} kHz")

print (f"Took {regularConnectTime} us for 50 toggles with fake gpio")

freq = (50 / (regularConnectTime)) * 1000
print(f"Frequency = {freq} kHz")

print (f"Took {fastConnectTime} us for 50 toggles with fake gpio")

freq = (50 / (fastConnectTime)) * 1000
print(f"Frequency = {freq} kHz")