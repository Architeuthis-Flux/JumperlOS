# Multiplexed ADCs: ADC_n becomes a virtual, auto-assigned slot

## Context

Kevin's dump (the `n` command) showed the failure this plan removes. Three ADC bridges
`[3,ADC_1] [3,ADC_2] [2,ADC_2]` merged rows 2 and 3 and two physical ADC channels into one
net, and all three paths came back `x=-1,y=-1` ("Couldn't find a path") because chip K's
eight Y lanes were already full (two rail duplicates, BUF_IN, the ADC_0 net, DAC0, DAC1).
Physical ADC nodes are electrical nodes today: two rows on the same ADC short together, two
ADCs on one row merge nets, only ADC0-3 reach the fabric (chip K X8-X11), and each one
placed costs a chip K lane permanently.

Kevin's direction: stop making the user pick a channel. Keep a second pool of
time-domain-multiplexed ADCs so any number can be placed, and remove the ADC-number picker
from probing, menus, and everywhere else.

### Rulings taken this session (durable - save as memory `adc-virtual-slots-ruling` first thing after approval)

1. **ADC_n becomes a virtual slot.** Placing an ADC auto-assigns the lowest free `ADC_n`.
   Physical ADC0-3 become an internal pool nobody names. Old slot files, docs,
   `connect(5, ADC0)` and `adc_get(0)` keep working and never short or fail. `ADC_7` stays
   the probe tip (never assigned as a slot). The 0-5 V physical ADC4 stops being a user node.
2. **Dedicated while free, multiplex under pressure.** One or two placed ADCs behave
   exactly as today (own physical channel, static route, 48 kHz ring, scope app and USB
   audio unaffected). Extra ADCs, or ones that fail to route, fold onto a shared channel
   and sample round-robin. Matches the sense-tap ruling: degrade only under real pressure.

Design philosophy applies: does exactly what I expect, hints not instructions; old code
solidifies (a single ADC must behave identically); coherence across wheel, probe, OLED,
LEDs, terminal.

### Where it lands

New user-facing behaviour goes to **dev** (release policy). This session sits in the
`JumperlOS-main` worktree; the dev worktree is `/Users/kevinsanto/Documents/GitHub/JumperlOS`
(dev = 60ca87a, 6 commits ahead of main: the 5.7.10.1 sweep). Implementation must be done in
the dev worktree; all file:line references below are from main and are within a few lines on
dev (the sweep touched MeasureMode.cpp, TimeDomainMultiplexer.cpp, Probing.cpp, Menus.cpp,
NetManager.cpp, States.cpp lightly). Build with `<scratch>/pio313/bin/pio` (system pio is
Python-3.14-gated).

## Mechanisms verified in code (the plan builds on these)

- **Virtual-node template already exists.** Fake GPIO inputs `FAKE_GP_IN_0..31` (ids 158-189,
  `src/JumperlessDefines.h:516-560`) are virtual nodes. The router expands them to
  `ADC0 + fakeGpioInputAdcChannel` in `findStartAndEndChips`
  (`src/routing/NetsToChipConnections.cpp:5901-5925`), in the overlap validator's
  `expandNode` (`:5326-5340`), and in `assignPathType` (`:6058-6095`).
- **Lane sharing trick.** `mergeFakeGpioInputNets()` (`:142-176`) rewrites every fake-input
  path's `net` to the sentinel `FAKE_GPIO_TDM_NET` (58) before routing so they share one chip K
  Y lane as "same net"; `restoreFakeGpioInputNets()` (`:179-268`) puts the real nets back and
  scrubs `chipStates[].xStatus/yStatus`. `bridgesToPaths()` order (`:1595-1810`):
  `sortPathsByNet → mergeFakeGpioInputNets → findStartAndEndChips/assignPathType per path →
  resolveChipCandidates → commitPaths → resolveAltPaths → resolveUncommittedHops →
  [fillUnusedPaths dup pass] → restoreFakeGpioInputNets → couldntFindPath(1) →
  checkForOverlappingPaths → validateAllPaths`. The overlap validator (`:5277-5300`) and
  `validateAllPaths` (`src/routing/RouteSafety.cpp:392-402`) both skip fake-input paths
  explicitly because they run AFTER restore.
- **TDM engine.** `src/sensing/TimeDomainMultiplexer.{h,cpp}`: 32 channels, each with up to
  4 stored hops; `switchTo()` opens the previous channel's whole path and closes the next
  one's; `switchAndRead()` settles 80 us then `readAdcVoltage()`; `pollNext()` round-robins
  and disconnects after each read. ADC ownership through the pool (`assignFreeAdc` →
  `infraAcquireAdc(INFRA_ADC_TDM, 0x0F, false)`).
- **Pool arbiter.** `src/routing/InfraPaths.cpp:840-905`: `s_adcOwner[5]`, one owner enum per
  channel; `infraReleaseAdc(user)` frees EVERY channel that owner holds (header warns not to
  share an enumerator between consumers); `infraAdcUserClaimed(ch)` scans `bridges[]` for node
  `ADC0+ch`, ignoring fake-input and ephemeral bridges.
