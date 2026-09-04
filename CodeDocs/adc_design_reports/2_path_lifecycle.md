<!-- Design-agent report from the 2026-09-02 ADC redesign session (chat b816f338). Line numbers cite the JumperlOS-main worktree at 5.7.10.0 and drift a few lines on dev; symbols are authoritative. -->

Scope note: all `NetsToChipConnections.cpp` (NTCC) citations are the V5 file. `src/routing/NetsToChipConnections_OG.cpp` is excluded from V5 builds — `platformio.ini:253-258` "The OG routing engine lives in NetsToChipConnections_OG.cpp (guarded by ... don't define the same symbols twice. V5 builds never compile the _OG file". I read no _OG code.

---

## 1. Path lifecycle for a bridge like [row 3, ADC1]

### 1a. Call order inside `bridgesToPaths()` (NTCC:1595)

Exact sequence (all in `bridgesToPaths`, `fillUnused`/`allowStacking`/`startIndex` args):

| # | Call | Line |
|---|---|---|
| 1 | `sortPathsByNet()` (only if `startIndex == 0`) | NTCC:1621 |
| 2 | `mergeFakeGpioInputNets()` | NTCC:1630 |
| 3 | loop `i = startIndex..numberOfPaths`, skipping `duplicate == 1` (NTCC:1659): `findStartAndEndChips(node1, node2, i)` → `mergeOverlappingCandidates(i)` → `assignPathType(i)` | NTCC:1677 / 1688 / 1694 |
| 4 | `sortAllChipsLeastToMostCrowded()` (only `startIndex == 0`) | NTCC:1719 |
| 5 | `resolveChipCandidates(startIndex)` | NTCC:1725 |
| 6 | `commitPaths(2, -1, 0, startIndex)` | NTCC:1731 |
| 7 | `resolveAltPaths(2, -1, 0, startIndex)` | NTCC:1737 |
| 8 | `resolveUncommittedHops(2, -1, 0, startIndex)` | NTCC:1743 |
| 9 | if `fillUnused == 1`: `fillUnusedPaths(stack_paths, stack_rails, stack_dacs)` | NTCC:1758-1760 |
| 10 | then re-run 3 over the dup range `duplicateStartIndex..numberOfPaths` (skipping `duplicate == 0` and `pathType == VIRTUAL`) | NTCC:1765-1786 |
| 11 | `resolveChipCandidates(duplicateStartIndex)`; `commitPaths(0,-1,1)`; `resolveAltPaths(0,-1,1)`; `resolveUncommittedHops(0,-1,1)` | NTCC:1790 / 1795 / 1796 / 1798 |
| 12 | `restoreFakeGpioInputNets()` | NTCC:1803 |
| 13 | `couldntFindPath(1)` | NTCC:1805 |
| 14 | `checkForOverlappingPaths()` | NTCC:1807 |
| 15 | `validateAllPaths()` (RouteSafety.cpp:392) | NTCC:1808 |
| 16 | `updateLiveCrossbarDisplay()` | NTCC:1849 |

`sortPathsByNet()` (NTCC:1416) is what turns bridges into paths — it walks `nets[j].bridges[k]` and writes NTCC:1478-1481:
```
globalState.connections.paths[pathIndex].net = globalState.connections.nets[j].number;
globalState.connections.paths[pathIndex].node1 = node1;
globalState.connections.paths[pathIndex].node2 = node2;
globalState.connections.paths[pathIndex].duplicate = 0;
```
and ends with NTCC:1560-1561 `numberOfPaths = pathIndex;` / `globalState.connections.numPaths = numberOfPaths;` (note: this is set **before** duplicates are appended — see §6).

### 1b. Where the chip-K Y lane is chosen for a "BB to SF" path

**It is a lookup, not a search.** `commitPaths` case `BBtoSF` (NTCC:2600) computes:
```
NTCC:2606  int xMapSFc1 = xMapForNode(paths[i].node2, paths[i].chip[1]);
NTCC:2607  int yMapSFc1 = globalState.connections.paths[i].chip[0];
```
and commits it as:
```
NTCC:2630-2634  bool pathY1Success = setPathY(i, 1, globalState.connections.paths[i].chip[0]);
                // bb to sf connections are always in chip order,
                // so chip A is always connected to sf y 0
NTCC:2654       chipY1Success = setChipYStatusSafe(paths[i].chip[1], paths[i].chip[0], paths[i].net, "BBtoSF Y1");
```
So the K y-row **is** the breadboard chip index: chip A → K y0, B → y1 … H → y7. This matches the hardware map `chipStates[10].yMap = {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}` (MatrixState.cpp:148).

Answering the hypotheses explicitly:
- **`chipStates[].yStatus`? YES.** That is the occupancy structure. The gate is `freeOrSameNetY(paths[i].chip[1], yMapSFc1, paths[i].net, currentAllowStacking)` (NTCC:2618-2619), which reads `globalState.connections.chipStates[chip].yStatus[y]` (NTCC:4517-4518). Writes go through `setChipYStatusSafe` (NTCC:757) / `setChipYStatus` (NTCC:584 `globalState.connections.chipStates[chip].yStatus[y] = net;`).
- **`lastChipXY`? NO.** `lastChipXY[12]` is the *hardware shadow* defined at `CH446Q.cpp:64` and only written at send time (`CH446Q.cpp:849`, `1614-1617`). The router never reads it.
- Nothing else. There is no per-lane allocator.

`resolveAltPaths` case `BBtoSF` (NTCC:4169) does the same thing through a hop chip: `int yMapSF = bb; // always` (NTCC:4223), gated by `freeOrSameNetX(bb, xMapBB, ...) && freeOrSameNetY(bb, 0, ...)` (NTCC:4243-4246) and `freeOrSameNetY(sfChip, yMapSF, ...)` (NTCC:4319-4322), then records `paths[i].chip[2] = bb; paths[i].chip[3] = bb;` (NTCC:4334-4335).

