<!-- Design-agent report from the 2026-09-02 ADC redesign session (chat b816f338). Line numbers cite the JumperlOS-main worktree at 5.7.10.0 and drift a few lines on dev; symbols are authoritative. -->

# ADC-channel choice/placement/display/persistence inventory — JumperlOS (read-only pass)

Bucket key used throughout: **A** = a human or script *picks a physical ADC number*; **B** = something *reads back / displays / stores* a number that was picked; **C** = internal physical-channel use that must keep working. Only A and B carry "change needed".

---

## 1. Probe surfaces (src/Probing.cpp)

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Probing.cpp:5018-5115` | **`chooseADC()`** — the on-board 6-option pad menu. Paints `"0" "1" "2" "3" "4" "P"` (5027-5032) then decodes pad reading ranges to node ids: `case 31 ... 35: { selected = ADC0; function = ADC0; }` … `case 56 ... 60: { selected = ADC7; ... }` (5059-5111). **A** | Delete or repurpose. Under auto-assign this whole blocking menu disappears; the ADC_PAD tap should return a single "virtual ADC" node the allocator picks. If kept for power users it must offer *virtual slots*, not 0-4+P. |
| `src/Probing.cpp:4164-4178` | Call site 1: `case ADC_PAD: { inPadMenu = 1; function = chooseADC( ); blockProbing = 800; ... setLogoOverride( ADC_0, -2 ); setLogoOverride( ADC_1, -2 ); }` — probe taps the ADC logo pad in connect mode, the returned node becomes one end of the bridge. **A** | Replace `chooseADC()` with `function = <next free virtual ADC node>`; keep the LED override + `blockProbing` timing (they exist to swallow the pad-release bounce). |
| `src/Probing.cpp:4447-4455` | Call site 2, inside `attachPadsToSettings( int pad )`: `adcChosen = chooseADC( ); Serial.println( adcChosen ); settingOption = adcChosen - ADC0;` — the logo/building-pad *binding* flow. `settingOption` is written but every consumer is commented out (4613/4619/4624/4629), so the channel-index leak is dead; only `function = adcChosen;` (4601) survives. **A** | Same replacement. `settingOption = adcChosen - ADC0` should be deleted outright (dead, and meaningless once ADC ids are virtual). |
| `src/Probing.cpp:4601` | `case 1: { function = adcChosen; break; }` — the ADC branch of the pad-binding dispatcher. **A** | Feed it a virtual node id. |
| `src/Probing.cpp:4610-4632` | Write-back: `jumperlessConfig.logo_pads.top_guy = nodeToLogoPadConfig( function, jumperlessConfig.logo_pads.top_guy );` (and `.bottom_guy`, `.building_pad_top`, `.building_pad_bottom`), then `saveLogoBindings( )` (4634). **A + persist** | This is the **config key** the picker writes. See row below for the table. |
| `src/Probing.cpp:1774-1800` | **`nodeToLogoPadConfig()`** forward map node→config int: `case ADC0: return 2; case ADC1: return 3; case ADC2: return 4; case ADC3: return 5; case ADC4: return 6;` **A/persist** | Needs a value for "virtual ADC" (there is no 8+ hole reserved; `padNodeTable` tops out at 64). Either add `{"adc_auto", 65}` or map every virtual ADC node to one new value. |
| `src/Probing.cpp:1740-1769` | **`resolveLogoPadAssignment()`** reverse map config int→node: `case 2: return ADC0; … case 6: return ADC4; case 7: return ADC4; // legacy "adc_5" - no routable ADC5`. **B** | Must resolve the new value to "allocate a virtual ADC now"; note it may need to return a *fresh* node per tap rather than a constant. |
| `src/Probing.cpp:7915-7917` | `else if ( pad == ADC_PAD ) { row = ADC_PAD; }` in `convertPadsToRows()` — measure-mode taps pass ADC_PAD through unresolved. **B** | Nothing required, but see MeasureMode.cpp:217 — ADC_PAD is explicitly *not measurable*, so a measure tap on the ADC pad is silently ignored today. |
| `src/Probing.cpp:2222-2226` | Clickwheel cursor, display half: `const int adcMap[ 6 ] = { ADC0, ADC1, ADC2, ADC3, ADC4, ADC7 }; const char* adcLabels[ 6 ] = { "0","1","2","3","4","P" }; actualNode = adcMap[ subIndex ]; snprintf( displayName, …, "ADC %s", adcLabels[ subIndex ] );` then 6 pad LEDs painted per index (2233-2236). **A/B** | Collapse to a single "ADC" entry (or N virtual slots). `subIndex` max is hardcoded twice: `case ZONE_ADC: subIndex = 5;` (2029) and `case ZONE_ADC: maxSubIndex = 5;` (2072) — both must change or the zone becomes 1-wide. |
| `src/Probing.cpp:2513-2515` | Commit half: `} else if ( cursorZone == ZONE_ADC ) { const int adcMap[6] = {ADC0,ADC1,ADC2,ADC3,ADC4,ADC7}; selectedNode = adcMap[ subIndex ]; }`. `selectedNode` then falls into the shared `if ( selectedNode > 0 )` "treat encoder selection like a probe touch" path at 2544. **A** | Set `selectedNode` from the allocator. Everything downstream is node-generic, so this is a one-line change. |
| `src/Probing.cpp:1924/2661` | `ZONE_ADC = 4` in the two duplicate `CursorZone` enums (one local to the encoder helper, one at file scope). **A** | Keep the zone; only its cardinality changes. |
| `src/Probing.cpp:7486-7491` | `case ADC_PAD: clearColorOverrides(1,1,0); setLogoOverride( ADC_0, -2 ); setLogoOverride( ADC_1, -2 );` — pad-hover highlight. **B** | No change (pad-level, not channel-level). |
| `src/Probing.cpp:1428, 1435` | `probeRowMap[108]` / `probeRowMapByPad[108]` both contain `… GPIO_PAD, DAC_PAD, ADC_PAD, BUILDING_PAD_TOP …` — the raw pad→node decode. **C** | No change. |
| `src/Probing.cpp:3788-3843` | **REMOVE mode**: the clear branch is fully node-generic — `snprintf(node1Name, …, "%s", definesToChar( nodesToConnect[0] ))`, then `bool removed = removeBridgeFromState( nodesToConnect[ 0 ], -1, true );` (3843). There is **no ADC special-casing** anywhere in the remove path. **C/B** | Works unchanged *provided* the virtual node is a real node id present in `bridges[]`. The only visible effect is `definesToChar()` needing a name for the new ids (see §7). |

Related but outside Probing.cpp:
- `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Graphics.cpp:3722-3726` — `if (node == ADC_PAD) { setLogoOverride(ADC_0,color); setLogoOverride(ADC_1,color); return; }` in the node→pixel painter. **B**, no change.
- `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Apps.cpp:1170-1174` — `case ADC_PAD: setLogoOverride( ADC_0, modeLogoColor ); setLogoOverride( ADC_1, modeLogoColor );` in the app-mode pad picker. **B**, no change.
- Probe highlight/readout when an **ADC net** is tapped is in Highlighting.cpp, see §7.

---

## 2. Clickwheel menu (src/Menus.cpp + menu text)