- **Rebuild skeleton** (`src/Commands.cpp:200-340`, and the two siblings at ~395 and ~600):
  `infraEvaluate()` at the head inside the core1busy window → `clearAllNTCC` →
  `loadBridgesFromState` → `getNodesToConnect` → `bridgesToPaths()` → `chooseShownReadings()`
  → `core1req::post(REQ_SEND)` → core 1 `sendPaths()` → `sendAllPaths(clean)`
  (`src/CH446Q.cpp:798`) → `sendPath(i)` closes every hop whose chip/x/y are not -1.
  `sendPath` honours NO skip flag (`pathStruct.skip` is a router-internal flag; `grep skip
  src/CH446Q.cpp` finds nothing). Fake GPIO therefore closes all shared paths on every
  rebuild and only opens them afterwards (`finalizeFakeGpioAfterRouting`, called from
  `src/main.cpp:1504` at boot and from slot loads only; per-bridge edits go through
  `updateFakeGpioAfterConnectionChange`, `src/sensing/FakeGpio.cpp:851`).
- **Slot files store node NAMES** (`JumperlessState::serializeBridges`,
  `src/routing/States.cpp:1732-1785`, via `nodeValueToString`; the parser accepts numbers
  too). `src/remembering/FileParsing.cpp:1188-1192` and `:2573-2577` rewrite "110".."114" ↔
  "ADC0".."ADC4" in the legacy text path.
- **Free node ids:** 190-198 (9 ids) and 200-255 (`BOUNCE_NODE` is 199; nothing is defined
  above it; `nodeToNetIndex[256]` bounds ids below 256).
- **Measure mode** (`src/sensing/MeasureMode.cpp:337-400`) is an ephemeral bridge
  `node → ADC0+ch` acquired as `INFRA_ADC_MEASURE` (mask 0x1F); `addEphemeralConnection`
  runs `refreshLocalConnections` (`src/routing/States.cpp` ~630-700), so it passes the
  rebuild-head hook.
- **Display plumbing** keyed by physical channel: `adcReadings[8]`, `showADCreadings[8]`
  (net index shown per channel), `adcReadingColors[8]` (`src/Peripherals.h:76-78`);
  `showLEDmeasurements()` (`src/Peripherals.cpp:1215`) reads channels 0-7 and paints the net;
  `rebuildShownReadings()` walks paths for ADC nodes; Graphics colours ADC nets from
  `adcReadingColors[anyAdcConnected(net)]` (`src/Graphics.cpp:2972-3145`).

- **The chip K lane for a row is fixed, not allocated**: `commitPaths` case BBtoSF sets
  `yMapSFc1 = paths[i].chip[0]` (`NetsToChipConnections.cpp:2607`, committed at `:2630-2654`),
  so chip A rows always want K y0, chip B rows y1, and so on; `resolveAltPaths` borrows another
  breadboard chip's lane through a bounce hop (`:4223`). Two paths share a lane only when they
  carry the same net number AND on the `allowStacking == 1` retry (`freeOrSameNetY`, `:4517`).
  That is why fake GPIO merges its paths into one pseudo-net. It also means "any number of
  ADCs" cannot come from routed paths: the fabric has 8 K lanes, full stop.
- **Fake-GPIO stored-hop TDM has documented stale-hop bugs** (`updateFakeGpioAfterConnectionChange`
  runs before the rebuild it reacts to, `src/routing/States.cpp:653/700`; its `affected` predicate
  misses lane moves, `FakeGpio.cpp:898`; `setChannelPath` never consults `chipStates` or
  `wouldShort`, `TimeDomainMultiplexer.cpp:183-193`; 100 ms default per-hop timeout; non-strict
  ring reads). The net-voltage scan's tap (`senseNodeVoltage`, `src/sensing/NetVoltageScan.cpp:268`)
  computes a fresh route per poll with `fastConnectPath` (`src/routing/RouteSafety.cpp:864`:
  `wouldShort` check, lane claim, unwind on timeout, generation-stamped teardown), 80 us settle,
  500 us ring dwell with strict freshness, and runs one tap per 5 ms pass on core 1 from
  `serviceNetVoltageScan` (`src/main.cpp:2105`, after `core_sync_release`). Its one-shot variant
  (`serviceOneShotTap`, `:583-612`) shows how to keep the hardware gates and drop the politeness
  gates.
- **Probe connect flow**: a pad tap returns a function node through `selectSFprobeMenu`
  (`src/Probing.cpp:8223`, ADC pad → `chooseADC()` at `:4170`), stored as one end in
  `nodesToConnect[]`; a row tap fills the other end; the bridge is made at `:3739/3749` and both
  ends reset at `:3908-3910`. So slot allocation belongs at the bridge site, keyed on a sentinel
  node, and is independent of the tap order.

## Design

