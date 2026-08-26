# Guides Simplification 


Let's consider a less intrusive option for Guides, where we just label nodes on the part but don't go into a full blocking App that forces us through it, like allow the user to actually wire it themselves with th normal Jumperless system. the idea is these guides are more about getting comfortable with using the Jumperless. 


I think that means making a lot of this more of an integration with normal system, rather than an app that the user goes "into". I think these should be JumperlOS services that always run in the background. for the labeling, this can be a logical extension of Highlighting (we can give it a new file in the same folder) 

We should scrape the web for a database of (relatively common) parts (the entire 7400 / 4000 series ICs, analog ICs, adafruit / sparkfun modules, Axiometa modules, dev boards, buttons, swiches, pots, LEDs, rotary encoders, hall effect sensors, etc)  which we keep on the filesystem in a memory efficient way (a lot of the things will have the same pinouts, so our system should let us store multiple part names under one pinout definition. ) 

We should have a top level menu item Parts, then the menutree should look like (note \31 is a linebreak on the breadboard LEDs but not on the OLED, check if that works) 

Parts
-Auto\31Detect
-Clear
--All
--Pick
-Displays
--OLED
---SSD1309
---SSD1315
---SH1106
--LCD
---ST7789
---ILI9341
---ST7789
---HD44780
---KS0108
---SBN1661
---40RGBX160 {be sure to do this one because I have one and want to see it work)
--MIP {https://github.com/Gbertaz/JDI_MIP_Display}
---LS027B7DH01
---LS044Q7DH01
---LPM013M126C
---LPM013M091A
---LPM009M360A
--LED
---Direct
----Matrix
-----5 x 7
-----8 x 8
-----16 x 16
----$Segment$
-----*7**8**14**16**
------$Digits$
-------*1**2**3**4**5**6**7**8**
---Driver
----IS31FL373
----HDSP-xxxx
----HPDx-xxxx
----HCMS-xxxx
---Addressable
----WS2812
----SK6812
----SK9822

--Custom
{just let the user label pins, making it easy to skip and go to the next (so we don't even need to ask SPI or I2C, buts give premade labels for both, then we can infer the comms from that and automatically cycle through the drivers we know and see which one works. After labeling, we should have an optional `Detect\31Driver?` remove button exits and moves along, clickwheel cycles through each driver sending `See this? Click to confirm`, and the encoder button or Connect button confirms )
-Logic
--7400\31Series
---{list the ~30 most common}
--4000\31Series
---{list the ~30 most common}
--Other
---{common parts that don't fit 7400/4000 series}
--Custom
---{similar to above, let users cycle through all the labels in all the logic parts, VCC, GND, THR, OUT, CLK, etc. with the connect remove buttons (next/prev) then assign the label when a user taps a row, we should also allow undo/redo with a fast double clicks (don't persist this in the file though)}
-Analog
--OpAmp
--Clock
--Audio
--Power
--Other
--Custom
---{same thing, cycle through common labels and allow users to type in a label in the terminal}
-Discrete {this should dovetail in with the part ID / tester}
--Resistor
--Capacitor
--LED
--Inductor
--Diode
--Potentiometer
--Auto
-Transistors
--BJT
--MOSFET
--Other
--Auto

We might want to have this part of the menu tree generated from the file with parts definitions so we're not lying to users about what's supported.

We should think about the persistence of these things, like does clearing the board also clear the parts? I think it should. especially if our autodetection is good.


We should be able to drive these display and other parts as a service in the main loop, so we can have a display on the breadboard show animations and stuff even while doing other things like probing or navigating menus.


We should focus on making this all feel effortless to the user, and if they don't want to use it, it shouldn't change the behavior (except the auto id / testing, but that's purely informational) 

This might be a lot of memory to have libraries for a ton of displays and stuff, we should see what we can do to make it not use much runtime heap and keep stuff in our roomy flash. We'll need a coherent system to define parts and tell the jumperless how to interact with them.



/Users/kevinsanto/Documents/GitHub/JumperlOS/CodeDocs/DESIGN_PART_ID_FOLLOWUP.md

---

## STATUS + answers (2026-08-24, first slice landed on dev)

Everything above the line is the original brainstorm; this section records
what shipped and how the open questions resolved. Plan of record:
~/.claude/plans/jumperlos-codedocs-guidessimplification-velvet-bird.md.

**Landed (dev, all green V5+OG):** the blocking Guides app is GONE - opening
a project applies power, labels parts, arms a non-blocking step viewer, and
returns to idle. PartLabels service (one auto-hiding edge LED per pin,
tap-to-inspect, warn-never-block pin-class warnings). StepViewer (wheel
browses steps ONLY while they're on the OLED; click-and-hold exits).
Flash parts DB (data/partdb/*.yaml -> generator -> rodata; 112 records,
many-names->one-pinout AND ->one-driver, zero heap). Top-level **Parts**
menu row -> picker app (class -> part -> tap-row-for-pin-1 -> labels bloom
-> app exits; Clear row when parts exist). DisplayService (a placed
SSD1306/SH1106 routes its own SDA/SCL and animates when you wire power;
soft-I2C when the onboard OLED owns I2C1). Background MicroPython runner
(jumperless.bg_start(cb) - callback ticks in the service loop, pauses in
probe mode/menus). `driver:` round-trip + partdbResolveDriver.

**Answers to the questions above:**
- `\31` on breadboard LEDs: it is ZERO-WIDTH there (printString skips it
  without advancing); the two-line break is positional (7 glyphs/half). The
  OLED converts it to '\n'. Convention: pad the first segment to exactly 7
  glyphs, then `\31` - the DB generator enforces it (menuName validation).
- Static menuTree subtree: NOT used - menuLines[150] has ~28 free lines and
  deep submenus resolve to their depth-1 parent. The Parts row is childless
  and opens a picker app rendering from DB rodata, so the menu can never
  lie about what's supported. A generated static tree stays deferred.
- Persistence: yes - parts.clear() runs inside JumperlessState::clear(), so
  clearing the board clears parts. (Already true before the slice.)
- Undo/redo "fast double clicks": satisfied by the existing global probe
  double-tap undo; the encoder has NO double-click gesture (standing rule).
- "Scrape the web": no - datasheet-derived facts only, clean-room, per the
  Part-ID licensing ruling. Never import Fritzing/KiCad/AVR-tester tables.
- Heap: DB + display init tables live in flash (XIP rodata, ~23 KB); heap
  cost is ~1.3 KB only while a display is attached.

**Still open:** auto-detect (B-M6), user /partdb/ files (B-M7),
Detect-Driver UX (B-M8/C-M2), JDI MIP + 40RGBX160 + color TFT drivers,
MP display surface (C-M5/D-M2), HIL rewrite (A-M7), and Kevin's editorial
pass over the seed DB's `# VERIFY` markers.