The menu tree is a C array, not a file on disk at rest: `src/Menus.cpp:31` `#include "menuTree.h"`, and `readMenuFile(0)` first calls `writeMenuTree()` then re-reads `/MenuTree.txt` (`src/Menus.cpp:132-159`). So the strings below are the source of truth.

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/remembering/menuTree.h:89,103-105` | The ADC picker itself: under `"Show",` (89) → `"-$Voltage$",` (103) → `"--*0**1**2**3**4*",` (104) → `"--->n1",` (105). The five `*n*` stars ARE the ADC-number picker. **A** | Replace `"--*0**1**2**3**4*"` with a single node prompt (`"--Voltage>n1"` shape) so "Show → Voltage" goes straight to node selection and auto-assigns. |
| `src/remembering/menuTree.h:79-83` | Connect-side `"-$Voltage$", "--$DAC$", "--*DAC 0**DAC 1**Top R '**Bot R ,*", "--->v1", "---->n1"` — DAC/rail, **not** ADC. No change. |
| `src/remembering/menuTree.h:165` | `"-Tip    \31Voltage"` under Calibration — probe-tip cal, ADC7. **C**, no change. |
| `src/remembering/menuTree.h:249,251,253` | `"---*0**1**2**3**4**Max *"` ×3 under `Routing Options → Stack` — **not ADC** (path-stacking counts). Do not touch. |
| `src/remembering/menuTree.h:340,370` | `-$Voltage$` inside the commented-out legacy `char menuTree[]` blob (262-…). Dead text. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Menus.cpp:2077-2089` | Where the star option index becomes the ADC number: `optionSelected = highlightedOption;` … `currentAction.from[ currentAction.connectIndex ] = optionSelected;`. **A** | If the `*0*..*4*` line is removed, `from[]` no longer carries a channel — the action handler must stop switching on it. |
| `src/Menus.cpp:4164-4193` | The action: `if ( menuLines[ currentAction.previousMenuPositions[1] ].indexOf( "Voltage" ) != -1 )` then `switch ( currentAction.from[i] ) { case 0: addBridgeToState( ADC0, currentAction.to[i] ); … case 4: addBridgeToState( ADC4, currentAction.to[i] ); }`. **A** | Replace the 5-way switch with one `addBridgeToState( <allocated virtual ADC>, currentAction.to[i] );` per selected node. Note: this is a *plain* `addBridgeToState`, i.e. the menu ADC is a persistent user bridge, unlike measure mode. |
| `src/Menus.cpp:825-853` | `setMenuDepthLEDs()` breadcrumb: `static const logoOverrideNames padPairs[3][2] = { { ADC_0, ADC_1 }, { DAC_0, DAC_1 }, { GPIO_0, GPIO_1 } };` — comment at 826-827 "Pads read top-to-bottom as ADC -> DAC -> GPIO". **B, cosmetic only** — these are LED *pad* names, not ADC channels. | No change. |
| `src/Menus.cpp:4207-4300` | Sibling handlers (`Current` → ISENSE_PLUS/MINUS, `Digital/GPIO` → RP_GPIO_1..8, `UART`, `I2C`) use the identical `from[i]` switch idiom — they are the pattern to preserve when the ADC one is collapsed. | Untouched, but they show why removing a whole star line is safe: each handler is keyed by its own `menuLines[...].indexOf(...)`. |
| `src/eyecandy/OledGui.cpp:142-146` | `if ( strcmp( base, "adc" ) == 0 ) { … snprintf( out, outSize, "%.2f", (double)adcReadings[argN] ); }` — the `{adc:N}` OLED token, N is a **physical** channel index into `adcReadings[8]`. **A (user types N) + B** | Needs a virtual-slot token (`{vadc:N}`) or `{adc:N}` must be redirected through the virtual table; today N is raw hardware. |

No "Show Voltage" OLED flow beyond the `{adc:N}` token was found — `src/oled.cpp` has **zero** `adc` matches.

---

## 3. Measure mode (src/sensing/MeasureMode.cpp)

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/sensing/MeasureMode.cpp:337-341` | `int MeasureMode::findUnusedADC() { … return infraAcquireAdc(INFRA_ADC_MEASURE, 0x1F, false); }` — comment: "measure mode may use ADC0-4 (mask 0x1F; ADC7 is hardwired to the probe tip). Exclusive ownership until disconnectADC." **C (already auto-assigned — this is the model)** | Keep. This is exactly the arbitration the redesign generalizes. |
| `src/sensing/MeasureMode.cpp:343-377` | `connectADCToNode()`: `const int adcDefines[] = { ADC0, ADC1, ADC2, ADC3, ADC4 }; adcChannel = channel; adcDefine = adcDefines[channel];` then `globalState.addEphemeralConnection(node, adcDefine, …)` (363). On failure: `ReadingDisplay::emitLiveSerialLine("No ADC available");` + `oled.clearPrintShow("No ADC\navailable", …)` (348-349). **C + B** | Once user ADCs are virtual and TDM'd, "No ADC available" should become far rarer — but the pool has only 5 slots and measure mode takes one *exclusively*, so the virtual pool and measure mode will contend. Decide whether measure mode also becomes a TDM channel. |
| `src/sensing/MeasureMode.cpp:379-396` | `disconnectADC()` → `globalState.removeEphemeralConnection(...)` then `infraReleaseAdc(INFRA_ADC_MEASURE);` (395). **C** | Keep. |
| `src/sensing/MeasureMode.cpp:405-421` | `if (!measurementActive \|\| adcChannel < 0) return; … float rawVoltage = readAdcVoltage(adcChannel, 16);` **C** | Keep (reads the physical channel it owns). |
| `src/sensing/MeasureMode.cpp:445` | `ReadingDisplay::show(definesToChar(measuredNode, 0), -1, valueString);` — the label is the **measured node's** name, never "ADC n". **B** | No change; this is the good precedent (channel-agnostic UI). |
| `src/sensing/MeasureMode.cpp:205-219` | `isMeasurableNode()`: rails/DACs/3V3/5V/GND true, then "Special function pads - NOT measurable … LOGO_PAD_TOP (142), GPIO_PAD (144), **ADC_PAD (146)**" → `return false;`. **B** | If virtual ADC nodes ever reach here they'd be rejected by the same `return false` default — add them explicitly if a measure tap on a virtual ADC should work. |
| `src/sensing/MeasureMode.cpp:231-247` | Same-net shortcut using `nodeToNetIndex[256]` — skips the re-route when the tapped node is already on the measured net. **C** | Keep; it also constrains new node ids to `< 256`. |
| Highlighting interplay | `src/eyecandy/Highlighting.cpp:1644` `int adc = anyAdcConnected( netHighlighted );` and `:1891` `snprintf( name, sizeof(name), "ADC %d", adc );` — see §7. | The measure-mode readout says the node name; the *highlight* readout says "ADC n". Inconsistent today; the redesign should unify on the node name. |

---

## 4. MicroPython

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/modules/jumperless/modjumperless.c:393-410` | String→node table: `{ "ADC0", 110 }, { "ADC_0", 110 }, { "ADC0_8V", 110 }, … { "ADC7", 115 }, { "ADC_7", 115 }, { "ADC7_PROBE", 115 }`. Consumed by `connect()`/node parsing. **A (script names a channel)** | Must keep resolving to 110-115 for back-compat, **plus** new names for virtual ADCs. |
| `modules/jumperless/modjumperless.c:4597-4602` | `static const node_obj_t node_adc0_obj = { …, .value = 110 };` … `node_adc7_obj … .value = 115`. **A** | Same fork: keep pinned, add `VADC`/auto constant. |
| `modules/jumperless/modjumperless.c:6530-6535` | Module dict: `{ MP_ROM_QSTR( MP_QSTR_ADC0 ), MP_ROM_PTR( &node_adc0_obj ) }, … MP_QSTR_ADC7`. **A** | Add new symbols; don't remove old ones (breaks every published example). |
| `modules/jumperless/modjumperless.c:1983-1998` | `jl_adc_get_func`: `if ( channel < 0 \|\| channel > 7 ) mp_raise_ValueError("ADC channel must be 0-7"); float voltage = jl_adc_get( channel );` **A (channel number is the argument)** | This is the hard one: `adc_get(0)` means *physical channel 0*, not "the ADC I placed". Needs an overload accepting a node (`adc_get(ADC0)` / `adc_get(20)` → read whatever virtual channel is on that node), or a new `read_node(n)`. |
| `modules/jumperless/modjumperless.c:6722,6725` | `{ MP_QSTR_adc_get, &jl_adc_get_obj }` and `{ MP_QSTR_get_adc, &jl_adc_get_obj }` — both names, one impl. **A** | Both aliases inherit whatever the fix is. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/JumperlessMicroPythonAPI.cpp:384-386` | `float jl_adc_get( int channel ) { return readAdcVoltage( channel, 16 ); }` — raw pass-through to the physical channel. **C/A boundary** | This is the single choke point where a node→virtual-channel indirection could be inserted. |
| `modules/jumperless/modjumperless.c:4736` | help: `"  ADC:          ADC0-ADC4, PROBE (analog inputs)\n"` **B** | Rewrite. |
| `modules/jumperless/modjumperless.c:4884-4886` | help("ADC"): `"   adc_get(channel)                  - Read ADC input voltage\n"` / `"   get_adc(channel)                  - Alias for adc_get\n\n"` **B** | Rewrite. |
| `modules/jumperless/modjumperless.c:5094` | `"  voltage = get_adc(1)                       # Read ADC1 using alias\n"` **B** | Rewrite. |
| `modules/jumperless/modjumperless.c:3892` | comment `//   e = oled_add_text(s, "{adc:0} V", x=0, y=0, …)` **B** | Rewrite with the token decision from §2. |
| `modules/jumperless/modjumperless.c:5010,5014` | `"   s.add(Text(\"A0 {adc:0}V\", …))\n"` and `"   Tokens: {adc:N} {gpio:N} {dac:N} {uptime} {freemem} {undo}\n"` **B** | Same. |
| `modules/jumperless/modjumperless.c:643, 6654` | `{ "ADC_PAD", 146 }` and `{ MP_QSTR_ADC_PAD, &probe_adc_pad_obj }` — the *pad*, not a channel. **C** | No change. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/lib/micropython/port/machine_adc_jl.c:44-59, 95-140` | `machine.ADC(id)`: `// GPIO26=ADC0, GPIO27=ADC1, GPIO28=ADC2, GPIO29=ADC3`, `NUM_ADC_CHANNELS (5)`, `ADC_CHANNEL_FROM_GPIO(gpio) ((gpio) - ADC_BASE_PIN)`, and it calls `adc_select_input(channel); adc_read();` directly. **C** | **Bypasses the crossbar entirely** — raw pico-sdk. Must NOT change; but document that `machine.ADC` and `jumperless.adc_get` are different things (the RP2350 pin mapping here also disagrees with the V5 board's 40-47 init at Peripherals.cpp:428). |
| `src/snakes/micropythonExamples.h`, `src/snakes/projectFiles.h`, `src/snakes/Python_Proper.cpp` | 70 combined hits for `adc_get`/`ADCn`; e.g. `micropythonExamples.h:4340 ADC_PAD = _native.ADC_PAD`, `:4868` pad-name tuple, `:6678 ADC_PAD: ProbePad`. **B (docs/examples)** | Bulk text update; nothing structural. Not enumerated. |