### Vocabulary
- **Virtual ADC slot** `ADC_n`, n in 0..15, node ids `VADC_0..VADC_15` = 200..215 (contiguous,
  below the 256 ceiling of `nodeToNetIndex[256]`). Slot 7 is reserved (never assigned) so
  `ADC_7` / `adc_get(7)` stay the probe tip (node 115). Slot 15 is reserved for measure mode.
  Slots 0-6 and 8-14 are user slots (14 of them).
- **`ADC_AUTO`** sentinel (id 216): "the next free user slot". Every UI placement path stores
  this sentinel as the special-function end; it is resolved to a real slot at the moment a
  bridge is added (one helper, `vadcResolveAuto(rowNode)`), so each row tap gets its own slot.
- **Physical channels** ADC0..ADC3 (ids 110-113) keep their macros for internal users only and
  lose every user-facing name. ADC4 (0-5 V, chip L) is out of the pool in v1.

### Two modes per slot (Kevin's ruling 2)
- **Dedicated**: the slot owns one physical channel. The router expands `VADC_n` to that
  channel's node exactly like `FAKE_GP_IN` (write-back in `findStartAndEndChips`), the path is
  routed and sent as a normal static path, and reads come off the ADC ring at full rate. This is
  today's behaviour for the first one to three ADCs.
- **Shared**: the slot has no fabric of its own. Its path is marked as an unrouted "sense" path
  (new `pathType SENSE`, skipped by every routing stage, by `couldntFindPath`, by the overlap and
  short validators and by `sendPath`, exactly where `VIRTUAL` is skipped today). A priority tap
  slot on core 1 inside `serviceNetVoltageScan` samples every shared slot round-robin with the
  scan's fresh-route tap primitive, acquiring a channel per tap the way the scan does. Cadence
  200 Hz divided by the number of shared slots; latest sample cached per slot with its age.

### Allocator `vadcEvaluate()` (core 0, rebuild head, next to `infraEvaluate()`)
1. Active slots = slots with any bridge in `bridges[]` (persistent or ephemeral).
2. Priority: measure mode's slot 15 first, then user slots in ascending order.
3. At most **3** dedicated channels, so one channel always remains for per-tap consumers (scan,
   shared slots, guide one-shots). Fake-GPIO TDM keeps its own channel when inputs exist; the
   scan already rides along on TDM's channel when nothing is free.
4. Ownership through the infra pool under new enumerators (`INFRA_ADC_VADC_0..3`, one per
   dedicated slot, because `infraReleaseAdc` frees everything one enumerator owns).
5. `infraAdcUserClaimed()`, `nodeHasAnyBridgePM()` and `partsNodeWired()` stop scanning bridges
   for ids 110-114 and ask the pool, otherwise they go blind once no user bridge carries a
   physical id.
6. **Demotion**: after `bridgesToPaths()`, a dedicated slot whose path came back unroutable is
   marked shared and the routing portion of the rebuild runs once more (no re-entry of
   `refreshConnections`). Promotion back to dedicated happens only when the bridge set changes,
   so a slot cannot flap every rebuild.
7. Measure mode keeps its ephemeral bridge and node-name readout; if no channel is free the
   allocator demotes the highest user slot for the duration of the measurement.

### Placement surfaces (coherent across probe, wheel, menu, terminal, Python)
- Probe ADC pad: `chooseADC()` is deleted; the pad returns `ADC_AUTO`. Each row tap makes a
  bridge `row-VADC_n` with a fresh n. Tapping a row whose net already has a virtual ADC is a
  no-op with a hint (bloom the existing ADC's net, OLED note); the UI never puts two ADCs on
  one net. Probe REMOVE on the row removes the bridge; a slot with no bridges is free again.
- Clickwheel `ZONE_ADC`: one entry, "ADC", commits `ADC_AUTO`.
- Menu Show → Voltage: the `--*0**1**2**3**4*` picker line goes away; the action adds
  `ADC_AUTO` per chosen node.
- Terminal `f 5-ADC_3` and Python `connect(5, ADC3)` honour the explicit slot (power users may
  merge nets on purpose); bare `ADC` means auto.
- Logo-pad config gains an `adc` (auto) value; `adc_0..adc_4` keep parsing for old configs.

### Readback
`adc_get(n)` / `get_adc(n)`: dedicated → fresh ring read of the slot's channel; shared → the
sampler's cached sample (core 0 never drives crosspoints); 7 → probe tip. A status accessor
reports volts, age, and dedicated/shared/no-route/unplaced. The terminal `v` lists placed slots
with their net and voltage; `vN` reads slot N. OLED `{adc:N}`, JSON state `"ADC"`, the net
listing, the highlight readout ("ADC n · x.xx V") and the LED net colouring all read the slot
table instead of the physical-indexed `adcReadings/showADCreadings/adcReadingColors[8]`.

### Persistence
Slot files store names, so `n1: ADC0` loads as `VADC_0` once the name tables point at the
virtual ids; numeric `110..114` remap on deserialize. The legacy text node-file replace tables
are audited for 3-digit substring collisions before the ids are final.

(File-by-file change list, ordered steps, and verification are being drafted from the two design
agents' plans and will replace this note.)