`resolveUncommittedHops` (NTCC:4661) **cannot rescue a BBtoSF failure**: it only acts on half-committed markers —
```
NTCC:4692  if (globalState.connections.paths[i].y[checkY] == -2) { hasUnresolvedY = true; ... }
NTCC:4699  if (globalState.connections.paths[i].x[checkX] == -2) { hasUnresolvedX = true; ... }
NTCC:4706  if (!hasUnresolvedY && !hasUnresolvedX) { continue; }
```
A failed BBtoSF leaves `-1`, not `-2`, so it is skipped.

### 1c. Concrete trace for `[3, ADC1]`

1. `findStartAndEndChips` (NTCC:5806): node `3` hits `case 1 ... 60` → `paths[i].chip[0] = board::currentBoard().bbNodesToChip[3]` (NTCC:5837). `ADC1 == 111` hits `case GND ... 141` (NTCC:5926) → scans chips 8-11 `xMap[]` for 111 (NTCC:5930-5933). Only chip K carries it: `chipStates[10].xMap[9] == ADC1` (MatrixState.cpp:147, duplicated at :170). `candidatesFound == 1` → `paths[i].chip[1] = CHIP_K` and candidates cleared (NTCC:5944-5947).
2. `mergeOverlappingCandidates` (NTCC:5963): both `candidates[*][0] == -1`, so it is a no-op.
3. `assignPathType` (NTCC:6057): node1 in 1-28 → `nodeType[0] = BB` (NTCC:6130-6132). node2 branch fires on `... || globalState.connections.paths[pathIndex].chip[1] == CHIP_K` (NTCC:6142) → `nodeType[1] = SF`. Result `pathType = BBtoSF` (NTCC:6184); no `swapNodes`.
4. `commitPaths` case BBtoSF as in §1b.

### 1d. What produces `x=-1, y=-1` and the "Couldn't find a path" message

`clearAllNTCC` initialises everything to -1 by memset:
```
NTCC:1317  memset(globalState.connections.paths, -1, pathsToClear * sizeof(pathStruct));
```
(`x[6]`, `y[6]`, `chip[4]` from `pathStruct`, MatrixState.h:55-82). If `commitPaths` takes the `else` at NTCC:2713-2721 (`altPathNeeded = true; "no direct path, setting altPathNeeded flag (BBtoSF)"`) and `resolveAltPaths` finds no hop chip (the `for bb` loop at NTCC:4194 exits with `foundPath == 0`), nothing ever writes `x[]`/`y[]` — they stay -1.

`couldntFindPath` (NTCC:4554) then:
```
NTCC:4570  for (int j = 0; j < 3; j++) {
NTCC:4571    if (globalState.connections.paths[i].chip[j] == -1 && j >= 2) { continue; }
NTCC:4575    if (globalState.connections.paths[i].x[j] < 0 || globalState.connections.paths[i].y[j] < 0) { foundNegative = 1; }
...
NTCC:4607        Serial.print("\n\rCouldn't find a path for ");
NTCC:4616      if (numberOfUnconnectablePaths < 10) { unconnectablePaths[n][0] = ...node1; [1] = ...node2; }
NTCC:4620      numberOfUnconnectablePaths++;
NTCC:4621      globalState.connections.paths[i].skip = true;
```
Suppressions before the print: `pathType == VIRTUAL` (NTCC:4561 and again 4594), `node1 == -1 || node2 == -1` (NTCC:4591), and the current-sense virtual wire pair (NTCC:4598-4605).

### 1e. State left behind, and does the bridge survive?

For the failed **non-duplicate** path: `node1`/`node2` intact (and already virtual-expanded if applicable — §2), `net` intact, `chip[0]` = BB chip, `chip[1]` = `CHIP_K`, `chip[2..3] = -1`, all `x[]`/`y[] = -1`, `altPathNeeded = 1` (set NTCC:2714, never cleared), `pathType = BBtoSF`, `skip = true` (NTCC:4621).

For a failed **duplicate** the coordinates are actively wiped (NTCC:4622-4634):
```
// A duplicate that couldn't finish routing ... is silently dropped. Wipe its
// coordinates: a half-committed hop keeps x set with y == -2, and sendPath()
// only filters -1 ...
for (int j = 0; j < 4; j++) { paths[i].x[j] = -1; paths[i].y[j] = -1; }
paths[i].altPathNeeded = false;
```

**The bridge stays in `bridges[]`.** Nothing in NTCC writes `globalState.connections.bridges[]` — it only reads it (e.g. NTCC:1928-1930, 5283-5291). The failure is re-attempted on every subsequent rebuild. Consumers of the failure signal:
- `paths[].skip`: RouteSafety.cpp:401, NetVoltageScan.cpp:971/989/1153/1337, Graphics.cpp:2169/2386/3292.
- `unconnectablePaths[]`/`numberOfUnconnectablePaths`: declared `NetsToChipConnections.h:24-25`; read by `guiding/GuideChecks.cpp:210-211, 476-479` and `sensing/PartMeasure.cpp:110-115`.
- **`sendPaths()` does NOT consult `skip`** — `CH446Q.cpp:834-836` sends every path index, and `sendPath` filters per hop: `if (paths[i].chip[chip] != -1)` (CH446Q.cpp:1583) and `if (paths[i].y[chip] == -1 || paths[i].x[chip] == -1) continue;` (CH446Q.cpp:1589-1593). Only `-1` is filtered; `-2` would be encoded (that is exactly the hazard the NTCC:4623-4629 comment describes).