---

## 5. Terminal

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/SingleCharCommands.cpp:2766-2779` | `v<N>`: `char ch = arg[0]; if ( isdigit(ch) ) { int adc = ch - '0'; if ( adc >= 0 && adc <= 4 ) { target->print(" adc"); target->print(adc); … readAdcVoltage( adc, 32 ); } }`. **A + B** | `v0`-`v4` are raw physical reads. Keep for diagnostics; add a "list placed virtual ADCs" view. |
| `src/SingleCharCommands.cpp:2817-2831` | Bare `v`: `for ( int i = 0; i < 5; i++ ) { target->print("adc"); target->print(i); … readAdcVoltage( i, 32 ); }` then `float probeVoltage = readAdcVoltage( 7, 32 );` printed as `"probe = "`. **B** | Should additionally (or instead) list *placed* channels by node name. |
| `src/SingleCharCommands.cpp:3954-3966` | `"M23" -> left = ADC2, right = ADC3.` → `const int l = arg[0]-'0'; const int r = arg[1]-'0'; usb_audio_set_channels(l, r)`; error text `"Channels must be two distinct ADC channels 0-7, e.g. M01"`. **A** | USB-audio picks *physical* channels. If a virtual ADC gets TDM'd onto channel 0 or 1 (the config defaults), audio and the multiplexer fight over the same ring lane — see Open questions. |
| `src/SingleCharCommands.cpp:3983-3986` | `Jerial.printf("USB audio: streaming ADC%d (left) + ADC%d (right) at %lu Hz\n\r", s.left_ch, s.right_ch, …)` and `Jerial.println("Route rows to them first, e.g.  connect(ADC0, 20)")`. **B** | Help text pins the old model; update. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Commands.cpp:823-885` | `float measureVoltage(int adcNumber, int node, bool checkForFloating)`: `switch (adcNumber) { case 0: adcDefine = ADC0; … case 4: adcDefine = ADC4; case 5: /* adcDefine = ADC5; */ case 6: /* ADC6 */ case 7: adcDefine = ADC7; }` then `removeBridgeFromState(adcDefine, -1); addBridgeToState(node, adcDefine); … readAdcVoltage(adcNumber, 8);` (867), and cleanup `removeBridgeFromState(node, adcDefine)` (883). The 5/6 gap is deliberately dead (no ADC5/6 exist). **A** | Only one caller: `src/Apps.cpp:1796 float measuredVoltage = measureVoltage( 2, i, true );` — hardcodes channel 2. Prime candidate for the auto-assign API (`measureVoltage(node)`). Note it *bulldozes* the channel with `removeBridgeFromState(adcDefine, -1)` — that would destroy a user's virtual placement if the channels merge. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/NetManager.cpp:109-114` | `specialDefines[]`: `{"ADC_0","ADC0",ADC0}, {"ADC_1","ADC1",ADC1}, {"ADC_2","ADC2",ADC2}, {"ADC_3","ADC3",ADC3}, {"ADC_4","ADC4",ADC4}, {"ADC_7","ADC7",ADC7}` (and `{"ADC_PAD","ADC_PAD",ADC_PAD}` at 131). Short name is the one printed in net listings. **A/B** | Add entries for the new virtual ids or `definesToChar` prints the bare number (see §7). |
| `src/routing/NetManager.cpp:2136-2143, 2156-2167` | `defSpecialToCharShort[49]` (`"ADC_0","ADC_1","ADC_2","ADC_3","ADC_4","ADC_7"`) and `defSpecialToCharLong[49]` (`"ADC0",…,"ADC7"`) — index-based fallback tables. **B** | Only reachable for ids 100-148 (see `definesToChar` guard 2190); new ids must go in `specialDefines[]`. |
| `src/routing/NetManager.cpp:2175-2205` | `definesToChar()` — `findDefineInfoByValue()` first, then `else if (defined >= 100 && defined <= 148) { index = defined - 100; … }`, else `itoa(defined, same, 10); return same;` **B** | An unnamed virtual id prints as a raw number everywhere (net lists, probe clear messages, slot YAML). |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/States.cpp:2736-2789` | `parseNodeName()` — accepts numeric first (`if (val >= 0 && val <= 200) return val;` at 2745-2748) then scans `nanoDefines[35]` and `specialDefines[]` with `const int numSpecialDefines = 70;` (2769). **A/B/persist** | Two hard edits: the **≤200 numeric cap** and the **hardcoded 70** element count (already a landmine — `specialDefines[]` visibly ends at FGPI23/181, so the 70 is a manual count that must be bumped in lockstep). |
| `src/SingleCharCommands.cpp:425-427` | `registerCommand( 'f', "load node file", …, cmd_loadNodeFile, …)`. Node-file text parsing itself is `replaceSFNamesWithDefinedInts()` — see §6. **A** | |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/selfreflection/HelpDocs.cpp:260,261,1010,1011` | `"  v       - show all ADC readings"`, `"  v0-v4   - show specific ADC (0-4)"`, `"  v     - All ADC readings"`, `"  v0-v4 - Specific ADC channel"`. **B** | Rewrite. |
| `src/selfreflection/HelpDocs.cpp:270,322,338,954,980,1000,1126,1283` | `"  - Python access: adc_get(0)"`, `"  - adc_get(0)          - Read voltages"`, `"  print(adc_get(0))"`, `"     - ADC (0-4, Probe)"`, `"  adc_get(0)                 - Read voltage"`, `"  Use scope app to visualize signals on ADC pins"`. **B** | Rewrite. Note 1283 references a "scope app" — `grep -i scope src/Apps.cpp` returns **nothing**; the oscilloscope entry in menuTree.h:135 is commented out. Stale doc. |
| `src/Tui.cpp`, `src/ConfigTui.cpp` | **Zero** `ADC`/`adc` matches. ConfigTui reaches ADC config only generically via `{ "Logo Pads", nullptr, CAT_SECTION, JLSECT_logo_pads, 0 }` (`src/ConfigTui.cpp:60`) driven by the X-macro tables. | Nothing direct; the `[calibration] adc_*` and `[logo_pads]` rows appear automatically. |

