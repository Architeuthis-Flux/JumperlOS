#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ambient parts layer, end to end over the machine grammar (A-M7 skeleton).

The blocking guide runner is gone (test_guide_flow.py retired with it); this
suite drives its ambient replacement through the serial twins and asserts
the new grammar:

  PARTS n=            project open reports the parts table
  VIEWER steps=/step= the non-blocking step viewer (z steps twins)
  VIEWER off          the viewer's serial off-ramp
  PARTPIN row=        tap-to-inspect (probe_tap injection)
  PARTWARN            pin-class warnings (warn-never-block; certainties only)
  CHECK start=/result= on-demand electrical check (z check)

Fixture: a throwaway project whose wiring carries ONE dip8 part with a
deliberate vcc_to_gnd contradiction (its power-class pin connects to GND) -
the netlist-certain warning class, independent of rail setpoints. Steps are
SYNTHESIZED from parts: (no guide: section needed - ProjectsApp.cpp
wiringHasGuideSections accepts either).

Caveats this skeleton is honest about:
- PARTPIN listens in probe SELECT mode only (PartLabels gates on
  switchPosition == 1). A rig whose probe switch sits elsewhere skips that
  check with a note instead of failing the suite.
- The picker/placement UI (PARTPICK / PARTDB place) is encoder-driven and
  stays bench-only until an injection twin exists for the wheel.

Bench convention: snapshot board state + active context up front, restore
in a finally (an uncaught exception must not strand the bench)."""

import time

from jl import (jl_exec, port1_command, check, finish,
                board_state_capture, board_state_restore,
                active_context, restore_context)

TMP_PROJ = "/projects/ambtest"
TMP_WIRING = TMP_PROJ + "/wiring.yaml"

WIRING = """meta:
  name: ambient HIL
  summary: ambient-parts fixture
parts:
  - name: "U1"
    type: ic
    footprint: dip8
    row: 35
    placed: true
    pins:
      VCC: {pin: 8, connect: GND, class: power}
      GND: {pin: 4, class: gnd}
      OUT: {pin: 3, class: signal}
"""


state = board_state_capture()
ctx = active_context()
try:
    # ---- fixture --------------------------------------------------------
    jl_exec("import os\n"
            "try: os.mkdir('/projects')\n"
            "except OSError: pass\n"
            "try: os.mkdir('%s')\n"
            "except OSError: pass\n" % TMP_PROJ)
    jl_exec("f = open('%s', 'w')\nf.write('''%s''')\nf.close()" %
            (TMP_WIRING, WIRING))

    # ---- phase 1: ambient open ------------------------------------------
    resp = port1_command("z ambtest noscript", collect_seconds=8)
    check("PARTS n=1" in resp, "ambient open reports the parts table (PARTS n=1)")
    check("VIEWER steps=" in resp, "viewer armed from synthesized steps (VIEWER steps=)")

    # ---- phase 2: PARTWARN (vcc_to_gnd is netlist-certain) ---------------
    # The fold changed at load; PartLabels evaluates at 50 Hz. Give it a
    # moment, then ask for anything buffered.
    time.sleep(2.0)
    resp = port1_command("m")   # innocuous - flushes buffered async lines
    check("PARTWARN" in resp and "vcc_to_gnd" in resp,
          "power-class pin on a GND net raises PARTWARN vcc_to_gnd")

    # ---- phase 3: z steps serial twins -----------------------------------
    resp = port1_command("z steps")
    check("VIEWER step=1/" in resp, "bare z steps re-announces the cursor")
    resp = port1_command("z steps next")
    check("VIEWER step=" in resp, "z steps next moves and announces")
    resp = port1_command("z steps off")
    check("VIEWER off" in resp, "z steps off is the serial off-ramp")
    resp = port1_command("z steps on")
    check("VIEWER steps=" in resp, "z steps on re-arms from guideSource")

    # ---- phase 4: tap-to-inspect (switch-position dependent) -------------
    jl_exec("import jumperless\njumperless.probe_tap(35)")
    time.sleep(1.0)
    resp = port1_command("m")
    if "PARTPIN" in resp:
        check("row=35" in resp and "part=U1" in resp,
              "probe_tap(35) inspects U1 pin 1 (PARTPIN row=35 part=U1)")
    else:
        print("  (note: no PARTPIN - probe switch not in SELECT on this rig; "
              "inspect path unverified, not failed)")

    # ---- phase 5: z check grammar smoke ----------------------------------
    # Unpowered IC -> presence-hint check; the grammar is what's under test,
    # not the electrical verdict.
    resp = port1_command("z check U1", collect_seconds=25)
    check("CHECK start=" in resp, "z check announces (CHECK start=)")
    # the concluding line is "CHECK part=<label> result=..." - the label sits
    # between the two words (the skeleton's needle never matched anything)
    check("result=" in resp, "z check concludes (CHECK ... result=)")

finally:
    port1_command("z steps off")
    jl_exec("import os\n"
            "try: os.remove('%s')\n"
            "except OSError: pass\n"
            "try: os.rmdir('%s')\n"
            "except OSError: pass\n" % (TMP_WIRING, TMP_PROJ))
    restore_context(*ctx)
    board_state_restore(state)

finish("test_ambient_parts")