---

## 2. Virtual node expansion — every site

**Router / routing-state sites**

| Site | What it does | Mutates persistent state? |
|---|---|---|
| NTCC:5902-5925 `findStartAndEndChips`, `case FAKE_GP_IN_0 ... FAKE_GP_IN_31` | `int expandedNode = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : ADC0;` (5908) then `bothNodes[twice] = expandedNode;` (5915) and **writes it back**: `paths[pathIdx].node1/node2 = expandedNode` (5918/5920). Falls through into `case GND ... 141` (5926) so the ADC gets K as its chip. | **YES** — this is the one permanent mutation. From here on `paths[]` holds `ADCn`, not `FAKE_GP_IN_x`. |
| NTCC:5871-5899 same function, `case FAKE_GP_OUT_0 ... FAKE_GP_OUT_7` | expands to `highVoltageNode`/`lowVoltageNode` by `currentState`, same write-back. | YES |
| NTCC:6058-6091 `assignPathType` | Recomputes expanded `node1`/`node2` into **locals**. I scanned NTCC:6092-6215 for bare `node1`/`node2` — the only hits are inside comments. **These locals are dead code.** Harmless only because `findStartAndEndChips` (step 3a) already mutated the struct before `assignPathType` (step 3c) runs. | No |
| NTCC:5280-5299 `checkForOverlappingPaths`, `isFakeGpioInputPath` lambda | Re-derives "is this an FGPI path" by scanning `bridges[]` for `IS_FAKE_GP_IN` (5286/5289) because the path node was already overwritten to `ADCn`. `if (isFakeGpioInputPath(i) && isFakeGpioInputPath(j)) continue;` (5296-5299). **This is validator-only** — it suppresses the overlap *report*, nothing else. | No |
| NTCC:5322-5344 `expandNode` lambda inside the same loop | Expands for the *power-rail exemption* check only (5346-5349). | No |
| NTCC:135-139 `pathIsFakeGpioInput` + 144-173 `mergeFakeGpioInputNets` + 177-272 `restoreFakeGpioInputNets` | See below. | net field + chip status |
| NTCC:1509-1521 `sortPathsByNet` | visibility + `nets[j].virtual_net = true` using range `FAKE_GPIO_1 .. FAKE_GPIO_32`. | nets[] |
| NTCC:1983-1987 `fillUnusedPaths` | duplicate-skip using the same `FAKE_GPIO_1 .. FAKE_GPIO_32` range. | — |
| RouteSafety.cpp:380-390 + 404 | `validateAllPaths` skips FGPI paths entirely: `// TDM manages these; simultaneous presence in paths[] is intentional.` (403-404). | No |
| RouteSafety.cpp:62 | `(node >= ADC0 && node <= ADC4) || ...` ADC predicate. | No |
| InfraPaths.cpp:822 | `if (IS_FAKE_GP_IN(n1) || IS_FAKE_GP_IN(n2)) continue;` inside `infraAdcUserClaimed` — FGPI bridges are not a user claim. | No |

**Display / reporting / state-serialization sites**
- `NetManager.cpp:152` name table `{"FGPI0", "FAKE_GP_IN_0", FAKE_GPIO_9}`.
- `NetManager.cpp:1246-1252` — `chooseShownReadings`: `netsShowingSpecial[i] = 6; netsFakeGpioSlot[i] = node - FAKE_GP_IN_0;`.
- `NetManager.cpp:1998-2009` — bridge↔path matching that tolerates the expansion.
- `NetManager.cpp:2075-2117` — `printNodeOrName`: raw `FAKE_GP_IN_x` → `FGPIn` (2075-2085); and `ADCn == ADC0+fakeGpioInputAdcChannel` → reverse-mapped to `FGPIn` via `netIndex` (2087-2110).
- `JsonState.cpp:234-245` — JSON `FAKE_GPIO_IN` output, reads `tdmInputs.channels[tdmSlot].lastVoltage`.
- `Peripherals.cpp:1127-1143` — ISENSE/ADC net detection skips paths whose net matches an active `fakeGpioInputs[s].netIndex`.
- `hardwarestuff/RoutableGpio.cpp:64-93` — `gpioState[50]`, `gpioReading[50]`, `gpioNet[50]` reserve indices 18-49 for `FAKE_GP_IN_0..31`.
- `FakeGpio.cpp:634, 684, 874, 1077-1080, 1256-1259` — slot ↔ virtual node arithmetic.
- `MatrixState.cpp:459-461` — `isConnectable()` accepts `FAKE_GPIO_1 .. FAKE_GPIO_32` (see risks).

### Do two FAKE_GP_IN paths share one chip-K Y lane?

**They share a lane iff their breadboard rows sit on the same A-H chip**, because the lane is `paths[i].chip[0]` (NTCC:2607). Two FGPI nodes on rows served by chip A both want K y0; rows on chip A and chip C want y0 and y2.