---

## 6. Persistence

**Decisive answer: modern slot files store node NAMES; the legacy `.txt` node-file path stores NUMBERS and rewrites them by ordered string substitution.**

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/States.cpp:1764-1772` | `serializeBridges()`: `// Use node names instead of raw numbers for readability` → `String n1Name = nodeValueToString(node1); … output += "  - {n1: " + n1Name + ", n2: " + n2Name + ", dup: " + String(...)`. **persist** | A slot written today with an ADC bridge contains `n1: ADC0`. So old slots survive **iff** `ADC0` keeps resolving to 110. |
| `src/routing/States.cpp:2793-2806` | `nodeValueToString()` → `const char* name = definesToChar(nodeValue, 1); … // Fallback to number if name not found; return String(nodeValue);` **persist** | Unnamed virtual ids silently persist as bare integers — then reload through the `≤200` numeric branch of `parseNodeName`. Works, but ugly and version-fragile. Give them names. |
| `src/routing/States.cpp:1787-1790` | `deserializeBridges()`: `// Parse bridge entry: - {n1: TOP_RAIL, n2: NANO_D5, dup: 2, color: red}` / `// Also accepts: - {n1: 101, n2: 75, dup: 2}` — both forms. **persist** | Back-compat lever confirmed: names *and* numbers both load. |
| `src/routing/States.cpp:1733-1747` | Ephemeral/infra bridges are filtered out of slots: `if (infraIsBridge(node1, node2)) continue;` / `if (isEphemeralConnection(node1,node2)) continue;`, with the comment "persisting them is how phantom power claims got into slots". **persist** | **Design decision**: a user-placed virtual ADC must be a *persistent* bridge (menu path uses plain `addBridgeToState`), while its TDM plumbing must be ephemeral/infra so it never lands in a slot. Mirror what FakeGPIO does. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/remembering/FileParsing.cpp:1188-1195` | `printNodeFile()` display path, number→name: `specialFunctionsString.replace("110","ADC0"); … replace("114","ADC4"); replace("115","PROBE_MEASURE");` **B** | Substring replacement on decimal ids — adding 3-digit ids near existing ones is hazardous (see next row). |
| `src/remembering/FileParsing.cpp:2573-2592` | `replaceSFNamesWithDefinedInts()` parse path, name→number, **order-sensitive**: `replace("ADC0_8V","110") … replace("ADC4_5V","114"); replace("PROBE_MEASURE","115"); replace("115","139"); replace("ADC0","110") … replace("ADC4","114"); replace("PROBE_MEASURE","139"); … replace("ADC_0","110") … replace("ADC_7","115");` **persist** | Note the live `replace("115","139")` at 2579: any literal `115` in an old node file becomes BUFFER_IN before the ADC_7 rule at 2592 can produce a 115. Pre-existing hazard; **every new virtual id must be audited against this whole 2515-2600 block** or a legacy load will corrupt it. |
| `src/remembering/FileParsing.cpp:1234` | dead: `// specialFunctionsString.replace("132", "ADC_PAD");` (the old ADC_PAD=132 numbering, cf. `JumperlessDefines.h:409`). | — |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/config.h:303-318` | `[logo_pads]`: `X(logo_pads, top_guy, INT, 0, …, padNodeTable, …)`, `bottom_guy` (305), `building_pad_top` (307), `building_pad_bottom` (309) + four `*_idle` rows (311-317). These are the **config keys the ADC_PAD chooser writes**. **persist** | Values are `padNodeTable` indices, not node ids. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/configManager.h:491-517` | `padNodeTable[]`: `{"off",-1}, {"choose",-2}, {"uart_tx",0}, {"uart_rx",1}, {"adc_0",2}, {"adc_1",3}, {"adc_2",4}, {"adc_3",5}, {"adc_4",6}, … {"buffer_out",64}` — comment "Values keep the legacy arbitraryFunctionTable numbering … nodes that never had a number get 60+". **persist** | Add e.g. `{"adc",65}` meaning auto-assign; keep `adc_0..adc_4` parsing so old `.txt` configs load. |
| `src/configManager.h:597-604` | `arbitraryFunctionTable[]` duplicates `{"adc_0",2}…{"adc_4",6},{"adc_5",7}` — note **`adc_5` exists only here**, and `resolveLogoPadAssignment` maps `case 7: return ADC4; // legacy "adc_5"` (`src/Probing.cpp:1751`). **persist** | Keep the alias or old config files silently shift. |
| `src/config.h:271-294` | `[calibration] adc_0_zero/adc_0_spread … adc_4_* … adc_7_*` (12 floats). **C** | Per-physical-channel calibration; must stay physical. A virtual channel's reading must be scaled by whichever *physical* channel currently serves it. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/remembering/PersistentStuff.cpp:56-61, 93-98, 225-228` | Mirrors `calibration.adc_N_zero/spread` in and out of `g_store` and `adcSpread[]`. **C** | No change. |
| `src/remembering/PersistentStuff.cpp:344-365` | `logo_pads.*` ↔ `logoTopSetting[2]` / `logoBottomSetting[2]` / `buildingTopSetting[2]` / `buildingBottomSetting[2]`. **persist** | No change beyond the new table value. |
| `src/config.h:169-175` | `[measurement] net_currents / current_flow / show_probe_current / crosspoint_resistance` — no ADC-channel keys. | — |
| `src/config.h:227` | `[debug] net_scan_pair_taps`: "Tap both ends of a routed path at once on two ADCs (1, default) or sequentially (0)." **C** | No change, but it competes for the same pool. |
| `src/config.h:419-423` | `[usb_audio] left` default **0**, `right` default **1**, range 0-7. **A** | Physical channels; see Open questions on collision. |

---

