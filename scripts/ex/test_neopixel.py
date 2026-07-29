"""
Interactive NeoPixel Demo

This also shows how you can grab modules 
from the Viper IDE package manager

At the bottom left of the window, click the 3 boxes icon, 
find neopixel in the list and click the download button, 
then this demo should work.

Controls:
- ROTATE: Change animation speed
- BUTTON: Switch animation mode
"""

import machine
import time
import neopixel

NUM_PIXELS = 15

print("=" * 60)
print("🎮 INTERACTIVE NEOPIXEL DEMO 🌈")
print("=" * 60)
print("\nControls:")
print("  🔄 Rotate encoder = Speed")
print("  🔘 Press button   = Change mode\n")

# Setup connections
print("Setting up connections...")
nodes_clear()
connect(GPIO_1, 15)  # Data to breadboard row 15
connect(GND, 14)      # Ground
connect(TOP_RAIL, 16) # Power (5V)

# Create NeoPixel strip using the library
print("Initializing NeoPixels...")

pin = machine.Pin(20, machine.Pin.OUT) # machine pin 20 is GPIO_1
np = neopixel.NeoPixel(pin, NUM_PIXELS)

# Color utility functions
def wheel(pos):
    """Generate rainbow colors across 0-255 positions"""
    pos = pos % 256
    if pos < 85:
        return (pos * 3, 255 - pos * 3, 0)
    elif pos < 170:
        pos -= 85
        return (255 - pos * 3, 0, pos * 3)
    else:
        pos -= 170
        return (0, pos * 3, 255 - pos * 3)

# Animation modes
modes = [
    "🌊 Rainbow Flow",
    "✨ Sparkle",
    "🎭 Theater Chase",
    "🌈 Rainbow Cycle",
    "🔥 Fire",
    "💎 Twinkle"
]

# State variables
current_mode = 0
frame = 0
last_button_state = 0
last_speed = 0
speed = 50

# Reset encoder position for clean start
clickwheel_reset_position()

print(f"Mode: {modes[current_mode]}")
print(f"Speed: 50/100 (turn encoder to adjust)\n")

try:
    while True:
        # Check encoder button
        button = clickwheel_get_button()
        if button and not last_button_state:
            current_mode = (current_mode + 1) % len(modes)
            frame = 0
            print(f"\n🎨 Mode: {modes[current_mode]}")
            oled_print(f"{modes[current_mode]}")
            time.sleep(0.8)
            
        last_button_state = button
        
        # Get absolute encoder position and map to speed
        position = clickwheel_get_position()
       
        # Position can be negative or positive, so we clamp to 1-1000 range
        speed = max(1, min(1000,speed + position))
        
        clickwheel_reset_position()
        
        # Show speed changes
        if speed != last_speed:
            # print(f"⚡ Speed: {speed}/1000")
            last_speed = speed
            delay_us = 50000 // speed
            print(f"\r                       \r⚡ Delay: {delay_us} us", end="")
            oled_print("Delay " + str(delay_us) + " us")
            time.sleep(0.01)
            continue
        # Calculate delay based on speed (inverse relationship)
        
        # Render current animation mode (all scaled by speed)
        step = frame * speed // 10  # Scale animation speed
        
        if current_mode == 0:  # Rainbow Flow
            for i in range(NUM_PIXELS):
                pixel_index = (i * 256 // NUM_PIXELS + step) % 256
                np[i] = wheel(pixel_index)
        
        elif current_mode == 1:  # Sparkle
            # Fade all pixels
            for i in range(NUM_PIXELS):
                r, g, b = np[i]
                np[i] = (int(r * 0.9), int(g * 0.9), int(b * 0.9))
            # Add new sparkle (speed affects frequency)
            if frame % max(1, 10 - speed // 10) == 0:
                sparkle_pos = (step // 10) % NUM_PIXELS
                np[sparkle_pos] = wheel((step * 23) % 256)
        
        elif current_mode == 2:  # Theater Chase
            offset = (step // 5) % 3
            for i in range(NUM_PIXELS):
                if (i + offset) % 4 == 0:
                    np[i] = wheel((i * 256 // NUM_PIXELS + step) % 256)
                else:
                    np[i] = (0, 0, 0)
        
        elif current_mode == 3:  # Rainbow Cycle
            for i in range(NUM_PIXELS):
                np[i] = wheel((i * 20 + step // 2) % 256)
        
        elif current_mode == 4:  # Fire
            for i in range(NUM_PIXELS):
                flicker = (step + i * 7) % 256
                if flicker < 128:
                    r = 255
                    g = flicker * 2
                else:
                    r = 255 - (flicker - 128) * 2
                    g = 255 - (flicker - 128)
                np[i] = (r, g, 0)
        
        elif current_mode == 5:  # Twinkle
            # Random-like twinkling using frame counter
            for i in range(NUM_PIXELS):
                val = (step * (i + 1) * 13) % 256
                if val > 8:
                    np[i] = (0,0,0)
                else:
                    np[i] = wheel((i * 20) % 256)
        
        # Write to strip
        np.write()
        
        # Update frame counter
        frame = (frame + 1) % 10000
        
        # Delay based on speed
        time.sleep_us(delay_us)

except KeyboardInterrupt:
    print("\n\n✨ Shutting down...")
    # Turn off all pixels
    for i in range(NUM_PIXELS):
        np[i] = (0, 0, 0)
    np.write()
    print("\n" + "=" * 60)
    print("✓ Demo stopped! Press Ctrl+C again to exit.")
    print("=" * 60)
