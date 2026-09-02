<!-- Design-agent report from the 2026-09-02 ADC redesign session (chat b816f338). Line numbers cite the JumperlOS-main worktree at 5.7.10.0 and drift a few lines on dev; symbols are authoritative. -->

# A. Ephemeral-tap mechanism (NetVoltageScan + RouteSafety)

## A1. The tap primitive

**`static bool senseNodeVoltage(int node, int adc, float* volts)` — `src/sensing/NetVoltageScan.cpp:268`.** This is *the* primitive. It is `static`, so nothing outside the file can call it.

Sequence, with the decisive lines:

1. ADC lock (legacy converter path only). `NetVoltageScan.cpp:274-283`:
   `const bool ringLive = adcRingActive();   // T2.1: no converter to own - skip the lock` … `while (!ringLive && __atomic_test_and_set(&readingADC, __ATOMIC_ACQUIRE)) { if (micros() - waitStart > 2000) { tapFailAdcBusy++; return false; }` — 2 ms spin cap, encoder serviced inside it.
2. Route + close: `int rc = fastConnectPath(node, adcNode, &handle, 500);` (`:294`), `adcNode = ADC0 + adc` (`:287`). On `rc != 0` → `noteRouteFail(...); tapFailNoRoute++; return false;` (`:297-300`).
3. Settle: `waitServicingEncoder(80); // CH446Q settle (same value TDM landed on)` (`:302`).
4. Read. Ring path (`:305-333`): snapshot `gen = adcRingGeneration(); s0 = adcRingSweeps();` → `waitServicingEncoder(500);` → `s1 = adcRingSweeps();` → validity `ok = (adcRingActive() && adcRingGeneration() == gen && (s1 - s0) >= 17);` → `early = adcRingMeanWindow(adc, s0 + 9, 8)`, `late = adcRingMeanWindow(adc, s1, 8)`. So **8 samples early + 8 samples late out of one 500 µs dwell**. Legacy path (`:335-339`): `readScanAdcVoltage(adc, 4, &early)` → `waitServicingEncoder(250)` → `readScanAdcVoltage(adc, 4, &late)`.
5. Open: `fastDisconnectPath(&handle);` (`:341`), release lock (`:342`).
6. Floating verdict: `if (fabsf(late - early) > kFloatingDriftVolts) { tapFailDrift++; return false; // floating }` (`:352-355`), threshold `static const float kFloatingDriftVolts = 0.05f;` (`:264`). `*volts = late;` (`:357`).

`readScanAdcVoltage` (`:207`) is the ADC-read helper; in ring mode it is `adcRingMeanAfterStrict(channel, adcRingSweeps(), samples, samples*25+400, &fresh)` and **fails the read rather than return history**: `if (!fresh) { tapFailRingStale++; return false; }` (`:220-222`).

**Duration.** The author's own budget, `NetVoltageScan.cpp:288-293`:
> `// Budget: 2ms lock spin + 4x500us connect + unwind + ~0.6ms dwell (80us settle + 500us ring hold) + 4x1ms disconnect ~= 11ms hard worst case.`

Typical is much smaller: 80 µs settle + 500 µs dwell + 4 crosspoint sends ≈ **0.7–1.2 ms**. Cadence limiter is `static const unsigned long kScanIntervalUs = 5000; // min gap between taps` (`:81`) — **one tap per 5 ms, one tap per service pass**.

**Pair variant: `static bool pairSenseTap(int node1, int node2, int adcA, int adcB, float* v1, float* v2)` — `:369`.** Two `fastConnectPath`s, one 80 µs settle, one 500 µs dwell, both channels sliced from the *same* sweeps. Refuses hop-asymmetric pairs: `if (hops1 != hops2) { … tapPairAsym++; return false; }` (`:399-404`). Ring-only.

**Core.** Both run on the second core. Call chain: `loop1()` (`src/main.cpp:1553`) → `core2stuff()` (`:1708`) → `serviceNetVoltageScan()` (`src/main.cpp:2124`). The comment there: *"Still core 1 only (taps stay serialized with sendPaths); internally gated on the frame hold/sendAllPathsCore2…"* Serial output is deliberately kept off this core — `serviceNetVoltageScanDebug()` runs on core 0 (`src/JumperlOS.cpp:689`).

**ADC acquisition.** `static int pickScanAdc()` — `NetVoltageScan.cpp:1204-1210`: `return infraAcquireAdc(INFRA_ADC_NVSCAN, 0x0F, true);` — mask 0x0F = **ADC0-3 only**, acquired **per tap**, released immediately after (`infraReleaseAdc(INFRA_ADC_NVSCAN); // per-tap: don't hold between taps`, `:1539`). `allowSharedTdm = true`.

**Coexistence with TDM.** `infraAcquireAdc` (`src/routing/InfraPaths.cpp:831-852`): a free channel is taken exclusively; if none is free, `if (allowSharedTdm && s_adcOwner[adc] == INFRA_ADC_TDM && sharedFallback < 0) sharedFallback = adc; // ride along, ownership stays TDM's`. Safety of that ride is *positional*, not locked: both consumers run in the same `core2stuff()` pass, and if the TDM happens to have its channel closed, `buildEphemeralRouteTiered`'s first gate refuses the tap anyway — `if (adcChip == CHIP_K && !laneOk(CHIP_K, adcX)) return false;` (`RouteSafety.cpp:658`), because `laneOk` needs `xColumnFreeHW`. TDM's `pollNext` leaves it open (`disconnectActive()`, `TimeDomainMultiplexer.cpp:299`), so in practice the column is free between polls.

