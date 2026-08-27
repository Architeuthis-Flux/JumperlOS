#ifndef MENUTREE_H

#define MENUTREE_H
#include <Arduino.h>




// IMPORTANT: this array MUST be declared with an explicit size matching the
// companion arrays in Menus.cpp (menuLevels[150], stayOnTop[150], actions[150],
// ...). It used to be sized by its initializer (~117 entries), so any code
// indexing up to 150 walked off the end — straight into menuParsed/menuPosition/
// menuTreeFile, reinterpreting them as String objects (heap corruption, then a
// hard fault in free()). The unused tail is value-initialized to empty Strings.
String menuLines[150] = {
  "$Rails$",
  "-*Both* *Top* *Bottom*",
  "-->v1",

  "$Connect$",
  "-*Add**Remove**",
  // "-->c2",
  


  "Output",
  "-Limits",
  "--$Min Max$",
  "---*0 _ 3V3**0 _ +5V** ~ 5V** ~ 8V*",
  "-$GPIO$",
  "--*1**2**3**4**5**6**7**8*",
  "--->n1",
  "-$UART$",
  "--*Tx* *Rx*",
  "---Nodes>n2",
  //   "----*USB 2*  *Print*",
  //   "-----*9600* *115200*",
    // "-$Buffer$",
    // "--*In* *Out*",
    // "--->n2",
    // "--DigitalOptions",
    // "---Output",
    // "----*USB 2*  *Print*",
    // "---$UART$",
    // "----$Baud$",
    // "-----*9600**19200**57600**115200*",
    // "---$I2C$",
    // "----$Speed$",
    // "-----*100 K**400 K**1   M**3.4 M*",
    "-$Voltage$",
    "--$DAC$",
    "--*DAC 0**DAC 1**Top R '**Bot R ,*",
    "--->v1",
    "---->n1",

    // "--$*Min**Max*$",
    // "--->v2",


    "Show",
    "-$Digital$",
    "--$GPIO$",
    "---*1**2**3**4**5**6**7**8*",
    "---->n1",
    "-$UART$",
    "--*Tx* *Rx*",
    "--->n2",
    "-$I2C$",
    "--*SDA* *SCL*",
    "--->n2",
    "-$Current$",
    "--*Pos* *Neg*",
    "--->n2",
    "-$Voltage$",
    "--*0**1**2**3**4*",
    "--->n1",
    //   "-$Options$",
    //   "--Analog Display",
    //   "---$Type$",
 
 
 

  // Top-level, childless: selecting it walks off the end of its (empty)
  // submenu range into doMenuAction, the same way Files and History do, and
  // getActionCategory()'s "Parts" branch sends it to APPSACTION ->
  // runApp(-1, "Parts"). Placed at the head of the "things you run"
  // cluster, after the wiring tools (Rails/Connect/Output/Show).
  //
  // The row was "Projects" through wave 2 and "Guides" through the first
  // ambient slice; Kevin retired Guides as a menu-level concept at the
  // 2026-08-24 bench pass ("let's remove guides as a menu item and work on
  // parts") - projects stay reachable via `z` and the Files browser. This
  // string and the apps[] row { "Parts", ... } are ONE unit with the
  // getActionCategory branch: "Parts" -> APPSACTION with the CHILD line as
  // runApp's name arg (previousMenuPositions[1] - the 2-deep dispatch shape).
  // The child text and the apps[] row name are ONE unit after
  // normalizeSpaces ("-Place  \31Part" -> "Place Part").
  "Parts",
  "-Place  \31Part",
  "-Test   \31Part",

  "Apps",
  "-Bounce \31Startup",
  "-Snake",
  "-Calib  \31DACs",
  "-Custom \31App",
  // "-XLSX   GUI",
  //"-Micropython",
  "-uPython\31REPL",
  // "-File   \31Manager",
  "-Probe  \31Calib",
  "-Switch \31Calib",

  "-JDI MIP\31display",
  "-DMX \31Serial",  
  "-OLED \31Images",


  //"-Show   Image",
 

 //   "-Oscill oscope",
 //   "-MIDI   Synth", 
 //   "-I2C    Scanner",
 //   "-Self   Dstruct",
 //   "-EEPROM Dumper",
 //   "-7 Seg  Mapper",
 //   "-Rick   Roll",
 //   "-$Circuts>$",
 //   "--555",
 //   "--Op Amp",
 //   "--$7400$",
 //   "---*74x109**74x161**74x42**74x595*",
  //  "-$Games  >$",
  //  "--*DOOM*", //*Pong**Tetris**Snake*",
   //"-$Manage >$",
 //   "--Delete",
 //   "--->a3",
 //   "--Upload",
 //   "--->a4",

   "-Scan",
   "-I2C    \31Scan",

  
   "Calib  ration",
  "-Probe  \31Pads",
  "-Switch \31Thresh",
  "-DACs   \31Calib",
  "-Full   \31Test",
  "-Probe  \31Cable",
  "-Xbar   \31Route",
  "-Tip    \31Voltage",
  "-PSRAM  \31Check",


  "Files",
  //auto populate from python_scripts and examples directory
  
   "Slots",
   "-$Load$",
   "--*0**1**2**3**4**5**6**7*>s",
   "-$Clear$",
   "--*0**1**2**3**4**5**6**7*>s",
   "-$Save to$",
   "--*0**1**2**3**4**5**6**7*>s",

   "History",
  //  "Settings",
  //      "-Encoder Probing",
  //      "--*On**Off*",
  //      "-Hilight Clear",
  //      "--*On**Off*",
            


       "Display\31Options",
      //  "-$Demo$",
      //  "--*On**Off*",
       "-$Colors$",
       "--*Rainbow**Shuffle*",
       "-$Jumpers$",
       "--*Wires* *Lines*",
       "-$Bright$",
       "--$Menu$",
       "---*1**2**3**4**5**6**7**8*",
       "--$Special$",
       "---*1**2**3**4**5**6**7**8*",
       "--$Rails$",
       "---*1**2**3**4**5**6**7**8*",
       "--$Wires$",
       "---*1**2**3**4**5**6**7**8*",



      "OLED",

      "-Font",
      "--Eurostl^",
      "--Jokermn^",
      "--ComicSns^",
      "--Courier^",
      "--Science^",
      "--SciExt^",
      "--BerkMono^",
      "--Pragmtsm^",
      "--IosevkaR^",
      "-Display\31Size",
      "--$Width$",
      "---*32**64**128**256**Custom*",
      "---->i(16)(2048)",
      "--$Height$",
      "---*32**64**128**256**Custom*",
      "---->i(16)(2048)",
      "-$Pins$",
      "--*GPIO7/8**RP6/7**Intrnal*",
      "-$StartUp\31Messge$",
      "--*Text**Bitmap**Clear*",
      "--->t(32)",
      "-Show \31in Term",
      "--*On**Off*",
      //"-Dis    connect",

      "-Lock   \31Connect",
      "--*On**Off*",
      "-Connect\31On Boot",
      "--*On**Off*",

      "-Connect",

      //  "-Demo",


       "Routing\31Options",
       "-Stack",
       "--$Rails$",
       "---*0**1**2**3**4**Max *",
       "--$DACs$",
       "---*0**1**2**3**4**Max *",
       "--$Paths$",
       "---*0**1**2**3**4**Max *",
       "end"
  };