## 7. Display / listing

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Peripherals.cpp:1098-1189` | `rebuildShownReadings()` — the central "which net is each ADC on" map. Clears `showADCreadings[0..7]`, then walks **paths** (comment 1120: "We can't use nodeToNetIndex because ADC nodes might not be in nets[].nodes[] arrays"), skips TDM paths (`int tdmAdcNode = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : -1;` 1127, then the `isFakeGpioNet` continue at 1143), then `if ( n1 == ADC0 \|\| n2 == ADC0 ) { showADCreadings[0] = pathNet; }` ×5 (1147-1161). **B — the single biggest rework** | `showADCreadings[]` is **indexed by physical channel** and holds one net each. With N virtual channels sharing one physical channel this array can no longer be the display key. Needs a per-virtual-slot array (exactly like `fakeGpioInputs[slot].netIndex` + `tdmSlot`). |
| `src/Peripherals.cpp:328,331,332,316` | `float adcReadings[8]`, `int showADCreadings[8]`, `uint32_t adcReadingColors[8]`, `float adcRange[8][2] = {{-8,8},{-8,8},{-8,8},{-8,8},{0,5}}`. **B** | All three parallel arrays are physical-channel-indexed and must gain virtual-slot twins (or become slot-indexed). |
| `src/Peripherals.cpp:1215-1290` | `showLEDmeasurements()` — per channel `i`: `adcReading = readAdcVoltage(i, samples); adcReadings[i] = adcReading;` (1246-1247), `color = measurementToColor( adcReading, adcRange[i][0], adcRange[i][1] );` (1251), `lightUpNet( showADCreadings[i], -1, 1, brightness, 0, 0, color );` (1266), `adcReadingColors[i] = color;` (1281). **B** | Must iterate virtual slots, reading each slot's last TDM sample rather than doing a live per-physical-channel read. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/hardwarestuff/RoutableGpio.cpp:1137-1156` | `int anyAdcConnected( int net )` — `for (i=0..7) if ( showADCreadings[i] == net ) return i;` (net form) / `if (showADCreadings[i] > 0 && showADCreadings[i] <= numberOfNets && i != 7) return i;` (net==-1 form). Declared `src/hardwarestuff/RoutableGpio.h:69`. **B** | Returns a *physical channel number* that callers print as "ADC n". Signature must return a slot/handle. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/eyecandy/Highlighting.cpp:1644, 1877-1896` | Probe/clickwheel net readout: `int adc = anyAdcConnected( netHighlighted );` then `float adcV = readAdcVoltage( adc, 32 ); snprintf( name, sizeof(name), "ADC %d", adc ); snprintf( value, …, "%0.2f V", adcV ); showNetReading( name, value, netCurrentValue(...) );` **B — this is the user-visible "ADC 2" label** | Label must become slot- or node-based. Also `readAdcVoltage(adc,32)` is a live physical read — under TDM it must read the slot's cached sample instead. |
| `src/eyecandy/Highlighting.cpp:2082-2098` | Second copy of the same readout: `int adcChannel = anyAdcConnected( showReadingNet ); float currentAdcReading = readAdcVoltage( adcChannel, 64 ); … snprintf( name, …, "ADC %d", adcChannel );` **B** | Same change, twice. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/NetManager.cpp:1229-1246` | Net-list scan: `if ((nets[i].nodes[j] >= ADC0 && … <= ADC4) \|\| … == ADC7 ) { showVoltage = 1; netsShowingSpecial[i] = 1; }`, and the FakeGPIO analogue `netsShowingSpecial[i] = 6; netsFakeGpioSlot[i] = nodes[j] - FAKE_GP_IN_0;` (1246-1251). **B** | The FakeGPIO branch is the template: virtual ADCs need their own `netsShowingSpecial` type + slot field. |
| `src/routing/NetManager.cpp:1391-1411` | Column headers: `stream->print("ADC / GPIO");` / `stream->print(" Voltage");` / `stream->print(" GPIO");`. **B** | Cosmetic. |
| `src/routing/NetManager.cpp:1439-1450` | `for (int k = 0; k < 8; k++) { if (nets[i].nodes[j] == ADC0 + k \|\| nets[i].nodes[j] == RP_GPIO_1 + k) { gpioOrAdcNumber = k; break; } }` — recovers the channel number for display. **B** | Replace with a slot lookup. |
| `src/routing/NetManager.cpp:1633-1645` | `// Regular ADC - display with color` → `float voltage = adcReadings[gpioOrAdcNumber]; … measurementToColor(voltage, -8.0, 8.0); … stream->print(voltage, 2); stream->print(" V");` and the FakeGPIO branch at 1649-1662 reading `tdmInputs.channels[tdmSlot].lastVoltage`. **B** | Virtual ADCs should take the FakeGPIO branch's shape. |
| `src/routing/NetManager.cpp:1746-1751` | Change detection: `for (i<8) if (fabs(lastADC[i] - adcReadings[i]) > 0.02) changed = 1;` (with `float lastADC[8]` at 1192, seeded 1198) plus the fake-GPIO loop at 1753-1761. **B** | Add virtual-slot voltages to the change detector or the listing goes stale. |
| `src/routing/NetManager.cpp:775-781` | Net lookup fallback: `// showADCreadings[] is sized 8 but only 0-4 are ever written (ADC0-ADC4); 0 means "not on a net".` → `static const int adcFallbackNodes[5] = {ADC0,ADC1,ADC2,ADC3,ADC4}; for(i<5) if (node == adcFallbackNodes[i] && showADCreadings[i] > 0) return showADCreadings[i];` **B** | Needs a virtual-node arm, or `findNetForNode(virtualAdc)` returns -1 and PartLabels/LEDs go blind. |
| `src/routing/NetManager.cpp:2044-2118` | `printNodeOrName(node, longOrShort, netIndex, stream)` — comment 2049 "netIndex: when >= 0, used to disambiguate shared ADC/voltage nodes"; the FakeGPIO-input arm at 2087-2115 maps `ADC0 + fakeGpioInputAdcChannel` back to `FGPI<slot>` using `netIndex`, falling back to `"FGPI*"` when several slots are active. **B — exact precedent** | Virtual ADCs need the identical disambiguation (`node → slot` via `netIndex`), otherwise every virtual ADC prints as the same physical `ADC_n`. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/JsonState.cpp:206-212` | `if ((node >= ADC0 && node <= ADC4) \|\| node == ADC7) { specialType = "ADC"; gpioOrAdcIndex = (node == ADC7) ? 7 : (node - ADC0); voltage = adcReadings[gpioOrAdcIndex]; … }`, vs the FakeGPIO arm at 233-245 using `tdmSlot` → `tdmInputs.channels[tdmSlot].lastVoltage`. Emitted at 274-280 as `"special": "ADC", "voltage": …`. **B (machine-readable API)** | Add a virtual arm; decide whether `"special"` stays `"ADC"` (keeps consumers working) with a new slot field. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Graphics.cpp:2972-2991` | `extern int anyAdcConnected(int net); extern uint32_t adcReadingColors[8]; int adcChannel = anyAdcConnected(actualNet); bool isAdcNet = (adcChannel >= 0 && adcChannel < 8);` then the FakeGPIO exclusion loop `for (int fakeIdx = 18; fakeIdx < 50; fakeIdx++) if (gpioNet[fakeIdx] == actualNet …) isAdcNet = false;` **B** | The `18..50` magic window is how fake inputs are kept out of ADC coloring — virtual ADCs need an equivalent (or, better, the whole thing keyed on slot). |
| `src/Graphics.cpp:3135-3147` | `if (isAdcNet) { __dmb(); uint32_t adcColor = adcReadingColors[adcChannel]; if (adcColor != 0) { for (i<5) { frameColors[i] = adcColor; brightenedNodeColors[i] = adcColor; } } }` **B** | Same. |
| `src/Graphics.cpp:2499-2504` | `… ISENSE_PLUS, ISENSE_MINUS, ADC0, ADC1, ADC2, ADC3, ADC4, ADC7}` — a "special nodes" list used for row painting. **B** | Extend with the virtual range. |
| `src/Graphics.cpp:4405, 4419-4420, 4651-4660, 5620` | Terminal-art board render: `snprintf(screenLines[currentLine++], LINE_WIDTH, "           ADC  ")`, `{ADC_LED_0, "AD", …, ADC_0}, {ADC_LED_0, "C ", …, ADC_1}`, `uint32_t adc0C = leds.getPixelColor(ADC_LED_0) \| 0x00000f;`. These are the **logo pad LEDs**, not channels. **C** | No change. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/LEDs.cpp:1553-1561` | Net color priority: `// HIGHEST PRIORITY: Real-time measurements` → `for (int a = 0; a < 8; a++) { if (i == showADCreadings[a]) { globalState.connections.nets[i].color = unpackRgb(adcReadingColors[a]); netColors[i] = …; showingReading = 1; continue; } }` **B** | Keyed on `showADCreadings[]`; follows whatever that array becomes. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/eyecandy/SyntaxHighlighting.cpp:167` | `"GPIO_PAD", "DAC_PAD", "ADC_PAD",` in the known-identifier list. **B** | Add new ADC names. |
| `src/eyecandy/SyntaxHighlighting.cpp:434` | `return (nodeNum >= 1 && nodeNum <= 150);  // Valid node number range` — **a 150 ceiling** on what highlights as a node. **B** | Must be raised for any new id ≥ 151 (this already mis-handles FAKE_GP_* 150-189). |
| `src/eyecandy/ReadingDisplay.cpp/.h` | **Zero** `adc` matches; API is `void show(const char* name, int rowNode, const char* value = nullptr, …)` (`ReadingDisplay.h:36`). **B** | Nothing to change — it takes whatever name the caller supplies. Good target for a unified label. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/MatrixState.cpp:282-300` | `struct nodeStruct nodeNames[30]` incl. `{"adc_0", ADC0}, {"adc_1", ADC1}, {"adc_2", ADC2}, {"adc_3", ADC3}, {"adc_4", ADC4}, {"probe", ADC7_PROBE}` + `int printOrder[30]` (291) → printed by the loop at 314-318 (`Serial.print(nodeNames[printOrder[i]].name)`). Declared `src/routing/MatrixState.h:130`. **B (special-node listing)** | 30-element fixed table with a hand-built print order; adding virtual entries means resizing both. |
| `src/routing/MatrixState.cpp:548-555` | `{"ADC0_5V",110},{"ADC1_5V",111},{"ADC2_5V",112},{"ADC3_8V",113},{"ADC0",110}…{"ADC3",113}` — yet another name table. **B** | Third independent ADC name table (after `specialDefines[]` and modjumperless's). Any rename must hit all three. |
| `src/unused/tuiNetHelpers.h` | 10 ADC hits, directory named `unused`. **not read in detail** | Presumed dead; verify before relying on it. |

---

## 8. Internal ADC users that must keep working untouched

| site | what |
|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/sensing/PartMeasure.cpp:352-366` | `for (c…) if (nodeHasAnyBridgePM(ADC0 + c)) mask &= (uint8_t)~(1u << c); int ch = infraAcquireAdc(INFRA_ADC_SCAN, mask, false); … s.adcCh[i] = ch;` — three simultaneous channels, one per part leg. |
| `src/sensing/PartMeasure.cpp:389, 409, 416, 465, 504, 518, 542-543, 582-592` | `legAdd(s, s.rows[i], ADC0 + s.adcCh[i]);` and `readAdcVoltage(s.adcCh[i], 8)` throughout. |
| `src/sensing/PartMeasure.cpp:455` | `infraReleaseAdc(INFRA_ADC_SCAN);` |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/PartsApp.cpp:3392-3431, 3560-3659` | `if (partsNodeWired(ADC0 + c)) adcMask &= ~(1u<<c); int adcCh = infraAcquireAdc(INFRA_ADC_SCAN, adcMask, false);` … `addBridgeToState(ADC0 + adcCh, gndRow)` … `infraReleaseAdc(INFRA_ADC_SCAN)` (3659). Also the failure message at 4638 `"no clean measurement lane - every free ADC"`. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/sensing/NetVoltageScan.cpp:640-642, 660, 1204-1209, 1347, 1482` | `adcA = infraAcquireAdc(INFRA_ADC_NVSCAN, (uint8_t)(1u << prefer), true); if (adcA < 0) adcA = infraAcquireAdc(INFRA_ADC_NVSCAN, 0x0F, true);`, `pickScanAdc()` = `infraAcquireAdc(INFRA_ADC_NVSCAN, 0x0F, true)`. Ephemeral routes computed per poll via `fastConnectPath(node1, ADC0 + adcA, &h1, 500)` (372). |
| `src/sensing/NetVoltageScan.cpp:749, 765, 818, 844, 898-900` | `if (node >= ADC0 && node <= ADC4) return false; // filled from adcReadings[]`; `adcNodePresent[node - ADC0] = true;` → `nodeVoltage[ADC0 + a] = adcReadings[a];`. **This is a place a virtual ADC node would fall through** — it only special-cases 110-114. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/sensing/TimeDomainMultiplexer.cpp:313-352` | `// ADC0=X8, ADC1=X9, ADC2=X10, ADC3=X11`, `if (adcChannel < 0 \|\| adcChannel > 3) return 8;`, `adcChannel = infraAcquireAdc(INFRA_ADC_TDM, 0x0F, false);`, and the migration path `int newAdc = infraAcquireAdc(INFRA_ADC_TDM, (uint8_t)(0x0F & ~(1u << adcChannel)), false);` when a user claims the current one. |
| `src/sensing/TimeDomainMultiplexer.cpp:206-243, 294-298` | Full-path disconnect/reconnect per slot, then `// Disconnect the ADC after reading so the TDM's ADC doesn't stay connected to this channel between polls`. `TDM_MAX_CHANNELS 32` (`TimeDomainMultiplexer.h:23`). |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/sensing/FakeGpio.cpp:44,75,661-668` | `int fakeGpioInputAdcChannel = -1;` / `fakeGpioInputAdcChannel = tdmInputs.adcChannel;` / `if (tdmInputs.adcChannel < 0) { if (tdmInputs.assignFreeAdc() < 0) { Serial.println("ERROR: fakeGpioConfigInput: no free ADC (0-3 all in use)"); return -1; } }`. **Confirmed: ALL 32 fake inputs share ONE physical channel** (`tdmInputs.adcChannel` is a single field) — this is the redesign's precedent and its ceiling. |
| `src/sensing/FakeGpio.cpp:1129-1160` | `finalizeFakeGpioAfterRouting()` — same single-ADC assignment post-routing, `"WARNING: finalizeFakeGpioAfterRouting: no free ADC"`. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/InfraPaths.cpp:814-829` | `bool infraAdcUserClaimed(int adcChannel)`: `if (adcChannel < 0 \|\| adcChannel > 4) return true; int adcNode = ADC0 + adcChannel;` then over all bridges `if (n1 != adcNode && n2 != adcNode) continue; if (IS_FAKE_GP_IN(n1) \|\| IS_FAKE_GP_IN(n2)) continue; // FakeGPIO input bridges are the TDM's own plumbing, not a user claim; if (globalState.isEphemeralConnection(n1,n2)) continue; return true;` **The rule the redesign hinges on** — a *persistent user bridge* on `ADC0+ch` locks that channel out of the pool entirely. |
| `src/routing/InfraPaths.cpp:831-851` | `infraAcquireAdc(user, mask, allowSharedTdm)` — keep-if-still-free, then first free-and-unclaimed, else `if (allowSharedTdm && s_adcOwner[adc] == INFRA_ADC_TDM …) sharedFallback = adc; // ride along, ownership stays TDM's`. Only NVSCAN passes `allowSharedTdm=true`. |
| `src/routing/InfraPaths.h:159-200` | `InfraAdcUser { INFRA_ADC_NONE, INFRA_ADC_TDM, INFRA_ADC_NVSCAN, INFRA_ADC_MEASURE, INFRA_ADC_SCAN, INFRA_ADC_GUIDE }` + `// Acquire a free ADC channel out of candidateMask (bit N = ADCN, ADC0-4)`; `bool infraAdcUserClaimed(int adcChannel);` |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/guiding/GuideChecks.cpp:517-543, 649` | Ring-based pair read `adcRingMeanWindow(chA, …)` with per-channel `adcSpread[ch]/adcZero[ch]` (`const float zeroA = (chA != 4 && chA != 5) ? adcZero[chA] : 0.0f;`), and `infraReleaseAdc(INFRA_ADC_GUIDE);`. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/tubes/USBAudio.cpp:49, 209-210` | `volatile bool usbAudioOwnsAdc = false;  // Set once the stream owns the ADC; read by readAdc()/updateLazyAdcReadings()`; `l += ring[(base + g_chL) & (ADC_RING_HALFWORDS-1u)] & 0x0FFFu; r += ring[(base + g_chR) …]`. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/selfreflection/SelfTest.cpp:595-683` | `// … with a rotating ADC0-3 return path`, `addBridgeToState( ADC0 + adcCh, row );` (626, 655), `Serial.printf("  row %2d ADC%d %5.3fV …")` (634), `addBridgeToState( rail == 0 ? TOP_RAIL : BOTTOM_RAIL, ADC2 + rail );` (683), plus ADC7 two-point self-cal at 779-812. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/Apps.cpp:2118-2136` and `2333-2349`, `2614-2639` | `calibrateDacs()`: `addBridgeToState( DAC0, ADC0 ); … addBridgeToState( DAC1, ADC1 ); … addBridgeToState( TOP_RAIL, ADC2 ); … addBridgeToState( BOTTOM_RAIL, ADC3 );` — hardwired physical pairings. |
| `src/Apps.cpp:1407-1427` | Xbar-route test: `addBridgeToState( ADC0, 20 ); … float voltage = readAdcVoltage( 0, 8 ); Serial.print("\n\rADC0: "); … removeBridgeFromState( ADC0, 20 );` |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/boards/v5/board_v5.cpp:82-89` | `{ADC0,"ADC0",-8.0f,8.0f,true}, … {ADC4,"ADC4",0.0f,5.0f,true}, {ADC7,"ADC7",-8.0f,8.0f,true}` — per-channel range/buffered flags. |
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/MatrixState.cpp:147, 153` | `{29, 59, ROUTABLE_BUFFER_IN, NANO_AREF, TOP_RAIL, BOTTOM_RAIL, DAC1, DAC0, ADC0, ADC1, ADC2, ADC3, CHIP_L, CHIP_I, CHIP_J, GND}` (chip K x-map, ADC0-3 = x8-x11) and `{30, 60, ROUTABLE_BUFFER_OUT, ADC4_5V, …}` (chip L, ADC4 at x3). **ADC7 appears in no x-map** — it is not routable. |
| `lib/micropython/port/machine_adc_jl.c` | Raw pico-sdk `adc_select_input/adc_read` on GPIO26-29; bypasses the crossbar and the pool entirely. |
| **Scope app** | `grep -i scope src/Apps.cpp` → **no matches**. `menuTree.h:135` has `//   "-Oscill oscope",` commented out. HelpDocs.cpp:1283 still advertises it. **There is no oscilloscope app.** |