**USB audio.** `#if USB_AUDIO_ENABLE  if (usbAudioOwnsAdc) return;` in both the one-shot servicer (`NetVoltageScan.cpp:607-609`) and the scan body (`:1427-1437`), with the rationale: *"The USB microphone holds the ADC for as long as the host has it open… Stand down entirely rather than sneaking taps into the few moments the audio path lets go."*

## A2. Gating and servicing

Serviced from `core2stuff()` on the second core, **after `core_sync_release()`** (`src/main.cpp:2117-2124`) so a multi-ms tap cannot extend the LED frame hold: *"stacking it inside the frame was the 'clickwheel super laggy' regression."*

`serviceNetVoltageScan()` — `NetVoltageScan.cpp:1386`. Order of gates:

| Gate | Line | Behavior |
|---|---|---|
| `serviceOneShotTap()` **first** | `:1389` | one-shots serve even when the scan is off/paused |
| `jumperlessConfig.measurement.net_currents` | `:1392` | off → wipes `nodeVoltageMs`, `pathCurrentValid`, `netCurrentInfo`, returns |
| `inClickMenu != 0 \|\| inPadMenu != 0` | `:1413` | full pause in menus |
| `core1FramesHeld()` | `:1424` | flash/XIP hold |
| `!core1req::allIdle()` | `:1425` | a path send pending/in flight |
| `core1busy \|\| refreshInProgress` | `:1426` | core 0 mid-rebuild |
| `usbAudioOwnsAdc` | `:1436` | mic open |
| `millis() - lastUserInputMs < 100 \|\| isEncoderButtonPhysicallyPressed()` | `:1443-1446` | **user input preempts outright** |
| `probeActive == 0 && !wavegen.isRunning()` + `micros() - lastScanUs > kScanIntervalUs` | `:1478-1479` | tap slot itself |

The config key is `measurement.net_currents` (`src/config.h:169`), default 1. (The header text at `NetVoltageScan.h:98` says "display.net_currents" — stale name; the code reads `jumperlessConfig.measurement.net_currents`.)

`serviceOneShotTap()` (`:588`) drops only the *politeness* gates; it keeps every hardware gate (`:604-612`): `core1FramesHeld`, `core1req::allIdle`, `core1busy||refreshInProgress`, `usbAudioOwnsAdc`, `probeActive`, `wavegen.isRunning()`, and the shared `kScanIntervalUs` throttle. Comment at `:583-587`: *"a one-shot IS user-requested work (the guide is waiting on it), bounded at one tap per pass. The hardware-safety gates stay - they exist for route/ADC integrity, not politeness."*

`core2busy` brackets **only** the tap (`:1480-1481`, `:1540`), not the bookkeeping — `:1470-1477` explains that raising it for the whole body starved `OledGui::renderNow()`.

## A3. Data produced

- `float nodeVoltage[NODE_VOLTAGE_MAX]` / `uint32_t nodeVoltageMs[...]` / `NetCurrentInfo netCurrentInfo[...]` — defined `NetVoltageScan.cpp:66-68`, declared `NetVoltageScan.h:45-47`. `NODE_VOLTAGE_MAX 141` (`NetVoltageScan.h:33`).
- Freshness: `static const unsigned long kVoltageFreshMs = 5000;` (`:87`), tested by `voltageFresh(node, ms)` (`:945-947`). The 5 s window is deliberate (`:82-87`): *"probing/scrolling preempts the scanner (user-input gate), so a tight window aged the currents out MID-INTERACTION."*
- Writer: `recordTapVoltage(node, volts)` (`:912`), EMA α=0.3 when the previous value is fresh, seed otherwise (`:933-938`). `GND` is pinned and never written (`:917`).
- `computePathCurrents()` (`:959`) runs at 20 Hz (`if (millis() - lastComputeMs >= 50)`, `:1547`), fills `netCurrentInfo[p.net]` and `pathCurrent_mA/pathShown_mA`.
- Public readers: `nodeVoltageValid(node)` (`:1171`), `netCurrent_mA(netIndex)` (`:1184`), `pathCurrentKnown/pathCurrentSigned_mA` (`:1190/:1199`), `netScanComputeGeneration()` (`:1195`). Header notes cross-core reads are torn-tolerant: *"Safe to call from core 0 … a torn float read is cosmetic only"* (`NetVoltageScan.h:52-54`).
- **Fingerprint reset**: `connectionsFingerprint()` (`:796`) hashes numNets, path count, every `node1/node2/net`, plus the display-bus nodes (`:809-811`). On change (`:1448-1463`): `rebuildScanList(); seqPairAbandon(); memset(nodeVoltageMs, 0, sizeof(nodeVoltageMs));` plus per-path invalidation. So **every routing change instantly invalidates all samples** — no stale voltage survives a rebuild.