What makes the same-lane case *legal* is the net merge, not the lane chooser:
```
NTCC:170  globalState.connections.paths[pathIdx].net = FAKE_GPIO_TDM_NET;
```
(`FAKE_GPIO_TDM_NET = MAX_NETS - 2 = 58`, JumperlessDefines.h:564). Then `freeOrSameNetY` passes on its second arm:
```
NTCC:4517-4518  if (chipStates[chip].yStatus[y] == -1 ||
                    (chipStates[chip].yStatus[y] == net && allowStacking == 1))
```
Three caveats, all load-bearing:
1. The merge only runs when there is more than one: `if (numFakeGpioInputPaths > 1)` (NTCC:160), and restore early-returns `if (numFakeGpioInputPaths <= 1)` (NTCC:178).
2. Same-net sharing requires `allowStacking == 1`, i.e. the **second** iteration of `commitPaths`' retry loop (`maxStackingAttempts = (allowStacking == 2) ? 1 : 0`, NTCC:2345-2346). `refreshConnections`/`refreshLocalConnections` reach it (`bridgesToPaths()` defaults → `commitPaths(2,...)`, NTCC:1731). **`fastRefresh` calls `bridgesToPaths(0, 0, 0)`** (Commands.cpp:674) — `allowStacking` there is the *fillUnused* arg, and NTCC:1731 still passes literal `2`, so the retry does exist even in fastRefresh. (The `allowStacking` parameter of `bridgesToPaths` is in fact unused in the body — no reference to it after the signature at NTCC:1596.)
3. The lane-choosing code does **not** know about fake GPIO. It only sees `yStatus[y]` and the (merged) net number. So: another fake-GPIO path's lane looks *occupied by net 58*, which is treated as "free" only via the same-net-stacking arm. A non-FGPI path of a different net on that same K row blocks it outright.

`restoreFakeGpioInputNets` is three phases: Phase 1 restores `paths[].net` (NTCC:189-199); Phase 2 fixes duplicates by node matching (NTCC:202-224); **Phase 3 brute-force rewrites `chipStates[chip].xStatus[x]` and `yStatus[y]` that still hold 58** (NTCC:226-267), attributing each to the first path whose hop lands on that chip+x (or chip+y) — and writing `realNet = -1` (i.e. freeing the row) if no path matches (NTCC:246, 264). So the phantom-net-58 risk is handled, but the attribution is a first-match heuristic.

### What happens when chip K is full and a FAKE_GP_IN path is added?

1. `commitPaths` BBtoSF: `freeOrSameNetY(CHIP_K, chip[0], 58, allowStacking)` fails → `altPathNeeded = true` (NTCC:2714).
2. `resolveAltPaths` walks `bb = 0 .. (8 - uncommittedHops1)` (NTCC:4194); each candidate needs `freeOrSameNetY(sfChip, bb, net, ...)` (NTCC:4319). If every K row is held by a different net, all 8 fail.
3. `resolveUncommittedHops` skips it (only `-2`, NTCC:4692-4708).
4. `couldntFindPath` prints "Couldn't find a path for <row> to ADCn" (NTCC:4607) and sets `skip = true` (NTCC:4621). `x/y` stay -1, so `sendPath` emits nothing (CH446Q.cpp:1589).
5. `finalizeFakeGpioAfterRouting`: `findChipKYForNode` returns -1, `addChannel` is still called with `chipKY = -1` (FakeGpio.cpp:1167-1177), and the `if (chipKY >= 0)` disconnect is skipped (FakeGpio.cpp:1180). At poll time `switchTo` bails (`if (channels[slot].chipKY < 0 || >= 8) return false;` TimeDomainMultiplexer.cpp:198) and `pollNext` skips it (`channels[slot].chipKY >= 0`, TimeDomainMultiplexer.cpp:290). Silent dead channel.

Note also `reservedKRowForSenseTaps` (NTCC:577-581) only fires when `routingDuplicatePathNow` is true, so a **primary** FGPI path is free to consume the last virgin K rows that ephemeral sense taps depend on.

---

## 3. `pathType VIRTUAL`

- Enum: `MatrixState.h:51` — `enum pathType {BBtoBB, BBtoNANO, NANOtoNANO, BBtoSF, NANOtoSF, BBtoBBL, NANOtoBBL, SFtoSF, SFtoBBL, BBLtoBBL, VIRTUAL};` → value **10**.
- **Sole setter:** `Graphics.cpp:717`, inside the current-sense overlay helper:
```
Graphics.cpp:712-724
  paths[pathIndex].node1 = plusNode;  paths[pathIndex].node2 = minusNode;
  paths[pathIndex].net = plusNet;  paths[pathIndex].duplicate = 0;  paths[pathIndex].skip = false;
  paths[pathIndex].pathType = VIRTUAL;
  virtualCurrentSensePathIndex = pathIndex;
  numberOfPaths = pathIndex + 1; // Temporarily increment
  currentSenseOverlayState.virtualWireNode1 = plusNode;
  currentSenseOverlayState.virtualWireNode2 = minusNode;
```
Torn down by `removeVirtualCurrentSensePath()` (Graphics.cpp:727-743), which clears node1/node2, `net = 0`, decrements `numberOfPaths` if it was last, and resets `virtualWireNode1/2 = -1`.
- **Consumes no fabric.** It is skipped by every routing stage: `fillUnusedPaths` (NTCC:1780, 1990), `commitPaths` (NTCC:2296), `resolveAltPaths` (NTCC:3082), `couldntFindPath` (NTCC:4561, 4594), `resolveUncommittedHops` (NTCC:4669), `resolveChipCandidates` (NTCC:6335), `validateAllPaths` (RouteSafety.cpp:402), `NetVoltageScan.cpp:972` and `:1338`, `Graphics.cpp:2172` and `:2389`. Its `chip[]` is never assigned, so `sendPath`'s `chip[chip] != -1` guard (CH446Q.cpp:1583) emits nothing — but note it *is* inside `numberOfPaths` and therefore inside `sendPaths`' loop (CH446Q.cpp:834).
- **Display:** `printPathType` (NTCC:6659) `case 10: return target->print("VIRTUAL");` (NTCC:6676-6677). Graphics renders it as the marching-ants current-sense wire (Graphics.cpp:1913 "Render VIRTUAL WIRE pixels").
- **`currentSenseOverlayState.virtualWireNode1/2`**: declared `Graphics.h:148-149` (`int virtualWireNode1 = -1;`). Set/cleared at Graphics.cpp:723-724 / 741-742; used for LED segment classification (Graphics.cpp:1229-1232, 1747) and — importantly for the router — to **suppress the "Couldn't find a path" message** for that node pair in either order (NTCC:4598-4605).