---

## 9. RouteSafety

| site | what | change needed |
|---|---|---|
| `/Users/kevinsanto/Documents/GitHub/JumperlOS-main/src/routing/RouteSafety.cpp:61-68` | `static bool isHighZNode(int node) { return (node >= ADC0 && node <= ADC4) \|\| node == ROUTABLE_BUFFER_IN \|\| node == ISENSE_PLUS \|\| node == ISENSE_MINUS \|\| node == RP_UART_RX \|\| node == NANO_AREF; }` — consumed by `allocWire()`: `wireIsHighZ[id] = (node > 0 && isHighZNode(node)) ? 1 : 0;` (72). **Must EXTEND, not replace** | If virtual ADC nodes are not added here, the short-checker treats them as ordinary (potentially driven) wires and will reject or mis-classify legal sense taps. Add the new contiguous range with an `\|\| (node >= VADC_0 && node <= VADC_n)`. |
| `src/routing/RouteSafety.cpp:53-59` | `isDrivenSourceNode()` — GND/rails/DACs/BUFFER_OUT/UART_TX/GPIO20-27. Virtual ADCs must **not** be added here. | — |
| `src/routing/RouteSafety.cpp:575-660` | `buildEphemeralRoute()`: `// Prefer: nodeB is ADC on chip K (the common sense-tap case)`, `if (adcChip == CHIP_K && !laneOk(CHIP_K, adcX)) return false;` (658). Comment at 579: "y-rows are the ONLY gateway to ADC0-3". **C** | Works on physical chip/pin, so it keeps working once the virtual node has been resolved to a physical ADC — the resolution must happen *before* this is called. |
| `src/routing/RouteSafety.cpp:782` | `// Generic same-chip or refuse — non-ADC pairs not needed yet for scanner` | Note the builder only handles ADC-terminated pairs. |
| `src/routing/RouteSafety.cpp:965-966` | `// Includes the ADC columns x8-x11: buildEphemeralRoute's very first gate is laneOk(CHIP_K, adcX), so a busy ADC column refuses every…` **C** | — |
| `src/routing/RouteSafety.h:85` | `// ADC0-3, and laneOk(CHIP_K, adcX) is the router's FIRST gate` | — |
| `src/routing/RouteSafety.cpp:243, 296, 607, 801, 859-887` | `nodeToNetIndex[node]` with `node >= 0 && node < 256` guards throughout. **C** | Reconfirms the <256 id ceiling. |
| part_safety | `grep part_safety` → no such file/symbol; part-related safety lives in `PartMeasure.cpp` / `PartsApp.cpp` (see §8). |