## A4. `buildEphemeralRoute` and the connect/disconnect API

`static bool buildEphemeralRoute(int nodeA, int nodeB, pathStruct* out)` — `src/routing/RouteSafety.cpp:786`. Also `static`; the public surface is:

- `bool planFastPath(int nodeA, int nodeB, pathStruct* out)` — `:845` (dry run; adds `wouldShort` check, no claim, no send).
- `int fastConnectPath(int nodeA, int nodeB, FastPathHandle* out, unsigned long hopTimeoutUs = 2000)` — `:864` / `RouteSafety.h:61`.
- `void fastDisconnectPath(FastPathHandle* p)` — `:919`.
- `struct FastPathHandle { pathStruct path; uint32_t generation; bool active; }` — `RouteSafety.h:28-32`.

`fastConnectPath` return codes (all from `:864-917`): `-1` not ready/null; `-2` `if (core1FramesHeld() || !core1req::allIdle())`; `-3` no route or zero hops; `-4` `if (wouldShort(hc, hx, hy, nHops, allowedNet))`; `-5` CH446Q handshake timeout (with full unwind — `for (int u = sent - 1; u >= 0; u--) sendXYrawUnchecked(hc[u], hx[u], hy[u], 0, hopTimeoutUs);` `:907-909`). On success it claims lanes as `EPHEMERAL_PATH_NET` (= `MAX_NETS - 1` = 59, `RouteSafety.h:12`) and stamps `out->generation = routingGeneration`.

**Route shapes** (`buildEphemeralRouteTiered`, `:590`; the file header at `NetVoltageScan.cpp:12-21` names them):
1. **Row on chips A-H**, direct: `(rowChip, laneToK, rowY)` + `(K, adcX, rowChip)` — 2 crosspoints, `:660-670, 734-736`.
2. Same row via **L/I/J interchip**, 4 crosspoints — `:672-699`.
3. Same row via a **neighbouring breadboard chip's y0 bounce** (`BOUNCE_NODE`), matched by `strcmp(connectionNamesX[chip][xc], connectionNamesX[n][xn])` — `:700-731`.
4. **Node already on a chip-K x pin** (rows 29/59, AREF, buffer in): bounce off a free K y — `:740-754`, 2 crosspoints.
5. **Node on I/J/L x pin** (nano header, ISENSE, UART, GPIO 1-8, rows 30/60): through a breadboard chip's y0 to K — `:756-780`, 4 crosspoints.
6. Anything else: `// Generic same-chip or refuse — non-ADC pairs not needed yet for scanner` → `return false;` (`:782-783`).

**What it refuses:**
- ADC column busy: `if (adcChip == CHIP_K && !laneOk(CHIP_K, adcX)) return false;` (`:658`) — this is the *first* gate. `laneOk` = `xColumnFreeHW(chip,x) && xStatusFree(chip,x)` (`:643-645`, file-scope twins at `:535-540`).
- No usable chip-K y row: every tier terminates through `kRowOk` (`:651-653`); if none, `if (chipKY < 0) return false;` (`:733`) or `return false` in the K-x/I-J-L tiers.
- Only nodeB (or nodeA) on **K x8-x11** is supported — `if (isXB && chipB == CHIP_K && pinB >= 8 && pinB <= 11)` (`:629-641`). **ADC4 (chip L x3) and ADC7 are not routable by this builder at all.**

**Same-net-row fallback** (the "0.0 mA starvation fix", `:575-589`): `buildEphemeralRoute` runs the tiered builder twice — `bool ok = buildEphemeralRouteTiered(nodeA, nodeB, out, false); if (!ok) ok = buildEphemeralRouteTiered(nodeA, nodeB, out, true);` (`:790-791`). Pass 2 enables `kRowSharedByNet(y)` (`:611-623`), which accepts a K row whose row claim **and every closed lane on it** already belong to nodeA's own net. Then pre-closed hops are stripped (`:801-819`) so teardown *"must NEVER open a crosspoint the user's circuit owns."*

**Floating detection / early-vs-late.** Lives entirely in `senseNodeVoltage`/`pairSenseTap`, not in the router. `NetVoltageScan.cpp:257-263`: *"A driven node reads the same early and late in the tap (measured: ±0.02V); a floating node's charge gets bumped by the charge kick of crosspoint switching and then decays through the ADC front end while tapped, so the two reads diverge (measured: 0.04-0.2V). … Oscillating signals (PWM etc.) also get rejected, which is correct - this scanner is DC-only."* Drift is stored per node in `nodeDrift[]` (`:350`) even when the tap is rejected.

**Teardown self-healing:** `fastDisconnectPath` (`:919-936`) — `if (p->generation != routingGeneration) { p->active = false; return; }` with *"Refresh already wiped hardware + xStatus; just drop the handle."* `routingGeneration++` happens in `sendPaths` (`src/CH446Q.cpp:757`). Best-effort per hop at 1000 µs, *"a stranded closed crosspoint is a short risk, so best-effort all four."*

## A5. Is there a reusable "read node N now"?