---

## 4. Duplicate paths

- Generated by `fillUnusedPaths(int duplicatePathsOverride, int duplicatePathsPower, int duplicatePathsDac)` (NTCC:1853), called once from `bridgesToPaths` with `jumperlessConfig.routing.stack_paths / stack_rails / stack_dacs` (NTCC:1758-1760). `duplicateStartIndex = numberOfPaths` is captured before the primary pass (NTCC:1657).
- Per-bridge budget, not per-net: `int bridgeDuplicates = globalState.connections.bridges[bridgeIdx][2];` (NTCC:1930). Infra bridges bail first: `if (infraIsBridge(node1, node2)) continue;` (NTCC:1937). Nets 1-3 default to `duplicatePathsPower` (NTCC:1960-1965); nets 4-5 to `duplicatePathsDac` (NTCC:1967-1972); everything else falls to the skip ladder:
  - real GPIO / UART (NTCC:1976-1979)
  - `node1 >= FAKE_GPIO_1 && node1 <= FAKE_GPIO_32` (NTCC:1983-1987)
  - `pathType == VIRTUAL` (NTCC:1990-1992)
  - **ADC — confirmed at NTCC:1994-2000:**
```
// Don't duplicate bridges connecting to ADC nodes (high-impedance inputs don't benefit from parallel paths)
if (!shouldSkipDuplicates) {
  if ((node1 >= ADC0 && node1 <= ADC4) || node1 == ADC7 ||
      (node2 >= ADC0 && node2 <= ADC4) || node2 == ADC7) {
    shouldSkipDuplicates = true;
  }
}
```
  - then `bridgeDuplicates = shouldSkipDuplicates ? 0 : jumperlessConfig.routing.stack_paths;` (NTCC:2003-2005).
  So yes: **any bridge touching ADC0-ADC4 or ADC7 gets dup = 0.**
- `routingDuplicatePathNow` (NTCC:563) is set per-iteration from `paths[i].duplicate` in `commitPaths` (NTCC:2340) and `resolveUncommittedHops` (NTCC:4685), and cleared at the tail of `resolveAltPaths` (NTCC:4478). The header comment (NTCC:558-562) states all three loops set/clear it and that `RouteSafety.cpp` never calls `freeOrSameNetY()`/`setChipYStatusSafe()`, so it cannot leak into ephemeral tap planning.
- **"Last two virgin K y-rows" guard** — NTCC:565-581:
```
static int virginChipKYRowCount(void) { for y in 0..7: if chipStates[CHIP_K].yStatus[y] == -1 count++ }
// True when a duplicate path may not take this row: the row is virgin on chip
// K and 2 or fewer virgin K rows remain (the candidate row included).
static bool reservedKRowForSenseTaps(int chip, int y) {
  return routingDuplicatePathNow && chip == CHIP_K &&
         chipStates[CHIP_K].yStatus[y] == -1 && virginChipKYRowCount() <= 2;
}
```
enforced as the first test in `freeOrSameNetY` (NTCC:4514-4516). A duplicate refused here ends up wiped by `couldntFindPath`'s duplicate branch (NTCC:4622-4634).

---

## 5. Post-routing hooks and exact rebuild order

**`refreshConnections(ledShowOption, fillUnused, clean)` — Commands.cpp:201**
1. `waitCore2()` (231) → `holdCore1Frames()` (233) → `core1busy = true` (235)
2. **`infraEvaluate()`** (241) — "Converge system-owned connections ... BEFORE loading bridges into the router" (236-240)
3. `cancelStaleBypass()` (245)
4. `clearAllNTCC()` (246)
5. `loadBridgesFromState()` (251)
6. `getNodesToConnect()` (254)
7. `globalState.display.reconcileAfterRebuild()` (259); `partsReassertNetNames(globalState)` (262); `partLabels.requestRun()` (263)
8. `rebuildChangedNetColorsFromBridges()` (265)
9. **`bridgesToPaths()`** (268)
10. `checkChangedNetColors(-1)` (271); `chooseShownReadings()` (272)
11. `releaseCore1Frames()` (280); `core1busy = false` (281)
12. `core1req::post(REQ_SEND, SEND_PATHS | maybe SEND_CLEAN)` (292-294) + `xbarLatRequest()` (295), **then waits for core 2** (297+)

**`refreshLocalConnections` — Commands.cpp:370**: `cancelStaleBypass()` (421) → `clearAllNTCC()` (422) → `core1busy = true` (423) → **`infraEvaluate()` (426)** → `loadBridgesFromState()` (429) → `getNodesToConnect()` (431) → reconcile/parts (438-442) → `rebuildChangedNetColorsFromBridges()` (444) → **`bridgesToPaths()` (449)** → `checkChangedNetColors(-1)` (454) → `chooseShownReadings()` (459) → `setGPIO()` (465) → `core1busy = false` (475) → `core1req::post(REQ_BYPASS, 1u)` (481) — **does not wait**.

**`fastRefresh` — Commands.cpp:582**: wait for core 2 (611-620) → `core1busy = true` (628) → **`infraEvaluate()` (632)** → `cancelStaleBypass()` (640) → `clearAllNTCC()` (641) → `loadBridgesFromState()` (647) → `getNodesToConnect()` (653) → **`bridgesToPaths(0, 0, 0)` (674)** (no fillUnused, skips reconcile/colors/readings) → `setGPIO()` (696) → `core1busy = false` (702) → `core1req::post(REQ_BYPASS, 1u)` (707) — **does not wait**.

