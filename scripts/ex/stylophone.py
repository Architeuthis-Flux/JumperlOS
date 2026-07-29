"""
Jumperless Stylophone
Musical instrument using probe and GPIO to generate audio tones.

Hardware Setup:
1. Connect speaker between rows 25 (positive) and 55 (negative)    
"""

import time

speaker_pos_row = 25
speaker_neg_row = 55
freq_multiplier = 40.0

def setup_audio():
    disconnect(speaker_pos_row, -1)
    disconnect(speaker_neg_row, -1)
    connect(GPIO_1, speaker_pos_row)
    connect(GND, speaker_neg_row)
    pwm(GPIO_1, 10, 0.5)
    print("Connect speaker: positive to row " + str(speaker_pos_row) + ", negative to row " + str(speaker_neg_row))
    time.sleep(0.1)

print("Jumperless Stylophone")
oled_print("Touch pads!")

setup_audio()


sustain = 500
sustain_timer = sustain

while True:
    
    pad = probe_read_nonblocking()
    if pad != NO_PAD:

        frequency = float(pad) * freq_multiplier
        pwm(GPIO_1, frequency, 0.5)
        # pwm_set_frequency(GPIO_1, frequency)

        print("\r                                 ", end="\r")
        print("Pad: " + str(pad) + " " + str(frequency) + " Hz", end="")
        oled_print(str(frequency) + " Hz")
        sustain_timer = time.ticks_ms() + sustain


    if time.ticks_ms() > sustain_timer:
        pwm_stop(GPIO_1)

    force_service("ProbeButton")
    button = probe_button(False)
    if button == BUTTON_CONNECT:
        sustain += 10
        oled_print("Sustain: " + str(sustain))
        time.sleep(0.1)

    if button == BUTTON_REMOVE:
        sustain -= 10 
        oled_print("Sustain: " + str(sustain))
        time.sleep(0.1)