/*



char menuTree[] = {"\n\
$Rails$\n\
\n\
-*Both* *Top* *Bottom*\n\
-->v1\n\
\n\
Apps\n\
\n\
-Oscill oscope\n\
-MIDI   Synth\n\
-I2C    Scanner\n\
-Self   Dstruct\n\
-EEPROM Dumper\n\
-7 Seg  Mapper\n\
-Rick   Roll\n\
-$Circuts>$\n\
--555\n\
--Op Amp\n\
--$7400$\n\
---*74x109**74x161**74x42**74x595*\n\
-$Games  >$\n\
--*DOOM**Pong**Tetris**Snake*\n\
-$Manage >$\n\
--Delete\n\
--->a3\n\
--Upload\n\
--->a4\n\
Slots\n\
\n\
-$Load$\n\
--*0**1**2**3**4**5**6**7*>s\n\
\n\
-$Clear$\n\
--*0**1**2**3**4**5**6**7*>s\n\
\n\
-$Save to$\n\
--*0**1**2**3**4**5**6**7*>s\n\
\n\
Show\n\
-$Digital$\n\
\n\
--$GPIO$\n\
---*5V* *3.3V*\n\
----*0* *1* *2* *3*\n\
----->n4\n\
\n\
--$UART$\n\
---*Tx* *Rx*\n\
---->n2\n\
\n\
--$I2C$\n\
---*SDA* *SCL*\n\
---->n2\n\
\n\
-$Current$\n\
--*Pos* *Neg*\n\
--->n2\n\
\n\
-Options\n\
\n\
--Analog Display\n\
---$Type$\n\
----*Mid Out**Bot Up**Bright**Color* \n\
-----$Range$\n\
------>r\n\
---$Range$\n\
---->r\n\
\n\
--DigitalOptions\n\
---Output\n\
----*USB 2*  *Print*\n\
---$UART$\n\
----$Baud$\n\
-----*9600**19200**57600**115200*\n\
---$I2C$\n\
----$Speed$\n\
-----*100 K**400 K** 1  M**3.4 M*\n\
\n\
-$Voltage$\n\
--*0* *1* *2*\n\
--->n3\n\
\n\
Output\n\
\n\
-$GPIO$\n\
--*5V* *3.3V*\n\
---*0* *1* *2* *3*\n\
---->n4\n\
-$UART$\n\
--*Tx* *Rx*\n\
---Nodes>n2\n\
----*USB 2*  *Print*\n\
-----*9600* *115200*\n\
\n\
-$Buffer$\n\
--*In* *Out*\n\
--->n2\n\
\n\
--DigitalOptions\n\
---Output\n\
----*USB 2*  *Print*\n\
---$UART$\n\
----$Baud$\n\
-----*9600**19200**57600**115200*\n\
---$I2C$\n\
----$Speed$\n\
-----*100 K**400 K**1   M**3.4 M*\n\
\n\
-$Voltage$\n\
--$Range$\n\
--*5V* *~8V*\n\
--->v2\n\
---->n1\n\
\n\
DisplayOptions\n\
-$DEFCON$\n\
--*On**Off**Fuck*\n\
-$Colors$\n\
--*Rainbow**Shuffle*\n\
-$Jumpers$\n\
--*Wires* *Lines*\n\
-$Bright$\n\
--*1**2**3**4**5**6**7**8*\n\0"};

*/

#endif