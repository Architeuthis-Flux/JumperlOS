"""
Interactive Demo - Control connections with probe, encoder, and buttons
This example shows how to use all the interactive controls together.

Hardware Setup:
- No special hardware needed
- Use the probe to tap nodes
- Use the encoder to adjust bridge spread
- Use buttons to change colors
"""

import time

node_1 = 1
node_2 = 4

last_node_1 = 0
last_node_2 = 0

bridgeSpread = 4

sleepTime_us = 1000 # controls how fast things move

hue = 230

clickwheel_reset_position() # the clickwheel is read by PIO and keeps an internal count, let's reset it to 0
last_pos = clickwheel_get_position() # should be 0 unless you're really fast

switchPosition = get_switch_position() 

while True:
    
# Probe stuff
    force_service("ProbeSwitch")
    switchPosition = get_switch_position() # this is fairly slow so we need to force service it

    if switchPosition == SWITCH_MEASURE: # make the bridge move if the switch is in measure
        node_1 += 1
        node_2 += 1
    
    tappedNode = read_probe(False) # False for non-blocking
    
    if (tappedNode != NO_PAD):
        # print(tappedNode)
        if (tappedNode <= 60):
            node_1 = int(tappedNode) # read_probe returns a PAD type so let's convert it to int so it can become a NODE
            node_2 = int(tappedNode + bridgeSpread)

# Encoder stuff
    pos = -clickwheel_get_position() # invert so it moves the right way
    
    if pos != last_pos:
        # print((pos-last_pos))
        node_1 += pos-last_pos
        node_2 = node_1+bridgeSpread
        last_pos = pos
    
    encoder_button = clickwheel_get_button()
    
    if encoder_button == CLICKWHEEL_PRESSED:
        bridgeSpread += 1
        node_2 = node_1 + bridgeSpread
        
    elif encoder_button == CLICKWHEEL_HELD:
        bridgeSpread -=1
        node_2 = node_1 + bridgeSpread

# Bounds check
    if (node_1 > 60):
        node_1 -= 60

    if (node_2 > 60):
        node_2 -= 60

    if (node_1 < 1):
        node_1 += 60
        
    if (node_2 < 1):
        node_2 += 60

# Send connections if they've changed
    if (node_1 != last_node_1) or (node_2 != last_node_2):
        oled_print("node 1 = " + str(node_1) + "\n\rnode 2 = " + str(node_2))
        # print("node 2 =" + str(node_2))

        disconnect(last_node_1, last_node_2)
        
        pause_core2(True) # we'll pause core 2 so the color of this net is set atomically
        
        connect(node_1, node_2)
        
    last_node_1 = node_1
    last_node_2 = node_2

# Net info stuff
    lastNet = get_num_nets() - 1
    set_net_color_hsv(lastNet,hue)
    pause_core2(False) # unpause so the LEDs can update with the new net color (if the nets are unchanged this does nothing)

# Probe button stuff 
    if (sleepTime_us < 50000):
        force_service("ProbeButton") # this is needed if there's not enough time in the time.sleep to check the button

    button = check_button()
    
    if (button == BUTTON_CONNECT):
        # print("CONNECT")
        hue += 3
        if (hue > 255):
            hue = 0
        # print(hue)
    if (button == BUTTON_REMOVE):
        # print("REMOVE")
        hue -= 1
        if (hue < 0):
            hue = 255
        # print (hue)

    
    time.sleep_us(sleepTime_us)