**No public single-shot on the scan core.** `senseNodeVoltage` and `pairSenseTap` are file-static (`NetVoltageScan.cpp:268`, `:369`). What exists is a **cross-core mailbox one-shot**:

```
bool requestNodeTap(int node, int preferAdc = -1);            // NetVoltageScan.h:114
int  nodeTapResult(float* volts, float* drift, int* adcUsed);  // :117
bool requestPairTap(int n1, int n2);  int pairTapResult(...);  // :118-119
void cancelOneShotTap(void);                                   // :120
```
Implementation `:500-735`. Contract limits (header `:78-100`, code `:501`): **single request in flight** (`if (oneShotReqSeq != oneShotDoneSeq) return false;`), one tap per service pass, shares `lastScanUs` with the scan (`:612`), result codes `0 pending / 1 ok / -1 floating / -2 route-or-ADC failure`. Only consumer today is GuideChecks (`src/guiding/GuideChecks.cpp:595-606` — and note it must call `cancelOneShotTap()` in teardown or *"the scan core closes a real sense route on a board nobody is measuring."*).

**What a virtual-ADC sampler would need:**

1. **Extract `senseNodeVoltage` into a header** (or add `bool nvScanTapNodeNow(int node, int adc, float* v, float* drift)`), keeping it core-affine to the scan core. It already takes `(node, adc)` — the signature is right.
2. **A drift-tolerant variant.** `:352-355` returns `false` on >50 mV early/late divergence. A user-placed ADC on a PWM'd or high-Z node would report "no reading" forever. The reading + drift must be returned and the *caller* decides.
3. **A priority slot table on the scan core**, not the mailbox. The current round-robin is one flat list (`scanNodes[kMaxScanNodes=128]`, `:88-92`) advanced one node per 5 ms pass (`:1511-1536`). Give virtual ADCs their own array serviced *before* `pairTapSlot`/`scanNodes` in the slot chain at `:1489-1537`, or give them N-of-every-M passes.
4. **Loosen the gates for them, as one-shots already are.** Today the scan pauses for 100 ms after any input (`:1443`), in any menu (`:1413`), and while `probeActive` (`:1478`). A user watching a virtual ADC while turning the clickwheel would see it freeze. The precedent for "serve anyway, keep hardware gates" is `serviceOneShotTap` (`:583-587`).
5. **Cadence math.** At `kScanIntervalUs = 5000` and one tap per pass, **N virtual ADCs round-robin at 200/N Hz**, sharing that budget with the existing node scan, source taps, and the GND auto-zero (`:1489-1494`). Raising the rate means either shrinking `kScanIntervalUs` or allowing >1 tap/pass — the latter is explicitly forbidden: *"stacking 2-3 full taps in one core2busy window could breach the 25ms waitCore2 contract on a sick PIO handshake"* (`:1254-1257`).
6. **Route-plan caching** is the obvious optimization and is safe to key on `routingGeneration` (`RouteSafety.h:17`) — but `planFastPath` (`:845`) is already a cheap dry run, so this is not required for correctness.

# B. TDM mechanism (TimeDomainMultiplexer + FakeGpio)

## B6. Timing of one `switchAndRead`, and the real cadence

`float TimeDomainMultiplexer::switchAndRead(int slot, int samples)` — `src/sensing/TimeDomainMultiplexer.cpp:269-281`:
```
if (!switchTo(slot)) return 0.0f;
delayMicroseconds(80);   // "30µs was too short and caused voltage readings from the
                         //  previous channel to leak into the new one."
return readActive(samples);
```
`switchTo` (`:195-231`): if a different channel is active, 1 SPI op to open `(K, adcX, oldY)` + `old.numHops` ops via `setChannelPath(old, 0)`; then `setChannelPath(ch, 1)` (`ch.numHops` ops) + 1 op to close `(K, adcX, ch.chipKY)`. `pollNext` then calls `disconnectActive()` (`:299`) which is another `1 + numHops` ops.

**SPI ops per poll**: because `pollNext` always disconnects at the end, `activeChannel == -1` at the next `switchTo`, so the "disconnect old" branch is skipped → **2·(1 + numHops)** ops per poll; typical `numHops == 1` (row → chip-K lane) → **4 `sendXYrawUnchecked` calls**.

`readActive` (`:261-267`) is `readAdcVoltage(adcChannel, samples)`. `readFakeGPIO` calls `pollNext(4)` (`FakeGpio.cpp:821`), so samples = 4. With the ring live that is `adcRingMeanAfter(ch, adcRingSweeps(), 4, 500us)` (`src/Peripherals.cpp:1699-1701`) ≈ **(4+1)·21 µs ≈ 105 µs of wait**. Total per poll ≈ 80 µs settle + ~105 µs read + 4 crosspoint sends ≈ **0.2–0.3 ms typical**.

**Timeout hazard.** TDM passes no `timeoutUs`, so it gets the default `100000` (`src/CH446Q.h:89-90`) — **100 ms per hop worst case, ~400 ms per poll**, versus the tap's explicit 500 µs/1000 µs budgets. `CH446Q.h:80-82` says exactly this is why taps pass short budgets: *"Background callers (NetVoltageScan sense taps) pass a short budget so a bad handshake degrades to a failed tap instead of freezing core 1 per hop."* TDM never got that treatment.