**`finalizeFakeGpioAfterRouting()` is not called by any of the three.** It is called by the *callers*, always in the shape "populate slots → refresh → finalize → apply":
- `States.cpp:3453 initializeFakeGpioFromLoadedState()` → `:3459 refreshConnections(-1, 1, 1)` → **`:3462 finalizeFakeGpioAfterRouting()`** → `:3466 applyStateToHardware()`
- `States.cpp:3703 initializeFakeGpioFromLoadedState()` → `:3705 refreshConnections(-1,1,1)` → **`:3706 finalizeFakeGpioAfterRouting()`** → `:3711 applyStateToHardware()`
- `JsonState.cpp:818`, `SingleCharCommands.cpp:4262`, `main.cpp:1504` — same hook point (I read only the call lines for these three, not their surrounding sequences).
- The `refreshLocalConnections` call sites I read (`Commands.cpp:859`, `:884`, `:977`, `:987`) do **not** call it.

**Where a "virtual ADC finalize" hook belongs.** Same slot as `finalizeFakeGpioAfterRouting`: after the refresh returns and before `applyStateToHardware()`. The constraint is physical, not stylistic — finalize immediately issues raw disconnects (`FakeGpio.cpp:1182-1190` `sendXYrawUnchecked(...,0)` plus `lastChipXY` clears), so it must run **after** the bulk send has completed or the send will re-close the crosspoints it just opened. `refreshConnections` waits for core 2 (Commands.cpp:297+) and is therefore safe; `refreshLocalConnections` (post `REQ_BYPASS` at :481) and `fastRefresh` (:707) return before the send lands, so a finalize hook wired there would race unless it first does `waitCore2()`.

---

## 6. `findChipKYForNode` / `extractPathHopsForNode`

Both `static` in `src/sensing/FakeGpio.cpp`.

**`findChipKYForNode(int node) -> int8_t`** — FakeGpio.cpp:92-109
```
for (int i = 0; i < globalState.connections.numPaths; i++) {
    const pathStruct& path = ...paths[i];
    if (path.node1 == node || path.node2 == node) {
        for (int j = 0; j < 4; j++) {
            if (path.chip[j] == CHIP_K) { return path.y[j]; }
        }
    }
}
return -1;
```
Returns the K y-row, or -1. Limitations:
- **First matching path only** — but note it does *not* `return` after the first node match; it falls through to the next path if that path has no K hop, so it does scan on. It does return on the first K hop found across paths.
- Bound is `globalState.connections.numPaths`, set once at NTCC:1561 **before** `fillUnusedPaths` appends duplicates — so duplicates and the Graphics VIRTUAL path (which bumps only `numberOfPaths`, Graphics.cpp:720) are invisible to it.
- Returns `path.y[j]` unvalidated; if the path half-failed this can be `-1` or `-2`.
- Only scans `chip[0..3]`; `pathStruct` declares `x[6]`/`y[6]` (MatrixState.h:62-63) but `chip[4]` (:61), so 4 is the real cap.
- No altPath awareness beyond `chip[2]`/`chip[3]` being in the same array.

**`extractPathHopsForNode(int node, int8_t* outChips, int8_t* outX, int8_t* outY) -> int8_t`** — FakeGpio.cpp:115-133
```
for (int i = 0; i < globalState.connections.numPaths; i++) {
    if (path.node1 != node && path.node2 != node) continue;
    int8_t count = 0;
    for (int j = 0; j < 4 && count < TDM_MAX_HOPS; j++) {
        if (path.chip[j] < 0) continue;
        if (path.chip[j] == CHIP_K) continue;   // line 124
        outChips[count] = path.chip[j]; outX[count] = path.x[j]; outY[count] = path.y[j]; count++;
    }
    return count;    // line 130
}
return 0;
```
- `TDM_MAX_HOPS == 4` (TimeDomainMultiplexer.h:26); `TDM_MAX_CHANNELS == 32` (:23). So the cap is `min(4, 4)`.
- **Returns after the first path whose endpoint matches** (line 130) even if `count == 0` — a node that appears in several bridges can yield the wrong path's hops.
- **Doc/code mismatch:** the header comment says "Stores ALL hops **INCLUDING** the chip K hop (the ADC crosspoint)" (line 113), but line 124 explicitly `continue`s past `CHIP_K`. The behaviour matches the inline comment at 123 and matches `TimeDomainMultiplexer::switchTo`, which adds the K crosspoint separately (TimeDomainMultiplexer.cpp:226). The line-113 comment is wrong.
- Copies `x`/`y` blindly; `setChannelPath` re-guards with `if (ch.hopChip[i] < 0 || ch.hopX[i] < 0 || ch.hopY[i] < 0) continue;` (TimeDomainMultiplexer.cpp:185).
- Same `numPaths` bound as above.

Callers: FakeGpio.cpp:421, 559, 610, 721/726, 901/906, 943, 1131, 1167/1172.

---

## 7. Routing to ADC4 (chip L X3) and ADC7

**ADC4 (114): yes, one xMap entry, on chip L index 3.** `MatrixState.cpp:153` (chip `{11,'L'}` block starting :150):
```
{30, 60, ROUTABLE_BUFFER_OUT, ADC4_5V, RP_GPIO_20, ... , CHIP_I, CHIP_J, CHIP_K, GND}
```
i.e. `xMap[3] == ADC4_5V == ADC4 == 114` (JumperlessDefines.h:433-434 `#define ADC4 114` / `#define ADC4_5V 114`). Mirrored in `rev5plusXmap` :171 and `rev4minusXmap` :186. `findStartAndEndChips` finds exactly 1 candidate → `chip[1] = CHIP_L` (NTCC:5944-5947). `assignPathType` additionally hard-codes 114 into the SF-first swap test (NTCC:6099-6102 for node1, NTCC:6139-6142 for node2), alongside 116/117.

