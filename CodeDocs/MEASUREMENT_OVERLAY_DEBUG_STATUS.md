# Measurement Overlay - Status

Status doc for the **measurement overlay** feature (voltage overlay on row LEDs +
current taps). Everything builds; the voltage sweep AND current taps are
**verified working on hardware** (2026-07-18/19 debug sessions). Full 60-row
sweep takes ~50ms.

**2026-07-20 release refactor**: the whole feature set is now gated behind
`[experimental] dev_features` (default 0 - see "Experimental flag" below),
LED painters are arbitrated per-net (see "Render arbitration"), and the ants
paint AFTER row animations so they can't be stomped.

Both original bugs are FIXED:

- **Bug 1 (rows didn't read)**: root cause was rail/DAC stacking occupying chip K
  Y lanes. Fixed with a third resolver tier that routes through chips I/J.
- **Bug 2 (overlay stomped wire rendering)**: force-on now uses COLOR, netted-row
  bars only overdraw tip+center, and clears use a per-row painted-pixel mask.

Current user config: `measurement_overlay = 2` (BAR), `current_cycling = 1`
(cycling also defaults to 1 in `config.h` since 2026-07-19).

---

## What this feature is

Two sub-features, both V5-only (OG has stubs):

1. **Voltage overlay**: time-multiplex a free routable ADC (ADC0-3) across breadboard
   rows via the CH446Q crossbar, and render each row's voltage on its 5 row LEDs
   (COLOR = center-channel LED only / BAR = 1V per LED / DOT = tip only).
2. **Current taps** (redesigned 2026-07-20, NETLIST-BASED - the bridge array
   is the source of truth): user-designated bridges measured IN SERIES.
   Inserting a tap quietly removes the pair's bridge from the bridge array
   and adds two EPHEMERAL bridges (`ISENSE_PLUS->A`, `ISENSE_MINUS->B`),
   then does ONE normal reroute. The router wires the shunt with its full
   machinery (stacking included), the paths dump always shows the real
   crossbar state, and the entire existing manual-ISENSE stack drives
   itself for free: `rebuildShownReadings()` detects the plus/minus nets,
   Peripherals polls INA0 continuously into `currentSenseState`, and the
   marching-ants overlay + OLED current readouts just light up. One shunt
   multiplexes across up to 8 taps (400ms dwell, one batched incremental
   reroute per swap); a LONE tap stays inserted permanently. Taps are added
   by probing a bridged row in measure mode (OLED shows `A-B V` + `mA`) or
   `V+<a>-<b>`.

The old auto-cycler (round-robin one-shot break-windows over EVERY BBtoBB
path) is gone: it disturbed every circuit indiscriminately and its numbers
were misleading whenever a physical jumper or the net's own stacked lanes
bypassed the broken path (hardware session: meter read 6.3mA, window read
0.85mA because a real wire 17-28 shorted around the shunt).

## Experimental flag (2026-07-20)

`jumperlessConfig.experimental.dev_features` (config.h `[experimental]`
section, default 0) is the master gate. Wired through all 7 configManager.cpp
sites + `requiredSections[]` (so old config.txt files get the section added
via full rewrite). Gates:

- `overlayEffectiveMode()` returns OFF (kills the sweep, the paint, AND the
  measure-switch force-on).
- `serviceCurrentCycling()` enable check + `addCurrentTap()` (taps can't even
  be registered, so none sit at "holding" forever).
- `V` command and `:overlay*` backchannel print a one-line enable hint.
- Menu: `!` prefix on menuTree.h lines = experimental; `parseMenuFile()`
  strips the prefix when the flag is on, drops the line when off (parsed at
  boot - visibility changes need a reboot; the DISPLAYACTION handlers gate
  regardless). `writeMenuTree()` regenerates /MenuTree.txt from menuTree.h
  each boot, so stale menu files aren't an issue.

**Releasing = flip the default to 1 in config.h** (or delete the gates).
Nothing needs to be ripped out.

## Render arbitration (2026-07-20 - the ants-vs-animations fix)

Root cause of "animations compete with the ants": ants were painted inside
`drawWires()` (early), then `showAllRowAnimations()` hard-overwrote the same
pixels (the old ISENSE-yield early-return in `showRowAnimation` was commented
out). No pixel ownership existed - implicit call order decided everything.

Now (all in Graphics.cpp/h + main.cpp):

- **`netDecor[MAX_NETS]`** (`DECOR_NONE/ANIM/ANTS`) filled once per frame by
  `arbitrateNetDecorations()` (called from `showAllRowAnimations()`): sense
  nets (`currentSenseState.plus/minusNet` when connected) get ANTS, anything
  else with an assigned animation gets ANIM. `showRowAnimation(net)` yields
  ANTS nets.
- **Sticky tap claims** (anti-flash while multiplexing): the arbiter also
  calls `MeasurementOverlay::claimTapNets()`, which marks EVERY net touched
  by a registered tap (active or not) as ANTS while cycling is enabled.
  Without it, each mux swap flapped tapped nets between animation and
  ants. Inactive tapped nets show their plain net color; ants render only on
  the active pair. EXCEPTION: brightened/warning nets (probe highlight)
  that are NOT the live sense pair get their claim released back to ANIM -
  interactive feedback beats the anti-flap claim; the live pair's ants
  already do their own highlight boost. Hardware-verified: `:leds` frames
  during continuous two-tap rotation show zero animation frame colors on
  any tapped row.
- **OLED 3-line fix** (`oled.cpp clearPrintShow`): the
  OLED_SCALE_LINES_INDEPENDENTLY branch reset the font to
  `desiredPointSize`, throwing away the pt the width+height fit loop had
  converged on; per-line scaling only shrinks for WIDTH, so 3-line text
  rendered oversized and overlapped vertically on the 128x32 panel. Now it
  starts per-line scaling from the fitted `currentPt`. Verified via `:oled
  full` framebuffer dump: three cleanly separated lines. (2-line callers
  are unaffected - for them currentPt == desiredPointSize.)
- **Build/paint split**: `buildCurrentSenseOverlay()` (in `drawWires()`, needs
  the virtual path in wireStatus) caches the pixel lists;
  `paintCurrentSenseOverlay()` runs in `core2stuff()` AFTER
  `showAllRowAnimations()`, so ants sit above animations, below the voltage
  overlay + graphic overlays. A `currentSenseBuiltThisFrame` flag stops the
  paint from using stale lists in node-mode frames. The virtual wire node
  endpoints now survive until the next `addVirtualCurrentSensePath()` (they
  used to be cleared in remove, before the paint ran).
- **Voltage overlay exact clears**: `paintedMask` now covers netted rows too
  (a bar-tip pixel off the wire route used to linger forever when the bar
  shrank); `clearMaskedPixels()` skips pixels the wire renderer repainted
  this frame (`wireStatus[row][k] != 0`), so no one-frame black holes. The
  overlay also skips `currentSenseVirtualRow[]` rows so it can't stomp the
  ant bridge.
- Self-check: `V?` / `:overlay:check` now also verifies sense nets are
  claimed by ANTS (`netDecorSelfCheck()` in Graphics.cpp).

Frame paint order: `drawWires` (base colors + build ants) ->
`showAllRowAnimations` -> `paintCurrentSenseOverlay` ->
`measurementOverlayPaint` -> `renderGraphicOverlays`.

## Parallel current sensing: investigated and REJECTED (2026-07-20)

Idea: characterize path resistances, leave the user bridge in place, attach
the INA shunt in PARALLEL, compute total current via the current divider.
Don't re-derive; it's blocked twice over:

1. **Netlist blocker (hard)**: `ISENSE_PLUS`/`ISENSE_MINUS` are mutual
   do-not-intersect nets (nets 6/7, special nets in
   NetsToChipConnections.cpp). With the A-B bridge intact, both ISENSE ends
   land on ONE net -> DNI rejects the topology. Series displacement isn't an
   implementation choice; it's the only way the net model can give the INA
   two distinct nets. (All the currentSenseState UI also assumes split nets.)
2. **Physics**: the shunt is 2 ohms but its crossbar legs are ~86-172 ohms
   of crosspoints vs a stacked bridge at ~20-45 ohms effective -> only a
   small, poorly-known fraction of current takes the shunt, multiplied by an
   uncalibrated 43-ohm/crosspoint estimate. Physical jumpers are invisible
   to any model (the 6.3mA-vs-0.85mA lie from the old cycler, but permanent).

Raw-crosspoint workarounds reintroduce the fight-the-router design the
2026-07-20 netlist rewrite exists to kill. Series taps stay.

`pathResistanceOhms()` (43 ohms/crosspoint) was kept anyway and got callers:
stacking-aware `connectionResistanceOhms(a, b)` in States.cpp (parallel
combination over all stacked lanes), exposed to MicroPython as
`resistance_between(a, b)` / `path_resistance(idx)` (+ a `resistance` key in
`get_path_info()`), with `pythonStuff/resistance_calibration.py` for fitting
the per-crosspoint constant against a real multimeter.

## Where the code is

| What | Where |
|---|---|
| All overlay logic | `src/MeasureMode.cpp` (bottom half, after the `MeasurementOverlay` banner) + `src/MeasureMode.h` |
| Core-2 hooks | `src/main.cpp`: `measurementOverlayCore2Tick()` called in `core2stuff()` right after the menu-frame pacing check (mutex held); `measurementOverlayPaint()` called after `showAllRowAnimations()`, before `renderGraphicOverlays()` |
| Core-0 service | `MeasurementOverlay::service()` registered in `src/main.cpp` setup, **before** `probeButton`/`probing` so it can consume probe button presses first |
| Cache invalidation | `volatile uint32_t crossbarPathGeneration` in `src/CH446Q.cpp`, incremented at the end of `sendPaths()`; declared in `CH446Q.h` |
| Path resistance | `pathResistanceOhms(int pathIndex)` in `src/States.cpp` (43 ohms per crosspoint, counts valid `chip[j]/x[j]/y[j]` triples) |
| Config | `jumperlessConfig.display.measurement_overlay` (0 off/1 color/2 bar/3 dot) and `.current_cycling` (0/1) in `src/config.h`; wired through all configManager.cpp sites (parse x2, save, change-detect, in-place update, print, oldValue) |
| Menu | `src/menuTree.h` under "Display Options": `$Overlay$` (Off/Color/Bar/Dot) and `$I Cycle$` (Off/On/Clear); handler in `src/Menus.cpp` DISPLAYACTION branch |
| Terminal | `V` command in `src/SingleCharCommands.cpp` (`cmd_measurementOverlay`) |
| Port-7 backchannel | `:overlay` (V table), `:overlay:rows` (Vr), `:overlay:check` (V?) in `src/Ser3Backchannel.cpp` - added because bare `V` is shadowed by the ADC fast-query on that port and single-char dispatch never collects args |

## Debug tools

- **`V?`** / **`:overlay:check`** - lane self-check: verifies the hardcoded topology
  tables (`chipToKlaneX/IlaneX/JlaneX`, `rowToChipY()`, ISENSE X positions) against
  the LIVE `globalState.connections.chipStates[].xMap/yMap`. Prints PASS or each
  mismatch. Verified PASS on hardware.
- **`Vr`** / **`:overlay:rows`** - per-row sweep dump: one line per row with last
  voltage, status (none/ok/stale/float), net number, and the exact resolved path
  (`direct chip B x11 y3 -> K y1`, `tap K y2`, or
  `via chip A x0 y6 -> I -> K x13 bounce y7`).
- **`V`** / **`:overlay`** - current tap table (mA, age, MEASURING/holding).
- **`V+17-28`** add a tap on that pair; **`V+17`** tap row 17's first bridge;
  **`V-17`** remove the tap touching row 17.
- **`V!`** - toggle current cycling. **`Vc`** - clear overlay row set + taps.
- Port 7 `:leds` dumps the LED buffer - useful to verify paint without eyes on the
  board (that's how the paint orientation was confirmed).

## How the voltage sweep works (core 2)

`measurementOverlayCore2Tick()` in MeasureMode.cpp, `__not_in_flash_func`, runs while
`core_sync` is held. Self-throttled to one slice per 8ms, up to 4 rows per slice.

Per row, `getRowPath()` resolves and caches (invalidated when
`crossbarPathGeneration` bumps) through THREE tiers:

1. **Direct lane** (`ROWPATH_DIRECT`): row's chip A-H Y lane (`rowToChipY()`),
   that chip's dedicated X lane to chip K (`chipToKlaneX[] = {9,11,13,15,1,3,5,7}`),
   then chip K crosspoint at X(8+adc), Y = chip index. Requires the chip-side X
   lane free and chip K's Y lane for that chip empty (or the router already owns
   the exact lane for this row's net - `preExisting`).
2. **Net tap** (`ROWPATH_NET_TAP`): if the row's net already reaches chip K in some
   routed path, close only ADC_X <-> that path's K Y. This is how rows 29/30/59/60
   (which sit on chips K/L X0/X1) get read when routed.
3. **I/J bounce** (`ROWPATH_IJ`) - **the Bug 1 fix**: row -> chip's I (or J) wire
   (`chipToIlaneX/JlaneX`) -> I/J's K wire (I/J X14 <-> K X13/X14) -> any chip K
   Y lane idle on BOTH ends (`pickFreeKBounceLane()`: no K-side bits AND the
   owning bb chip's K-wire X unused) -> ADC X. Four fresh crosspoints, nothing
   shared with routed nets, so nothing glitches.

Then `readRowVoltageViaPath()`: connect hops (only ones not already closed), for
un-netted rows pulse chip K X15 (GND) onto the ADC's K Y lane for 20us **before**
the row is connected to bleed held charge, then 80us settle, `readAdcVoltage(adc, 2)`,
disconnect only what we connected. Tried driving the RP2350B ADC pin low as SIO
instead (faster, no extra strobes) — the front-end op-amp saturates and subsequent
rows trail a GND reading, so stay on the crossbar GND pulse.

Cadence: one slice per 4ms, 5 rows per slice, slice CPU cost ~850us -> full
60-row sweep in ~50ms (hardware-measured; Vr prints live timing).

Status: `FLOATING` if not-in-net and |V| < 0.2V (renders dark), else `FRESH`.
Unreadable rows demote `FRESH` -> `STALE` (hold value, dimmed).

### Stability layer (2026-07-19, "err toward not updating")

User-reported: glitchy readings, and after the board sits powered for a long
time "most rows read ~3V" (CH446Q off-leakage slowly charges floating rows to
a solid-looking voltage; the high-Z read faithfully reports it). Defenses, all
in `measurementOverlayCore2Tick()` / `readRowVoltageViaPath()`:

- **Phantom-charge recheck** (`phantomRecheck()`): un-netted row reading over
  the deadband -> ground the ROW (chip K X15) for 20us, re-read after 80us. A
  real source recovers (hardware-verified: an externally wired 3.3V row
  re-reads full value at the first sample); held leakage charge stays
  collapsed -> row renders dark. Skipped while the reading matches the
  already-confirmed value; re-confirmed every 32nd sweep (~1.6s) so removed
  sources decay instead of holding forever.
- **Jump debounce**: a reading > 0.5V away from the published value is held
  back until the NEXT sweep reproduces it (within 0.5V). Single-read glitches
  never render; real changes lag one sweep (~50ms).
- **Route settle**: sweep pauses 100ms after any `crossbarPathGeneration`
  bump (LEDs hold last values).
- **Absurd-value drop**: non-finite or |V| > 10V reads are treated as failed
  (row goes STALE, holds).
- **Noise hold**: FRESH row within 0.05V of the published value doesn't
  update at all.
- **BAR rounding**: LED count is round-to-nearest-volt (was ceil-0.1), so the
  ~0.45V noise seen on idle netted rows can't flicker LED 1.

ADC choice (`pickFreeAdc()`): first of ADC0-3 that isn't FakeGPIO TDM's
(`tdmInputs.adcChannel`), isn't in the bridge array (`isAdcInUseByOtherConnections`),
and whose chip K X lane (X8-11) is idle in `lastChipXY`.

**Force-on**: `overlayEffectiveMode()` returns COLOR (center LED only - was BAR
during debugging) whenever the probe switch is in the measure position
(`switchPosition == 0`), even with the config off. Config mode wins when nonzero.

## BUG 1 post-mortem: why rows didn't read

The router's rail/DAC **stacking** eats chip K Y lanes: with probe power on and one
rail routed, the observed state was DAC0 stacked across K y0/y1/y5 and TOP_RAIL
across y2/y3/y4/y6 - every K lane occupied except y7. The old resolver's `kSideOk`
correctly refused to tap an occupied lane (it would short the row into DAC0/rail),
so fresh direct taps were only possible on chip H. Which chips die depends on which
lanes stacking picks - that session it looked like "top rows don't read".

Verified on hardware after the tier-3 fix: all rows 1-28/31-58 read (`via ...`
paths), values track DAC changes within ~3% (2.86V read for 3.0V set through the
4-crosspoint chain, consistent with ADC cal + chain resistance).

**Known ceiling** (documented in `resolveRowPath`): if stacking ever occupies every
K lane on BOTH ends (K-side bits AND rows hanging on every bb chip's K wire),
tier 3 has no bounce lane and those rows go stale. Upgrade path: reserve one K
lane from the router's stacking.

## BUG 2 post-mortem: overlay vs wire rendering

Three changes in `measurementOverlayPaint()` / `overlayEffectiveMode()`:

- Force-on (measure switch position) uses **COLOR** instead of BAR - one LED per
  row leaves wires legible. BAR/DOT are opt-in via config/menu.
- In BAR mode, **netted** rows only overdraw the bar tip + the center anchor LED;
  the in-between fill LEDs keep the wire color. Un-netted rows still get the full
  bar (nothing else paints them).
- Clearing uses a per-row **painted-pixel mask** (which LEDs the overlay lit last
  frame) instead of wiping all 5 LEDs of un-netted rows - row animations and other
  painters coexist with the overlay now. Turn-off clears exactly the masked pixels.

Paint orientation (confirmed via `:leds` dumps with known voltages): physical
`base+0` is the top of every 5-LED column; center-channel LED = `base+4` for rows
1-30, `base+0` for rows 31-60. The wire renderer's bottom-flip
(`Graphics.cpp rowColumnToPixelIndex`) is consistent with this.

## Current taps (core 0) - hardware-verified working (2026-07-20 netlist design)

`MeasurementOverlay::serviceCurrentCycling()` runs in the jOS service (HIGH, core
0). No raw crosspoints and no core_sync: everything is normal netlist edits +
`refreshLocalConnections()` (core 0 only). Gates: config on
(`current_cycling`), no manual ISENSE bridges (`manualIsenseWiring()` - any
ISENSE bridge that isn't the active tap's own ephemeral pair; if one appears
mid-tap the mux restores and backs off), no wavegen/probe/pending send/LA,
`refreshLocalInProgress` false.

**Insert** (`insertTap`): capture the bridge's dup count + color, add
ephemeral `ISENSE_PLUS->A` and `ISENSE_MINUS->B` (no routing yet), QUIETLY
remove the A-B bridge (`removeConnection(..., quiet=true)` - new flag, no
markDirty/undo), then ONE `refreshLocalConnections(1)`. The router does all
lane work. `restoreTap` is the exact inverse (quiet re-add + color restore).
The displaced bridge is remembered in `displacedBridge` and injected into
YAML saves by `serializeBridges()` via `overlayTapDisplacedBridge()` (a save
mid-tap must not lose the user's wire; the ephemeral ISENSE bridges are
already excluded from saves by the ephemeral filter).

**Reading**: the routed ISENSE bridges make `rebuildShownReadings()` set
`currentSenseState.plus/minusNet`, so Peripherals' continuous INA0 polling
fills `currentSenseState` exactly as with hand-wired ISENSE; `readActiveTap()`
just copies `current_mA` into the tap. No INA I2C in the mux at all.

**Why netlist instead of raw crosspoints** (2026-07-20 rewrite): the raw
version fought the router - stacking bypassed the broken bridge (needed an
ever-growing "cut" heuristic), the paths/bridge dump lied about the crossbar
state, and none of the currentSenseState-driven UI (marching ants, OLED
readouts) worked. Netlist displacement fixes all of that by construction,
and reads HIGHER (8.3mA vs 3.8mA on the same test loop) because the router
routes the shunt with stacking instead of 4 fixed crosspoints.

**Rotation**: >1 tap -> 400ms dwell. A 150ms dwell was tried and REVERTED
(2026-07-20): the reroute-per-150ms storm broke routing and display behavior
in practice (each swap is a full incremental reroute racing user edits,
redrawing LEDs, and re-arming the sweep's 100ms settle hold). Faster
multiplexing needs a swap that doesn't reroute, not a smaller constant
(see the TAP_DWELL_MS comment in MeasureMode.h). A swap batches the restore + insert
netlist edits and does ONE incremental reroute (`restoreTap(i, false)` +
`insertTap(next, false)` + one `refreshLocalConnections`). Hardware-measured
disturbance on a live DAC-fed net during continuous swapping: baseline
two-reroute/2s version showed ~120mV dips around each swap; the batched
version showed ZERO dropouts over 12k samples/6s (min 2.84V on a ~2.9V
node) - this was the "DAC output on the probe tip glitches" complaint.
`readActiveTap()` drops readings whose `lastUpdatedMs` predates the insert
(`tapInsertedMs`), so a fast swap can't credit the previous tap's current to
the new one (the INA poll is 50ms). A lone tap stays inserted forever.
External netlist wipes (slot load, `nodes_clear`, undo) are detected by the
active tap's ephemeral bridges vanishing -> drop `displacedBridge` WITHOUT
restoring (the loaded state already contains the user bridge) and invalidate
the tap. Inactive taps whose bridge disappears are dropped each service pass.

**Measure-mode UX**: probing a row that's one end of a bridge (in
`MeasureMode::startMeasurement`) auto-adds a tap on that bridge; the OLED
shows three lines - `A-B` / `<V> V` / `<mA> mA` (voltage from the probe's
own ADC ephemeral, current from the tap; clearPrintShow auto-scales the
3-line block to the display). The probe's REMOVE button while measuring a tapped
row deletes the tap (checked before the overlay row-set REMOVE in
`handleRowSetButtons`).

**Marching ants + voltage coloring** (2026-07-20): because taps ARE
manual-ISENSE wiring as far as Graphics.cpp is concerned, the existing
`renderCurrentSenseOverlay()` marching-ants animation (speed/direction from
`filteredCurrent_mA`, ants layered over the existing net colors, virtual
wire between the split nets) runs for taps with no changes. One change was
made: the virtual wire's background color now shows the VOLTAGE
(`measurementOverlay.rowVolts[virtualWireNode1]` via `measurementToColor`)
when the sweep has a live reading for that row, falling back to the old
current-derived color otherwise. Hardware-verified via `:leds` frame diffs:
ant pixel marches across rows 45-55, voltage-tinted background on the
virtual wire, existing net colors intact elsewhere.

**Known limits**:
- A physical jumper across the tapped pair bypasses the shunt; the tap then
  correctly reads only the crossbar leg (the 17-28 LED session: meter 6.3mA,
  crossbar leg ~1mA). Remove the physical wire or tap a different bridge.
- The shunt path still adds series resistance vs the direct bridge, so
  low-impedance loops carry somewhat less current while tapped (kilo-ohm+
  loads see proportionally less error).
- While tapped, the pair's net is SPLIT into an I Sense + net and an I Sense
  - net (visible in the nets list; the ants' virtual wire visually bridges
  them). `disconnect(A,B)` while tapped won't find the bridge - remove the
  tap first (V-<row> or probe REMOVE).
- Tap endpoints can be anything the router can reach ISENSE to (rows, nano,
  rails) - no longer limited to chips A-H.

## Verified facts (don't re-derive)

- **2026-07-20 refactor hardware-verified** (flashed, tested over ports 1/5/7):
  - Flag off: `:overlay` prints the enable hint; flag on (`` `[experimental]
    dev_features = 1; `` on port 1): full table returns. Config full-rewrite
    added the `[experimental]` section to an old config.txt automatically.
  - Ants survive animations: with a DAC0->17, 17-28 tap (3.05mA MEASURING),
    GND, and GPIO_1 sharing row 17's net (a known stomper), `:leds` frame
    diffs show the ant pixel (0x22251e) marching across rows 17/28 and down
    the virtual-wire column (rows 19-27) over a dim background - previously
    animations wiped these.
  - `V?` self-check PASS including the new netDecor arbiter check.
  - Disabling the flag mid-tap restores the user bridge (17-28 stayed
    CONNECTED).
  - `resistance_between(3, 9)` = 34.4 ohms vs 86 for the single parent lane
    (= 86||86||172, stacking-aware as designed; string node names work);
    `get_path_info()` now carries per-lane `resistance`.

- Lane self-check (`V?`/`:overlay:check`) PASSES on hardware: `rowToChipY()`,
  `chipToKlaneX/IlaneX/JlaneX`, ISENSE X positions all match the live chipStates.
- I/J topology: bb chip I-lanes `{0,2,4,6,8,10,12,14}`, J-lanes `{1,3,5,7,9,11,13,15}`,
  I/J X14 = K wire, K X13 = I wire, K X14 = J wire (all in `rev5plusXmap`).
- Paint orientation confirmed via LED dumps (see Bug 2 section).
- Both `jumperless_v5` and `jumperless_og` build clean
  (`~/.platformio/penv/bin/pio run -e jumperless_v5` - plain `pio` on this machine
  runs Python 3.14 which the platform rejects).
- Registration order matters: `measurementOverlay` before `probeButton`/`probing`
  in main.cpp so `getButtonPress(true)` consumes CONNECT/REMOVE for row-set edits
  during active measure-mode measurement.
- Rows 29/30/59/60 sit on chips K/L X0/X1 and are net-tap only (ponytail ceiling).

## Remaining work

1. **Visual check** of BAR mode by a human (LED dumps look right; nobody has
   eyeballed the netted-row tip+center behavior in person). Same for the
   post-arbitration ants (should now survive GPIO/highlight animations).
2. Optional: reserve one chip K Y lane from rail stacking to remove the tier-3
   ceiling.
3. Optional: taps for nano-header/rail endpoints (needs non-A-H ISENSE lanes).
4. **Release promotion**: when the experimental period ends, flip
   `dev_features = 1` in config.h (or delete the gates).

## Gotchas for the next agent

- Anything touching the crossbar must hold `core_sync` (core-2 tick already does via
  `core2stuff()`; the core-0 tap mux acquires it explicitly).
- Stale manual `ISENSE_PLUS/MINUS` bridges left in the netlist silently pause
  the tap mux (by design - manual wiring wins). The V table prints a NOTE
  when this is the case. This bit a debug session: leftover ISENSE bridges
  from an earlier experiment made all taps sit at "holding" forever.
- `sendXYraw()` updates `lastChipXY` itself; never update the bitfield manually
  around it or the availability checks double-count.
- Chip K X4/5/6/7/15 are voltage sources - `sendXYraw` auto-disconnects conflicting
  sources on the same Y (see CHIP_K_VOLTAGE_SAFETY.md). The overlay only ever
  touches X8-11 (ADCs) and X13/X14 (I/J wires).
- The GND discharge pulse (chip K X15, before the row connects) exists because
  the high-impedance ADC lane holds the previous row's voltage and floating rows
  inherit it ("reads the next row" symptom). Fires for un-netted rows on fresh
  lanes (direct and IJ). Do NOT drive the MCU ADC pin as GPIO to discharge -
  the front-end op-amp saturates and trails GND on later reads.
- MeasureMode (the existing single-row service) creates an ephemeral ADC bridge for
  the measured row; `pickFreeAdc()` skips it via the bridge scan, no conflict.
- The port-1 terminal is usually held by the user's `jumperless` CLI app; drive
  debugging through the MicroPython REPL (port 5) + the port-7 backchannel instead.
  `print_nets()` over the raw REPL can hang the transport - avoid it there.
- Plan file with all interview decisions:
  `~/.cursor/plans/measurement_overlay_service_4df9a838.plan.md`.