**Polling cadence — NOT 50 µs.** `readFakeGPIO()` (`FakeGpio.cpp:814-845`) self-throttles at `readFakeGPIOInterval = 50` µs (`:810`), but its only caller is `src/main.cpp:1909`, deep inside the LED-frame branch of `core2stuff()`. That branch requires *all* of:
- `micros() - schedulerTimer > schedulerUpdateTime || ledImmediate` — `schedulerUpdateTime = 8000` (`main.cpp:1517`, gate at `:1806`);
- a pending `REQ_SHOW_LEDS` (or a logo swirl) **and** `core1req::allIdle()` (`:1808-1811`);
- `rails != 2 && rails != 3 && inClickMenu == 0 && inPadMenu == 0` (`:1852-1855`);
- `core1FramesHeld()` not set (`:1877`).

So the effective rate is **one channel per LED net-frame (≈125 Hz ceiling, less when idle/menu)**, i.e. **per-channel ≈ frameRate / activeCount**, not 20 kHz. The 50 µs throttle is essentially dead code. Same core as the scan (`loop1` → `core2stuff`), and TDM **does** pause in menus, contrary to what the `readFakeGPIO` code alone suggests.

`fakeGpioRead(node)` (`FakeGpio.cpp:778-803`) is the synchronous path — `tdmInputs.switchAndRead(in.tdmSlot, 2)`. Its only caller is `src/JumperlessMicroPythonAPI.cpp:1164`, which runs on **core 0** (*"the INA219s, the MCP4728 and the OLED are all core 0"*, `JumperlessMicroPythonAPI.cpp:483`). That means raw `sendXYrawUnchecked` crosspoint traffic from core 0 with no `core_sync`, no `core1req`, no `core1busy` check — while core 1 may be inside `pollNext`. **Two cores driving the CH446Q with no arbitration.**

## B7. Known fragilities

**(a) Rebuild leaves every TDM path physically closed — the short.** `sendAllPaths` (`src/CH446Q.cpp:832-854`) sends **every** path with no `IS_FAKE_GP_IN`/`FAKE_GPIO_TDM_NET` exclusion:
```
for (int i = 0; i < numberOfPaths; i++) { int pathIdx = ...; sendPath(pathIdx, 1, 0); ... }
```
And fake-GPIO-input paths are deliberately merged into one net so they *can* share a chip-K Y row — `mergeFakeGpioInputNets()`, `src/routing/NetsToChipConnections.cpp:143-172`: *"Temporarily merge all fake GPIO input paths into FAKE_GPIO_TDM_NET for routing. This allows TDM paths to share Y positions since same-net paths don't conflict."* The only thing that undoes the resulting simultaneous closure is the disconnect dance in `fakeGpioConfigInput` (`FakeGpio.cpp:733-748`) and `finalizeFakeGpioAfterRouting` (`:1180-1190`).

But `finalizeFakeGpioAfterRouting` **skips already-registered channels**: `if (!in.active || in.tdmSlot >= 0) continue;  // Already finalized` (`FakeGpio.cpp:1165`). And it is only called from load paths — `src/main.cpp:1504`, `src/routing/States.cpp:3462` and `:3706`, `src/routing/JsonState.cpp:818`, `src/SingleCharCommands.cpp:4262`. **`refreshConnections`/`refreshLocalConnections`/`fastRefresh` (`src/Commands.cpp:201/370/582`) contain no FakeGPIO hook at all** — only two commented-out remnants (`Commands.cpp:276-277`, `:471-472`). So: an unrelated bridge add → `refreshLocalConnections` → `sendPaths` closes all TDM input paths → nothing reopens them → **all fake-GPIO input nodes are shorted together through the shared chip-K Y row until a poll happens to disconnect one of them.** `switchTo` only disconnects `activeChannel`, never the others (`TimeDomainMultiplexer.cpp:207-218`).

**(b) Reroute hook reads pre-reroute paths.** `updateFakeGpioAfterConnectionChange(node1, node2)` is called from `src/routing/States.cpp:653` (`addConnection`) and `:700` (`removeConnection`) **before** `connections.invalidateCache(config.autoRefreshOnChange)` on the next line. Inside it, `findChipKYForNode` (`FakeGpio.cpp:92`) and `extractPathHopsForNode` (`:115`) both walk `globalState.connections.paths` — i.e. the **old** routing. The refreshed hops written at `:901-907` therefore describe the fabric as it was *before* the router ran.

**(c) The `affected` predicate misses lane reassignment.** `FakeGpio.cpp:898-900`:
```
bool affected = (node1 == in.userNode || node2 == in.userNode ||
                 IS_FAKE_GP_IN(node1) || IS_FAKE_GP_IN(node2));
if (affected && in.tdmSlot >= 0) { ... }
```
A bridge that touches neither the input node nor a FAKE_GP_IN node but pushes the router onto different lanes updates nothing. Stale `hopChip/hopX/hopY` then get **driven as real crosspoints** by `setChannelPath` on the next poll (`TimeDomainMultiplexer.cpp:183-193`) — closing crosspoints that now belong to someone else's net.