---

## 10. Node id space (src/JumperlessDefines.h)

**Taken, 96-199** (from `src/JumperlessDefines.h:394-560`, plus `:211`):

| ids | symbol |
|---|---|
| 69, 96-99 | `NANO_VIN 69`, `NANO_GND_1 96`, `NANO_GND_0 97`, `NANO_3V3 98`, `NANO_5V 99` |
| 100-105 | `GND 100`, `TOP_RAIL 101`, `BOTTOM_RAIL 102`, `SUPPLY_3V3 103`, `TOP_RAIL_GND 104`, `SUPPLY_5V 105` |
| 106-109 | `DAC0 106`, `DAC1 107`, `ISENSE_PLUS 108`, `ISENSE_MINUS 109` |
| 110-115 | `ADC0 110`…`ADC3 113`, `ADC4/ADC4_5V 114`, `ADC7/ADC7_PROBE 115` |
| **114 collision** | `src/JumperlessDefines.h:433-434` `#define ADC4 114 / #define ADC4_5V 114` **and `:442` `#define RP_GPIO_0 114`** — same id, two meanings. Flag before adding anything nearby. |
| 116-121 | `RP_UART_TX/RP_GPIO_16 116`, `RP_UART_RX/RP_GPIO_17 117`, `RP_GPIO_18 118`, `RP_GPIO_19 119`, `SUPPLY_8V_P 120`, `SUPPLY_8V_N 121` |
| 122-125 | **free** — but occupied by `{"NONE","NONE",122..125}` placeholders in `specialDefines[]` (`src/routing/NetManager.cpp:122-125`) |
| 126, 127 | `BOTTOM_RAIL_GND 126`, `EMPTY_NET 127` |
| 128-130 | **free** (128 collides textually with `MAX_BRIDGES 128`, different namespace) |
| 131-138 | `RP_GPIO_1..8` == `RP_GPIO_20..27` (dual names, same ids) |
| 139-140 | `ROUTABLE_BUFFER_IN 139`, `ROUTABLE_BUFFER_OUT 140` |
| 141 | **free** |
| 142-148 | `LOGO_PAD_TOP 142`, `LOGO_PAD_BOTTOM 143`, `GPIO_PAD 144`, `DAC_PAD 145`, `ADC_PAD 146`, `BUILDING_PAD_TOP 147`, `BUILDING_PAD_BOTTOM 148` |
| 149 | **free** |
| 150-157 | `FAKE_GP_OUT_0..7` |
| 158-189 | `FAKE_GP_IN_0..31` (`FAKE_GP_IN_BASE 158`, `MAX_FAKE_GP_IN 32`) — note `specialDefines[]` only names **FGPI0-23 (158-181)**; **182-189 are already unnamed** |
| 190-198 | **free** (9 ids) |
| 199 | `BOUNCE_NODE 199` (`src/JumperlessDefines.h:211`) |
| 200-255 | **free** (56 ids) |

**Recommended range: 200-231 (32 ids), or 208-223 (16).** It is the only contiguous ≥16 block. 190-198 is 9 — too small. Reusing 122-125/128-130/141/149 would give a fragmented 9.

**Constraints on any new id:**

| constraint | site | note |
|---|---|---|
| id **< 256**, hard | `src/routing/NetManager.cpp:27` `int8_t nodeToNetIndex[256];` (writer at `:39`), consumers `src/LEDs.cpp:1153,1206` `extern int8_t nodeToNetIndex[256];`, `src/routing/RouteSafety.cpp:296,607,801,859` `node < 256` guards, `src/sensing/MeasureMode.cpp:236` | 200-231 fits. |
| id **≤ 200**, must be raised | `src/routing/States.cpp:2745-2748` `if (val >= 0 && val <= 200) { return val; }` in `parseNodeName()` | **Blocks 201-255.** Either use 190-198+ a raised cap, or raise the cap to 255 (safe: `nodeToNetIndex` is the real ceiling). |
| id **≤ 150**, syntax highlighting only | `src/eyecandy/SyntaxHighlighting.cpp:434` `return (nodeNum >= 1 && nodeNum <= 150);` | Cosmetic; already wrong for 150-189. |
| `definesToChar` fallback only covers **100-148** | `src/routing/NetManager.cpp:2190` `} else if (defined >= 100 && defined <= 148) {` | New ids must be in `specialDefines[]` or print as bare numbers. |
| `specialDefines[]` count is **hardcoded 70** | `src/routing/States.cpp:2769` `const int numSpecialDefines = 70;` (comment: "updated to 70 elements (added 32 FAKE_GPIO, removed 7 PAD defines, was 49)") | Must be bumped by hand in lockstep — the table itself visibly ends at FGPI23. |
| `nodesToPixelMap[120]` | `src/LEDs.h:353` `const int nodesToPixelMap[120]`, guarded at `src/Graphics.cpp:4681` `int ogPix = (node >= 0 && node < 120) ? nodesToPixelMap[node] : -1;` | New ids ≥120 need that guard (already the norm for 131-189). |
| `MAX_NODES` is **nodes-per-net**, not id space | `src/JumperlessDefines.h:192/195` `#define MAX_NODES 24` (OG) / `40` (V5) | Do not conflate. |
| shared-net trick for TDM | `src/JumperlessDefines.h:562-566` `#define FAKE_GPIO_TDM_NET (MAX_NETS - 2)  // 58` with `// All FAKE_GP_IN paths temporarily use this net so they can share paths, then get restored to their individual nets after routing completes. Must be < MAX_NETS to fit in int8_t yStatus` | A virtual-ADC family needs its own equivalent (or must reuse this one). Only `MAX_NETS-1` is left. |
| `nodeNames[30]` / `printOrder[30]` fixed size | `src/routing/MatrixState.cpp:282,291`, `MatrixState.h:130` | Resize both. |