**ADC7 (115): no xMap entry anywhere → not routable.** I read all three tables in full — the primary `chipStatus` initializers (MatrixState.cpp:128-155), `rev5plusXmap` (:159-172) and `rev4minusXmap` (:174-188). `ADC7`/`115` appears in none of them; the only `MatrixState.cpp` mention is the name table `{"probe", ADC7_PROBE}` at :285. Consequences:
- `findStartAndEndChips` `case GND ... 141` scans `chipStates[8..11].xMap` (NTCC:5930-5933) and finds `candidatesFound == 0` → `chip[twice]` stays `-1` from the memset → the path is unroutable and `couldntFindPath` reports it.
- `isConnectable(115)` returns **false** (MatrixState.cpp:439-463: no yMap/xMap hit, and 115 is outside the `FAKE_GPIO_1..FAKE_GPIO_32` escape at :459).
- `assignPathType` does **not** special-case 115 (only 114/116/117, NTCC:6099-6102 / 6139-6142).
- ADC7 is nonetheless recognised everywhere else as a *readable* channel: `NetManager.cpp:113-114` name table, `:1238-1239` `chooseShownReadings`, `JsonState.cpp:207-209`, `RouteSafety.cpp:62`, `NTCC:1996-1997`. It is the probe tip — read directly, never switched through the fabric.

---

## 8. The single-char dump

**Correction to the premise: the dump at SingleCharCommands.cpp ~2054-2083 is bound to `'b'`, not `'n'`.**
```
SingleCharCommands.cpp:470-472
  registerCommand( 'b', "show bridge array",
                   "Display the internal bridge array and paths.",
                   cmd_showBridgeArray, MENU_STANDARD, CAT_DISPLAY );
```
`'n'` is `cmd_showNetlist` (SingleCharCommands.cpp:466-468, body at :1804-1814): it calls `couldntFindPath(1)` (:1806) then `listNets(...)` (:1812) — netlist only, no paths, no chip status.

`cmd_showBridgeArray` (SingleCharCommands.cpp:2054-2083) takes an optional arg (`0` = hide dupes, `2` = show all; default 1, :2058-2066) and prints, in order:
```
:2068-2073  "pathDuplicates: " jumperlessConfig.routing.stack_paths
            "dacDuplicates: "  jumperlessConfig.routing.stack_dacs
            "railsDuplicates: " jumperlessConfig.routing.stack_rails
:2074       couldntFindPath( 1 );          // re-scans and prints "Couldn't find a path for ..." lines
:2075-2076  "Bridge Array"  printBridgeArray( target );
:2077-2078  "Paths"         printPathsCompact( showDupes, target );
:2079-2080  "Chip Status"   printChipStatus( target );
```
`printPathsCompact` (NTCC:5646) prints `numberOfPaths`, `numberOfNets`, then the header at NTCC:5657-5659 `path net node1 chip0 x0 y0 node2 chip1 x1 y1 altPath sameChp dup pathType chip2 x2 y2`, with `chip2/x2/y2/x3/y3` only when `chip[2] != -1` (NTCC:5722-5733) and `chip[3]` appended when set (:5735-5739). `printPathType` renders `VIRTUAL` for type 10 and `"Not Assigned"` for anything else (NTCC:6659-6683). `printChipStatus` (NTCC:5756) dumps `xStatus[0..15]` and `yStatus[0..7]` per chip — this is the direct read-out of the K y-lane occupancy discussed in §1b.

For verification of a virtual-ADC change, `b2` gives you: the dup config values, every unroutable pair, per-path `chip1/x1/y1` (the K crosspoint) and `dup`, plus `chipStates[K].yStatus[]`. `c` / `C` (`cmd_showCrossbar` :2085, `cmd_showCrossbarFull` :2110) give the crosspoint matrix.

---

## Risks for a virtual-ADC expansion

Assume a new node range `VIRT_ADC_0 ..` expanded like `FAKE_GP_IN_x`.

1. **The K y-lane is fixed by breadboard chip, not allocated.** `yMapSFc1 = paths[i].chip[0]` (NTCC:2607) means *N* virtual ADCs on rows served by the same A-H chip all contend for one row. They can only coexist by sharing a net number (the `FAKE_GPIO_TDM_NET` trick, NTCC:170) **and** only on the `allowStacking == 1` retry (NTCC:2345-2346, 4518). Any new range needs its own merge/restore pair or it will collide on the very first same-chip pair. There is only one `FAKE_GPIO_TDM_NET` constant (JumperlessDefines.h:564 `MAX_NETS - 2`); a second range needs a second sentinel (`MAX_NETS - 3`?) or the two merges will alias each other.

2. **Hard 8-lane ceiling and the sense-tap reservation.** Chip K has 8 y-rows (`yStatus[8]`, MatrixState.h:109; `chipStates[10].yMap` = A..H, MatrixState.cpp:148). "Any number of ADCs" is only true across ≤8 *distinct breadboard chips* worth of concurrent K occupancy, shared with every other net that needs K. `reservedKRowForSenseTaps` (NTCC:577-581) protects the last two virgin rows **only against duplicates** — virtual-ADC primaries will happily eat them and starve `NetVoltageScan`'s `buildEphemeralRoute` (RouteSafety.cpp:786, tiered at :590).