**(d) TDM bypasses the router's ledger entirely.** `setChannelPath` (`:183-193`) calls `sendXYrawUnchecked` and hand-maintains `lastChipXY` — it **never touches `chipStates[].xStatus/yStatus`**, never calls `wouldShort`, and holds no generation stamp. Contrast `fastConnectPath`, which claims lanes (`claimPathLanes`, `RouteSafety.cpp:823`), runs `wouldShort` (`:890`), and self-voids on `routingGeneration` change (`:921`). Consequences: a stale TDM hop is invisible to `laneUsable`/`yUsable` (`RouteSafety.cpp:535-540`) except through the `lastChipXY` bits it sets, and a `sendPaths` clean refresh (`memset(lastChipXY, 0, ...)`, `CH446Q.cpp:~825`) silently desynchronizes TDM's model of the world with no generation check to catch it.

**(e) `updateChannelChipKY` reconnect asymmetry.** `TimeDomainMultiplexer.cpp:152-168` reconnects only `if (activeChannel == slot && oldY != newChipKY)`. Since `pollNext` leaves `activeChannel == -1`, the reconnect never fires in the background path — fine, but it means the "fix up the live connection" branch is effectively dead code.

**(f) Index bug, incidental.** `fakeGpioRemoveInput` does `gpioReadingColors[slot] = 0;` (`FakeGpio.cpp:644`) using the raw slot instead of `GPIO_INDEX_FAKE_IN(slot)` (= `18 + slot`, `src/Peripherals.h:87`). For slot 0-9 that clobbers a **real GPIO's** color; 10-17 clobbers a fake *output's*. `clearInputDisplay(slot)` on the next line does it correctly.

## B8. AdcRing interaction and effective per-channel rate

`readActive` → `readAdcVoltage` → `readAdc` → with the ring live, `adcRingMeanAfter(channel, adcRingSweeps(), samples, samples*25+400)` (`src/Peripherals.cpp:1697-1702`). Semantics from `src/hardwarestuff/AdcRing.h:65-68`: *"Wait until n sweeps NEWER than sweep index `after` exist (bounded by timeoutUs; ~ (n+1) x 21 us normally) … On timeout returns the newest n (and counts a stall)."*

So TDM's read is a **non-strict** wait — on timeout it silently returns history, i.e. samples from whatever was on that channel *before* the switch. NetVoltageScan deliberately uses the strict form and fails instead (`NetVoltageScan.cpp:213-222`). **TDM has no equivalent guard**, and `switchAndRead`'s 80 µs `delayMicroseconds` is precisely the fix for the same class of problem: *"under the ADC ring, readActive()'s window starts at the call, so without this delay the first sweeps of every read landed pre-settle"* (`TimeDomainMultiplexer.cpp:272-277`).

Ring cadence: 48 kHz/channel, 20.83 µs/sweep, 512-sweep depth ≈ 10.7 ms (`AdcRing.h:34-38`). 4 sweeps ≈ 105 µs of wait per poll.

**Effective per-channel TDM rate** = LED-net-frame rate ÷ `activeCount`. At the 8 ms scheduler tick that is ≈125 Hz ÷ N; with 8 fake inputs ≈ **15 Hz per channel**, and it stops entirely in menus, during `core1FramesHeld`, and whenever no LED show is requested.

# C. Display consumers of ADC voltage

**`adcReadings[8]`** — `src/Peripherals.cpp:328`, declared `src/Peripherals.h:76`. Refreshed by `updateLazyAdcReadings()` (`Peripherals.cpp:1299`), called from `core2stuff()` at `src/main.cpp:2103`, **after** `core_sync_release()`. With the ring live (`:1327-1341`) it refreshes **all 8 channels every pass, throttled to 1 kHz**: `if ((nowUs - lastRingUs) < 1000u) return;` then `for (ch 0..7) adcRingMeanNewest(ch, 16)`. Without the ring it rotates one channel per tick — `LAZY_ADC_FAST_INTERVAL_US 30000` for ch 0-4, `LAZY_ADC_SLOW_INTERVAL_US 200000` for 5-7 (`Peripherals.h:216-224`), so ≈ each of ch0-4 every 150 ms.

**`showLEDmeasurements()`** — `Peripherals.cpp:1215`, self-throttled at `showLEDmeasurementsInterval = 5000` µs (`:1213`), called from `core2stuff()` at `src/main.cpp:1915` (inside the LED frame block, so really once per net frame). For each `i` in 0..7 where `showADCreadings[i] > 0 && <= numberOfNets` (`:1243`):
```
if ( !readingADC ) { adcReading = readAdcVoltage( i, samples );  adcReadings[i] = adcReading; }
else               { adcReading = adcReadings[ i ]; }                      // :1245-1249
color = measurementToColor( adcReading, adcRange[i][0], adcRange[i][1] );  // :1251
... lightUpNet( showADCreadings[i], -1, 1, brightness, 0, 0, color );      // :1265
adcReadingColors[ i ] = color;                                            // :1281
```
`adcRange[8][2] = {{-8,8},{-8,8},{-8,8},{-8,8},{0,5}}` (`Peripherals.cpp:316`) — ADC0-3 are ±8 V, ADC4 is 0-5 V.