---

## Surfaces the redesign must NOT change

1. **The infra ADC pool contract** — `src/routing/InfraPaths.cpp:814-829` (`infraAdcUserClaimed`), `:831-851` (`infraAcquireAdc` incl. the `allowSharedTdm` ride-along), `:854-858` (`infraReleaseAdc`), `src/routing/InfraPaths.h:159-200`. Every internal consumer depends on it. If a "virtual ADC" becomes a persistent user bridge on a *physical* ADC node, `infraAdcUserClaimed` will lock that channel out of the entire pool — the virtual bridge must therefore be on a **new node id** and be excluded there the same way `IS_FAKE_GP_IN` is.
2. **TDM mechanics** — `src/sensing/TimeDomainMultiplexer.cpp:313-352` (`0x0F` mask, ADC0-3 = x8-x11), `:206-243` (full-path switch), `:294-298` (disconnect after read). Extend/parameterize, don't rewrite.
3. **FakeGPIO** — `src/sensing/FakeGpio.cpp:661-668`, `:1129-1160`; one physical channel serves all 32 inputs.
4. **Part measurement / classification** — `src/sensing/PartMeasure.cpp:352-366, 389, 416, 455`; `src/PartsApp.cpp:3392-3431, 3659`. Needs up to 3 exclusive physical channels simultaneously.
5. **Net voltage scan** — `src/sensing/NetVoltageScan.cpp:640-642, 1204-1209`; ephemeral, per-poll, ride-along-capable.
6. **Guided placement** — `src/guiding/GuideChecks.cpp:517-543, 649`.
7. **Measure mode's pool discipline** — `src/sensing/MeasureMode.cpp:337-341, 395`.
8. **Calibration** — `src/Apps.cpp:2118-2136` (DAC0→ADC0, DAC1→ADC1, TOP_RAIL→ADC2, BOTTOM_RAIL→ADC3), `src/config.h:271-294` (`adc_N_zero/spread`), `src/remembering/PersistentStuff.cpp:56-61, 225-228`, `src/selfreflection/SelfTest.cpp:626, 683, 779-812`.
9. **USB audio** — `src/tubes/USBAudio.cpp:49, 209-210`; physical ring lanes.
10. **`machine.ADC`** — `lib/micropython/port/machine_adc_jl.c:95-140`; raw pico-sdk, unrelated to routing.
11. **Hardware maps** — `src/routing/MatrixState.cpp:147, 153` (chip K x8-11 / chip L x3), `src/boards/v5/board_v5.cpp:82-89` (ranges & buffered flags), `src/routing/RouteSafety.cpp:625-660` (the K-lane gate).
12. **`isHighZNode`** — `src/routing/RouteSafety.cpp:61-68`: **extend** the predicate; removing 110-114 from it would break every internal sense tap.
13. **Existing MicroPython symbols** — `modules/jumperless/modjumperless.c:393-410, 4597-4602, 6530-6535` must keep resolving `ADC0`→110 etc.
14. **Slot-file name round-trip** — `src/routing/States.cpp:1764-1772` + `:2793-2806` + `:1787-1790`.

---

## Open questions

1. **Do virtual ADCs get their own node ids, or reuse 110-114?** The whole design hinges on this. Reuse breaks `infraAdcUserClaimed` (`InfraPaths.cpp:819-826`) — a persistent user bridge on `ADC0` removes that channel from the internal pool permanently. New ids (200-231) are the FakeGPIO-shaped answer, but require the `parseNodeName` ≤200 cap (`States.cpp:2746`) to be raised and `numSpecialDefines = 70` (`States.cpp:2769`) to be bumped.
2. **Old slot files containing `n1: ADC0` / `n1: 110`** — remap to a virtual channel on load, or keep them meaning the physical channel forever (two classes of ADC node coexisting)? `serializeBridges` writes names (`States.cpp:1768`) so both are decidable at load time.
3. **Legacy `.txt` node files**: `src/remembering/FileParsing.cpp:2578-2579` does `replace("PROBE_MEASURE","115"); replace("115","139");` — ordered decimal substring replacement. Any new 3-digit id must be audited against the entire `replaceSFNamesWithDefinedInts()` block (2515-2600) or old files will corrupt on load. (This also means "200" would collide with a literal "200" anywhere in the string.)
4. **How many virtual channels can one physical ADC actually serve?** `TDM_MAX_CHANNELS 32` (`TimeDomainMultiplexer.h:23`) and FakeGPIO already consumes the whole TDM instance on one channel (`FakeGpio.cpp:661-668`). Do virtual ADCs share that TDM instance with fake GPIO inputs, or get a second instance on a second physical channel? Two instances would leave only 2 channels for measure/scan/guide.
5. **USB-audio collision.** `[usb_audio] left` defaults to 0, `right` to 1 (`config.h:419-423`); the audio path reads the free-running ring per physical channel (`USBAudio.cpp:209-210`), and NVScan also reads the ring (`NetVoltageScan.cpp:208-217`). If the virtual-ADC TDM lands on channel 0 or 1, audio streams a time-sliced mix of unrelated rows. Force TDM off the audio channels, or make `infraAcquireAdc` aware of `usbAudioOwnsAdc` (`USBAudio.cpp:49`) — it currently is not.
6. **`ADC7`/"P" in both pickers** (`Probing.cpp:5106`, `adcMap` 2222/2514) is the probe tip and appears in **no crossbar x-map** (`MatrixState.cpp:147/153`), so "connecting" it is meaningless. Drop it from the picker, or keep a probe-tip readout as a separate concept?
7. **Two different readout labels today**: measure mode shows the *node* name (`MeasureMode.cpp:445`), the highlight panel shows `"ADC %d"` (`Highlighting.cpp:1891, 2094`). Which wins after the redesign?
8. **`showADCreadings[8]` holds one net per physical channel** (`Peripherals.cpp:1147-1161`) and is the key for LED coloring (`LEDs.cpp:1553-1561`), net listing (`NetManager.cpp:1633-1645`), Graphics (`Graphics.cpp:2978`) and `anyAdcConnected` (`RoutableGpio.cpp:1137`). Replacing it is the largest single mechanical change; is a slot-indexed array acceptable, or should it stay physical with a parallel slot table?
9. **`ADC4 == RP_GPIO_0 == 114`** (`JumperlessDefines.h:433/442`) — is `RP_GPIO_0` live anywhere? If so, the collision needs resolving before the id space is touched.
10. **OG board variant** — `src/boards/og/board_og.cpp` (11 ADC hits) and `src/routing/NetsToChipConnections_OG.cpp` (14 hits, incl. the FakeGPIO ADC expansion at 5360/5937/6104/6119) were **not read**. OG is RP2040 (ADC0-3 only, no buffer, no ADC7 — `Probing.cpp:7237`) and must be planned separately.
11. **`src/unused/tuiNetHelpers.h`** (10 ADC hits) — **not read**; confirm it is genuinely dead before ignoring it.
12. **`measureVoltage(int adcNumber, …)`** (`Commands.cpp:823`) does `removeBridgeFromState(adcDefine, -1)` — it wipes *all* connections to the channel it was handed. Its one caller passes a hardcoded `2` (`Apps.cpp:1796`). Does this API survive, and if so what does it destroy when channels are shared?
