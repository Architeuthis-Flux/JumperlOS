"""
USB Audio Microphone Demo
Stream two ADC channels to your computer as a 2ch / 48kHz USB microphone.

Hardware Setup:
1. Connect your left signal to breadboard row 20  (routed to ADC0)
2. Connect your right signal to breadboard row 40 (routed to ADC1)
3. Input range is +/-8V. Signals past full_scale clip.

No host software needed - the Jumperless shows up as a real input device, so
QuickTime, Audacity, a DAW, or any browser can record straight from a circuit.

HOW IT WORKS - two separate layers:
  usb_audio_setup()  makes the audio device VISIBLE. It rewrites the USB
                     descriptor and re-enumerates, so THIS SERIAL PORT DROPS
                     and comes back with the same name a couple of seconds
                     later. Run the script, wait, run it again.
  the host           starts and stops CAPTURE by opening or closing the input
                     device. Nothing in Python pumps samples - a C-side DMA
                     engine does, because MicroPython could never keep up with
                     48kHz.

While the host is recording, ADC0/ADC1 belong to the audio engine. adc_get()
still works on those two (served from the capture buffer), but the probe and
the other ADC channels are paused until recording stops.

API Demonstrations:
- usb_audio_is_enabled() - is the audio device currently advertised?
- usb_audio_setup(left, right, full_scale, dc_block) - configure and show it
- usb_audio_active() - is the host actually recording right now?
- usb_audio_status() - channels, rate, frames sent, dropouts
- usb_audio_teardown() - hide the audio device again
- connect() - route breadboard rows to the ADC channels
"""

import time

print("USB Audio Microphone Demo")

if not usb_audio_is_enabled():
    print("")
    print("Audio device is not enabled yet.")
    print("Enabling it now - THIS PORT WILL DROP and reconnect in ~2 seconds.")
    print("Wait for it to come back, then run this script again.")
    time.sleep(1)
    usb_audio_setup(0, 1, 8.0, True)
    raise SystemExit

# Route two breadboard rows to the stereo pair.
disconnect(20, -1)
disconnect(40, -1)
disconnect(ADC0, -1)
disconnect(ADC1, -1)
connect(ADC0, 20)
connect(ADC1, 40)
print("ADC0 -> row 20 (left), ADC1 -> row 40 (right)")
print("")
print("Pick 'JL Audio In' as an input device on your computer, then record.")
print("Ctrl+Q to stop.")

oled_print("USB Mic Live")

was_active = False
try:
    while True:
        st = usb_audio_status()
        active = st['streaming']

        if active != was_active:
            was_active = active
            oled_print("Recording" if active else "USB Mic Idle")

        line = "REC  " if active else "idle "
        line += "L=" + str(round(adc_get(0), 2)) + "V "
        line += "R=" + str(round(adc_get(1), 2)) + "V"
        drops = st['fifo_overflow'] + st['adc_overrun']
        if drops:
            line += "  drops=" + str(drops)

        print("\r                                                    ", end="\r")
        print(line, end="")
        time.sleep(0.25)
finally:
    print("")
    print("Stopped. The audio device is still advertised - run")
    print("usb_audio_teardown() to hide it again.")