3. **Node-ID range gaps at 181.** `FAKE_GPIO_32 == FAKE_GP_IN_23 == 181` (JumperlessDefines.h:598), so slots 24-31 (182-189) already fall outside four range tests. A new range above 189 inherits the same class of bug:
   - `NTCC:1983-1987` fillUnusedPaths dup-skip → new virtual nodes would get `stack_paths` duplicates.
   - `NTCC:1509-1512` `sortPathsByNet` visibility → net invisible.
   - `NTCC:1519-1521` `nets[j].virtual_net` marking → not flagged virtual.
   - `MatrixState.cpp:459-461` `isConnectable()` → returns false (used by `Probing.cpp:3548`).

4. **`case GND ... 141` is the SF gateway.** `findStartAndEndChips` (NTCC:5926) and both `nodeType` ladders in `assignPathType` (NTCC:6134-6136, 6153-6155 `node >= GND && node <= 141`) only classify 100-141. A 190+ id must be handled by an explicit `case` that falls through into `case GND ... 141` **and** must write the expanded node back into `paths[].node1/node2` before `assignPathType` runs, exactly as NTCC:5915-5921 does. Miss the write-back and the path gets `nodeType` unset, `pathType` never assigned (stays whatever the memset left) and `printPathType` prints "Not Assigned".

5. **Every reverse-mapping site must learn the new range.** After expansion, `paths[]` holds `ADCn`, so display/telemetry has to walk back by `netIndex`. The existing set: `NetManager.cpp:152` (name table), `:1246-1252` (`chooseShownReadings`, `netsShowingSpecial`/`netsFakeGpioSlot`), `:1998-2009` (bridge↔path match), `:2075-2117` (`printNodeOrName`), `JsonState.cpp:234-245`, `Peripherals.cpp:1127-1143`, `RoutableGpio.cpp:64-93` (`gpioState/gpioReading/gpioNet[50]` are exactly sized for 10+8+32 — a new range needs the arrays grown), `InfraPaths.cpp:822` (`infraAdcUserClaimed` exemption — without it, a virtual ADC's own bridge will look like a user claim and lock the pool against itself), `RouteSafety.cpp:380-390` (`validateAllPaths` short-detector exemption — without it, N virtual ADCs on one physical channel will be reported as mutual shorts and get `skip = true` at RouteSafety.cpp:424).

6. **The overlap validator exemption is separate from the safety validator.** `NTCC:5280-5299` only suppresses the *report* in `checkForOverlappingPaths`; `RouteSafety.cpp:404` is what actually prevents `skip = true`. Both need the new predicate. Both re-derive membership by scanning `bridges[]` for the virtual id (NTCC:5286/5289, RouteSafety.cpp:386-387) — an O(paths² × bridges) scan that will get expensive if the count grows.

7. **`infraAcquireAdc` mask is `0x0F`.** `TimeDomainMultiplexer.cpp:320` `adcChannel = infraAcquireAdc(INFRA_ADC_TDM, 0x0F, false);` and `getChipKX()` returns `8 + adcChannel` with a hard `adcChannel > 3` bail (`:312-315`). Virtual ADCs are therefore confined to ADC0-3 / chip K x8-x11. Extending to ADC4 means chip **L** x3 (MatrixState.cpp:153) — a completely different chip whose y-lane geometry is the same `bb chip index` lookup but whose x-lane and TDM crosspoint math (`getChipKX`) do not generalise. Also `infraReleaseAdc` frees *all* channels a `InfraAdcUser` owns (InfraPaths.h:168-171), so a new consumer must get its own enumerator, not reuse `INFRA_ADC_TDM`.

8. **`globalState.connections.numPaths` excludes duplicates and the VIRTUAL path.** Set once at NTCC:1561 before `fillUnusedPaths`. Any new finalize helper modelled on `findChipKYForNode`/`extractPathHopsForNode` (FakeGpio.cpp:93, :116) inherits that blind spot, plus the "first matching path wins" bug (FakeGpio.cpp:130) and the 4-hop cap.

9. **Finalize-after-send ordering.** A finalize hook that opens crosspoints (as FakeGpio.cpp:1180-1190 does) must run after the bulk send completes. Only `refreshConnections` waits (Commands.cpp:297+); `refreshLocalConnections` (:481) and `fastRefresh` (:707) post `REQ_BYPASS` and return. Also `fastRefresh` passes `fillUnused = 0` (Commands.cpp:674), so path indices and duplicate layout differ between refresh flavours — a finalize that caches path indices will go stale.

10. **`restoreFakeGpioInputNets` Phase 3 is a heuristic.** NTCC:230-267 rewrites any `xStatus`/`yStatus` still holding net 58 by finding *the first path* with a hop on that chip+x (or chip+y), and writes `-1` (frees the row) if none matches (NTCC:246, 264). With more virtual paths sharing rows, mis-attribution and spurious row-freeing become likelier; a second virtual range doubles the exposure.

11. **`resolveUncommittedHops` cannot rescue a failed BB→K path** (only `-2` markers, NTCC:4692-4708), and the failure is silent apart from a serial line — the bridge stays in `bridges[]` and is retried every rebuild, so a K-full condition produces a permanent, repeating "Couldn't find a path" with a dead TDM channel (`chipKY = -1`, TimeDomainMultiplexer.cpp:198/290) rather than a user-visible error. Any virtual-ADC design should surface `unconnectablePaths[]` (NetsToChipConnections.h:24-25) the way `GuideChecks.cpp:476-479` and `PartMeasure.cpp:110-115` already do.

12. **Dead code to clean up while you're in here:** `assignPathType`'s virtual-node expansion into locals (NTCC:6058-6091) is never read (verified by scanning NTCC:6092-6215 — only comment hits). It is currently harmless because `findStartAndEndChips` already mutated the struct, but if a future change makes `assignPathType` runnable *before* expansion, the dead locals will look like they handle it and they do not.