**`showADCreadings[8]`** (`Peripherals.cpp:331`) is *net indices, not voltages*: built by `rebuildShownReadings()` (`:1097`) which scans `globalState.connections.paths` and sets `showADCreadings[n] = pathNet` when a path names `ADCn` (`:1146-1160`). It is rebuilt **only on routing changes** — `chooseShownReadings()` (`:1192`) is called from `refreshConnections` (`src/Commands.cpp:272`), `refreshLocalConnections` (`:459`), `fastRefresh` (`:572`), all on core 0. The per-frame call at `src/main.cpp:1915` is commented out.

Its fake-GPIO exclusion (`Peripherals.cpp:1127`, `:1134-1144`):
```
int tdmAdcNode = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : -1;
...
if ( tdmAdcNode >= 0 && ( n1 == tdmAdcNode || n2 == tdmAdcNode ) ) {
    for (s...) if (fakeGpioInputs[s].active && fakeGpioInputs[s].netIndex == pathNet) isFakeGpioNet = true;
    if ( isFakeGpioNet ) continue;  // Skip -- TDM handles this, not regular ADC display
}
```
Note the single-slot semantics: `showADCreadings[]` has one net per channel, so if two nets shared one ADC only the **last** would win.

**Graphics.cpp coloring** (`src/Graphics.cpp:2976-2991`, `:3136-3164`):
```
int adcChannel = anyAdcConnected(actualNet);
bool isAdcNet = (adcChannel >= 0 && adcChannel < 8);
if (isAdcNet) { for (fakeIdx = 18; fakeIdx < 50; fakeIdx++)     // 18-49 = fake GPIO inputs
    if (gpioNet[fakeIdx] == actualNet && gpioNet[fakeIdx] > 0) { isAdcNet = false; break; } }
...
if (isAdcNet) { __dmb(); uint32_t adcColor = adcReadingColors[adcChannel];
                if (adcColor != 0) for (i<5) frameColors[i] = brightenedNodeColors[i] = adcColor; }
if (!isAdcNet) { for (fakeIdx = 18..49) if (gpioNet[fakeIdx] == actualNet && > 0) {
                    uint32_t fakeColor = gpioReadingColors[fakeIdx]; ... } }
```
`anyAdcConnected(net)` (`src/hardwarestuff/RoutableGpio.cpp:1137-1155`) is a pure `showADCreadings[i] == net` reverse lookup. Second consumer: `src/eyecandy/Highlighting.cpp:1644` and `:2083` (the OLED/highlight reading line).

**FakeGpio's own coloring** — `updateFakeGpioInputDisplay(slot)`, `src/sensing/FakeGpio.cpp:248-285`. It writes `gpioState[idx] = 0xFF`, `gpioNet[idx] = in.netIndex` and then mirrors `showLEDmeasurements` exactly (`:262-281`):
```
float voltage = tdmInputs.channels[in.tdmSlot].lastVoltage;
uint32_t color = measurementToColor(voltage, -7.0f, 7.0f);
int brightness = LEDbrightnessSpecial + (int)fabsf(voltage * 2.0f);  // clamp 4..100
if (lines_wires == 0 || numberOfShownNets > MAX_NETS_FOR_WIRES) lightUpNet(in.netIndex, -1, 1, brightness, 0, 0, color);
int scaleVoltage = map((int)fabsf(voltage), 0, 8, -30, 70);
gpioReadingColors[idx] = scaleBrightness(color, scaleVoltage);
```
(Range is `-7..7` here vs. `adcRange` `-8..8` in `showLEDmeasurements` — a small, pre-existing inconsistency.) It is called from `readFakeGPIO` after each poll (`:837`).

**What a virtual-ADC net needs to be colored the same way.** Two clean options, both already precedented:

- *ADC-shaped:* have `anyAdcConnected(net)` return a channel index and have `adcReadingColors[channel]` non-zero and fresh. Since a virtual ADC has no permanent channel, this needs either an index space beyond 0-7 (both `showADCreadings[8]` and `adcReadingColors[8]` are hard-sized, `Peripherals.h:77-78`, and `Graphics.cpp:2979` hard-checks `< 8`) or a parallel table.
- *FakeGPIO-shaped (less work):* write `gpioNet[idx] = netIndex` and `gpioReadingColors[idx] = scaleBrightness(measurementToColor(v, -8, 8), …)` for an index in a range Graphics scans. Today Graphics only scans `18..50` and that range is `GPIO_INDEX_FAKE_IN(0..31)` (`Peripherals.h:87`) — already fully claimed by fake GPIO inputs.

Concretely, a virtual ADC net needs: **a stable net index, a voltage refreshed at ≥ a few Hz, and a `uint32_t` color slot that Graphics' per-net override loop looks at.** `nodeVoltage[node]` + `nodeVoltageMs[node]` (already produced by the scan) supplies the first two; only the color-slot plumbing is genuinely missing.

# Recommendation

**Persistent registration + a fresh ephemeral tap per poll (option 2 with a registry), not persistent TDM channels with stored hops.** The evidence:

1. **Stored hops are the bug class; fresh routes are structurally immune.** `NetVoltageScan.cpp:7-10` states the design property directly: *"Sense routes are computed FRESH on every poll from the live crosspoint occupancy (lastChipXY) and the fabric map (chipStates[].xMap/yMap) … No persistent channel state exists, so rerouting can never leave a stale tap behind."* Every TDM fragility in B7 — the pre-reroute hook read (States.cpp:653/700), the narrow `affected` predicate (FakeGpio.cpp:898), the `tdmSlot >= 0` skip that leaves paths closed after a rebuild (FakeGpio.cpp:1165 + CH446Q.cpp:832), the missing `wouldShort`/`xStatus` participation (TimeDomainMultiplexer.cpp:183-193) — is a direct consequence of caching hops across a routing epoch. User-placed ADCs are exactly the feature most likely to sit on a board being actively rewired, which is the worst case for cached hops.

2. **The tap has real safety machinery TDM lacks.** Per tap: `wouldShort(hc, hx, hy, nHops, allowedNet)` (`RouteSafety.cpp:890`), lane claims under `EPHEMERAL_PATH_NET` so the router cannot double-book (`:896`, `:823-843`), full unwind on handshake timeout (`:907-909`), short per-hop timeouts (500 µs connect / 1000 µs disconnect), and generation-stamped teardown that no-ops after a refresh (`:921-924`). TDM has none of these and drives crosspoints with a 100 ms default per-hop timeout.

3. **The route is cheap enough.** ~0.7–1.2 ms typical per tap (80 µs settle + 500 µs dwell + 4 crosspoint sends) versus TDM's ~0.2–0.3 ms. That is a ~4× cost, but TDM's *measured* cadence is already only one channel per LED frame (B6), so the tap is not the bottleneck. At `kScanIntervalUs = 5000` a dedicated virtual-ADC slot gives 200 Hz shared among N channels — comfortably better than what TDM delivers today.

4. **The strict-freshness read is the correct default for a displayed voltage.** `adcRingMeanAfterStrict` + `if (!fresh) { tapFailRingStale++; return false; }` (`NetVoltageScan.cpp:217-222`) is why the scan can't paint pre-tap history as a live reading. TDM's `adcRingMeanAfter` returns history on timeout with no signal to the caller.

**What must change (none of it is speculative — each is a named line):**

- **Extract the primitive.** Publish a `senseNodeVoltage`-shaped entry point (`NetVoltageScan.cpp:268`) that returns `(volts, drift, ok)` rather than swallowing the >50 mV drift case at `:352-355`. A user-placed ADC on a PWM node must show a number, not nothing.
- **Add a priority registry on the scan core** — a small `virtualAdcNodes[]` served ahead of `pairTapSlot`/`scanNodes` in the slot chain at `:1489-1537`, one tap per pass (do **not** stack taps; `:1254-1257` and the 25 ms `waitCore2` contract).
- **Move it above the politeness gates**, like `serviceOneShotTap` (`:1389`, rationale `:583-587`): keep `core1FramesHeld`/`core1req::allIdle`/`core1busy`/`refreshInProgress`/`usbAudioOwnsAdc`/`probeActive`/`wavegen`, drop the `lastUserInputMs < 100` (`:1443`) and menu (`:1413`) pauses for these slots only.
- **Accept the range limit or extend the router.** `buildEphemeralRouteTiered` only targets chip K x8-x11 (`RouteSafety.cpp:629-641`, refusal at `:783`) and the pool mask is `0x0F` (`:1209`). **Virtual ADCs via taps are ±8 V ADC0-3 only** — ADC4 (0-5 V, chip L x3) and ADC7 are unreachable without new tiers. If ADC4 must be offered, that is a router change, not a sampler change.
- **Add the color slot** (see C). The `showADCreadings[8]`/`adcReadingColors[8]`/`< 8` triple (`Peripherals.h:77-78`, `Graphics.cpp:2979`) is what actually blocks "any number of ADCs" today; the sampling side does not care how many nodes are in the list.

**Where a hybrid is justified — precisely one place:** cache the *route plan* (`planFastPath`, `RouteSafety.cpp:845`) per virtual channel keyed on `routingGeneration` (`RouteSafety.h:17`), and discard it the moment the generation moves. That is pure latency optimization with an automatic invalidation edge already maintained by `sendPaths` (`CH446Q.cpp:757`). **Nothing about the physical crosspoints should be persistent** — no `activeChannel`, no hops held across polls, no closed crosspoint outliving the read that needed it. Stated as a rule: *persistent = which nodes, their priority, their color slot, and (optionally) a generation-stamped route plan; ephemeral = every crosspoint, every lane claim, every ADC-pool acquisition.*

**Not read:** the CH446Q PIO strobe timing (so per-crosspoint microsecond costs above are inferred from the code's own timeout budgets and comments, not measured); `MeasureMode.cpp` beyond its ADC-acquisition and `readAdcVoltage(adcChannel, 16)` call sites (`src/sensing/MeasureMode.cpp:340`, `:421`); `NetsToChipConnections.cpp` outside the FAKE_GPIO_TDM_NET merge/restore block (`:130-280`); `Highlighting.cpp` bodies around its two `anyAdcConnected` call sites.