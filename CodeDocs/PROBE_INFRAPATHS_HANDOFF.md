# Probe Power, InfraPaths & the Reading Line — Handoff Notes

**Goal for the next session:** the work described here is committed and hardware-verified
(the commit trail is at the bottom; `usb-audio-uac2` is simply where the tree was, and
none of it relates to the USB-audio work also in flight on that branch). What remains is the "Open items" list at the bottom —
the biggest are a second live-readout subsystem that never learned about the pinned
reading line, and a droop calibration that silently reverts to its default.

This document covers one connected arc: who is allowed to power the probe buffer, how
the firmware senses the probe switch, how a tap becomes a row number, and how a reading
reaches the terminal. They are one story because a wrong answer in the first breaks all
four.

---

## The hardware fact that drives all of it

**INA219 @0x41's 2 ohm shunt (R57) is hardwired in DAC_0's output path.** Nothing else
on the board is in series with it.

Consequences, all of which the firmware got wrong at some point in this arc:

- Switch-position sensing measures the probe LED's supply current through that shunt.
  It can therefore **only see a feed sourced from DAC0**. A DAC1 feed routes identically
  and works electrically, but is completely invisible to sensing.
- `measure_mode_output_voltage` is calibrated by the self test as a **DAC0** setting, so
  a runtime feed on any other source is being driven at an uncalibrated level.
- The probe cable self test's LED-delta step (its only end-to-end proof that pixels reach
  the probe) is likewise DAC0-only.

The earlier design fed the buffer from **DAC1** (2 crosspoints, same chip K as BUFFER_IN,
and it kept DAC0 free). That is why switch sensing was dead: `checkSwitchPosition()`
early-returned `1` whenever `probePowerDAC == 1`, on every call, forever. Flipping the
physical switch did nothing.

**Current model: the candidate chain is `DAC0 -> RP_GPIO_8 .. RP_GPIO_1`. There is no
DAC1 candidate at all** — DAC1 is the user's general-purpose DAC, and a DAC1 feed would
be sensing-blind by construction.

---

## InfraPaths (`src/routing/InfraPaths.cpp`, `.h`)

The arbitration layer for connections the *firmware* needs, as opposed to connections the
*user* asked for. It replaced a scattering of open-coded bridge adds, DAC swaps and
"is this node free?" scans.

### The model

An **InfraFunction** (`probe_power`, `oled_i2c`, `serial_1`) owns a service and has an
ordered list of **candidates**. Each candidate resolves to node pairs from live state,
declares whether it is `viable()` right now, and gets `onActivate` / `onDeactivate` hooks.
Evaluation walks the list in preference order and takes the first viable one — which is
also how automatic switch-*back* works: a more-preferred candidate becoming viable takes
over on the next rebuild. Rebuilds are user-action driven, so this cannot oscillate.

### Invariants worth not breaking

- **Evaluation runs at the head of the three hardware-applying rebuilds**
  (`refreshConnections`, `refreshLocalConnections`, `fastRefresh`) — deliberately *not*
  `loadBridgesFromState`, which the slot preview also calls.
- **Infra bridges are never serialized** (skipped in both the `bridges` and `nets` YAML
  sections — there are two independent serializers and the first fix missed one),
  **never enter undo** (`isAutoManagedBridge` -> `infraIsBridge`), and are **never
  duplicated** (`dup = 0`, so the path resistance stays a single known quantity).
- **Raw pair helpers bypass `addConnection`/`removeConnection` on purpose.** Their
  `markDirty` / undo / FakeGpio / refresh side effects are all wrong for a system bridge
  being mutated mid-rebuild. They keep an active-pair registry (the `isEphemeralConnection`
  precedent — a pair-keyed side list, so no bookkeeping is needed at the bridge-array
  mutation sites) and fix up ephemeral `bridgeIndex` after a removal.
- **The user always wins.** There is no rejection path. If a user connection lands on a
  function's own service node, `userOverridden()` fires, the function tears its path down
  *first* (so the user's connection routes cleanly in the same rebuild), prints one line,
  and stands aside. It re-converges automatically on disconnect. Take BUFFER_IN and the
  probe simply goes unpowered — that is the intended behavior, not a failure.

### Two subtleties that cost real debugging time

**Forced candidates do not yield.** `infraForceCandidate(fn, idx)` is how the self test
and calibration pin a specific feed. Those flows then add their own instrumentation
bridges on the service node — the tip test's droop phase loads BUFFER_IN through the
ISENSE shunt — which looks exactly like a user claim and made the function yield *mid
measurement*, tearing down the feed it had just been told to use. Evaluation now skips the
yield check while a candidate is forced.

**The DAC park epoch.** The feed parks its DAC at `measure_mode_output_voltage` and skips
the write when already parked — but that check reads `globalState.power`. Several callers
write the MCP4728 with `save = 0` (hardware only, state untouched): the self test's
normalize step, `calibrateDacs()`, the wave generator (whose default channel is DAC0), and
MicroPython `dac_set(save=False)`. After any of those the MCP4728 sits at a level the state does
not know about — 0 V after the self test's normalize, the last sweep step after
`calibrateDacs()`, the last streamed sample after the wave generator, whatever the script
asked for after `dac_set(save=False)` — behind an in-window state value, and the
skip-if-parked guard never re-writes it. Probe dead until reboot. Those sites call `infraDacParkEpochBump()`, which forces one unconditional
re-write on the next park. **Any new blind DAC write must bump the epoch.**

### `probePowerDAC` is a vestige — ask `infraProbePowerSource()` instead

A global `probePowerDAC` ("which DAC is the probe's") survives as a compatibility view
pinned to 0 by evaluation. Every writer outside InfraPaths is gone and it is down to a
**single** remaining read — a condition in `probeMode` whose entire body is commented
out, i.e. already dead. **New code must ask `infraProbePowerSource()`.**

Retiring it is four deletions: the dead `if` in `probeMode` (the condition reads
`probePowerDAC`, its body is entirely commented out), the `int& probePowerDAC` definition
in `Probing.cpp`, the member and `extern` in `Probing.h`, and the `probePowerDAC = 0`
write at the tail of `infraEvaluate()`. That was done and reverted during this session —
it builds clean on all three environments and passes `test_infra_paths` 14/14, but a
self-test crossbar anomaly appeared in the same window and the board was disconnected
before it could be cleared (see Open items), so it was backed out rather than committed on
a hunch.

The related config key `dacs.probe_power_dac` is still parsed and written for file
compatibility but is deliberately **not applied** — boards that ran the old firmware carry
`probe_power_dac = 1`, and re-applying that on every config change used to re-taint the
view and arm stale `== 1` branches (one of which wrote the user's voltage onto DAC0, the
live feed).

### Ephemeral ADC pool

The sibling mechanism: consumers that need *an* ADC rather than a specific one
(`INFRA_ADC_TDM`, `INFRA_ADC_NVSCAN`, `INFRA_ADC_MEASURE`) acquire from a pool. This
replaced `pickScanAdc`, `isAdcInUseByOtherConnections` and `MeasureMode::findUnusedADC`,
all of which scanned independently and disagreed. A user bridging an ADC node claims it
out of the pool the same way a user claims any other resource.

---

## Switch sensing (`Probing::checkSwitchPosition`)

Dispatch follows the **live feed source**, not a config flag:

| Feed | Sensing method |
| --- | --- |
| DAC0 | INA1 shunt current (the only feed it can see) |
| a routable GPIO | ADC7 voltage droop (see the dead-branch note below) |
| none (yielded, or every candidate claimed) | hold the last known position — no current signature exists, so do not guess |

There is a DAC0-swap fallback in the GPIO branch, but **it is currently unreachable**:
it runs only when `gpioDroopCurrentEstimate()` fails, and that function's single failure
return is guarded by the same `s_gpioPowerIdx < 0` condition that already skips the whole
branch. Either give it a real trigger or delete it — do not rely on it as a safety net.

The droop model treats the GPIO pad impedance plus crosspoint resistance as a free shunt:
`I = (V0 - ADC7) / R`. `probe_droop_ohms` is measured per board by the self test
(~167-177 ohm on this unit); the fallback when uncalibrated is an empirical 30 ohm.

`debug.probe_power_gpio` is **deprecated and ignored**. It is still parsed and written so
old config files round-trip, but it has no behavioral effect and must not regain one —
setting it used to force-*enable* probe power as a side effect.

---

## Measure-mode decode: why taps hit neighboring rows

Two independent causes, both fixed, both worth understanding before touching
`readProbeRaw()` / `probeMapRange()`.

**Measured on hardware:** every connect/disconnect rebuild wanders the probe tip by about
**70 mV** for a while afterward — more than a full row step at the top of the pad ladder.
This happens on **any** feed (DAC0 and GPIO measured identically: ~70 mV span during
churn vs ~9-13 mV idle), and LED activity does *not* move it. It is rebuild-correlated,
not a property of DAC0 being noisy.

1. **Ratiometric scale staleness.** The decode is `pad_reading x ladder_ratio` against
   endpoints scaled by `tip_voltage / 3.3`. The tip voltage cancels **exactly** — but only
   if both reads see the same tip voltage. The scale was cached up to 25 ms, which
   decoupled the two reads across that wander. Now 2 ms: still one read per decode pass,
   but the ratio re-forms fast enough that feed wander cancels.

2. **Connect mode re-accepting a held touch.** A reading more than 5 counts from the last
   accepted value counts as "new". A *held* contact drifting with that same wander crosses
   the threshold, decodes as the neighboring row, and chains a connection onto it. Connect
   mode (measure position only) now latches one node per touch and re-arms only on a burst
   back at the no-touch floor. The re-arm deliberately lives **outside** the accept branch,
   next to the median computation: a released probe reads the floor, which fails the
   `minimum_probe_reading` gate, so a re-arm placed inside the accept path can never run —
   that bug accepted exactly one tap and then nothing, ever.

Select position is untouched by both — its tip is driven directly from `PROBE_PIN`.

---

## The pinned reading line (`src/ReadingDisplay.cpp`)

One renderer for every live reading — probe/encoder highlighting, measure mode, the
rail/DAC adjuster — so a voltage looks the same wherever it came from.

The serial half is **pinned two rows above the line the user types on**:

```
3.29 V  row 25          <- rewritten in place
                        <- blank spacer row
> user input + cursor   <- cursor restored here, mid-word
```

Sequence per update: `ESC 7` (DECSC) -> `CSI 2 A` (CUU) -> `\r` + `CSI 2 K` (EL, Ps=2 =
erase all) -> text -> `ESC 8` (DECRC). Reserved once with `"\n\r\n\r"`.

Details that matter if you touch it:

- **`EL` with Ps=2, not the default Ps=0.** The old code printed a run of spaces, which
  only overwrites rightward and never clears — a shorter reading left the tail of a longer
  one behind. (`Apps.cpp`'s probe-calibration print carries the same warning.)
- **DECSC/DECRC, not `CSI s`/`CSI u`.** The ANSI.SYS forms are constrained when
  left/right margins are enabled.
- **`"\x1b" "7"`, split.** Written `"\x1b7"` the compiler parses it as one overlong hex
  escape, not ESC followed by `'7'`.
- **`"\n\r"`, not bare `"\n"`.** Over a raw serial link LF moves down but holds the
  column, leaving the input line — and every later DECRC — at a stale column.
- **The anchor is cursor-relative.** Anything that scrolls the terminal moves it. Contexts
  that take the terminal over (`probeMode`, `clickMenu`, `runApp`) call
  `ReadingDisplay::resetLastShown()` on entry, which drops the anchor *without* erasing —
  the reserved rows have scrolled somewhere unknown by then, and a blind CUU+EL would wipe
  one of *their* lines. The next reading pins a fresh pair below their output. Those loops
  cannot repaint underneath themselves: they run on `jOS.serviceCritical()`, which
  dispatches only CRITICAL services, and MeasureMode is HIGH.

### The myth that let this rot

`MeasureMode::service()` returns `BUSY` under a comment claiming it stops other services.
**It does not.** `JumperlOS.h` documents BUSY as "actively working but non-blocking", and
`serviceAll()` latches `blockingService` only on `BLOCKING`. The early return skips the
rest of that one function. ProbePads (20 Hz), ProbeSwitch (500 ms), Peripherals (150 ms)
and Probing (100 Hz) all keep printing to the same terminal while a measurement is live.
The comment is corrected in place; assume **any** service can print at any time.

Removed in the same pass: `checkPads()` blanked 33 columns at the cursor,
unconditionally, at 20 Hz, for as long as the tip rested on a row — i.e. all of measure
mode — for prints that are now all commented out or behind `&& false`.

---

## How to verify

```bash
pio run -e jumperless_v5 -e jumperless_og -e jumperless_v5_debug
pio run -e jumperless_v5_debug -t upload    # kill background openocd first
```

- `test/hil/test_infra_paths.py` — 14 checks: boot feed on DAC_0, exactly one routed path
  with `dup = 0`, relocation to a GPIO when the user claims DAC0, switch-back on release,
  survival across `nodes_clear()`, slot-file purity, the permissive yield cycle, and a
  RouteSafety self-check with the feed live.
- `` `self_test `` on the terminal — probe_cable, crossbar, tip_voltage (which also
  measures and stores the droop resistance), psram, peripherals.
- `get_switch_position()` from MicroPython reads the live sensed position (0 = measure,
  1 = select) — the fastest check that sensing is alive at all.
- `adc_get(n)` now reaches channels 0-7: 0-3 breadboard, 4 the 0-5 V channel, 5 pad sense,
  7 probe tip. The old 0-3 limit predated three of those.
- `i@` prints the InfraPaths status (active function, candidate, resolved node, droop
  ohms). `b` dumps bridges and the routed path table.

**HIL gotcha:** a stale Python process holding `/dev/cu.usbmodemJLV5port1` produces
"device reports readiness to read but returned no data". `lsof -t` the port and kill it;
it is not a board fault.

---

## Open items

**A second live-readout subsystem.** `Peripherals::showMeasurements()` prints at the
cursor every 150 ms whenever `showReadings >= 1` (default 0). It never learned about the
pinned line, so with readings enabled the two fight for the input row. It also uses the
spaces-*before*-CR idiom, which pads rightward and never clears. Either route it through
`ReadingDisplay::emitLiveSerialLine()` or give it its own pinned row.

**Oscilloscope mode pins a corpse.** `updateOscopeDisplay()` emits no serial output at
all, so a voltage pinned before the switch to oscope mode sits there looking live.
`clearLiveSerialLine()` on oscope entry would do it.

**`probe_droop_ohms` reverts.** The self test measures it (167 ohm, and the run summary
confirms `droopR:167`) and `saveConfig()` persists it, yet `/config.txt` later reads
`probe_droop_ohms = 0.0` with `probe_droop_v0` back at the validator's 3.35 fallback,
while neighbouring calibration keys keep their values. Something zeroes both droop keys
after the save. GPIO-feed sensing falls back to the computed 30 ohm estimate meanwhile, so
it is degraded, not broken. (A separate session was started on this.)

**`connectArduino` / `disconnectArduino` fight `serial_1`.** They add and remove the exact
UART pairs the infra function owns. With `lock_connection = 1` the disconnect does not
stick, because evaluation re-adds them. Arguably the lock working as intended, but the
Arduino flashing flow deserves a deliberate decision rather than a race.

**Encoder DAC preview writes state without hardware.** The voltage adjuster's non-live
preview path writes `globalState.power.dacX` so the LEDs update, without touching the
MCP4728. A preview value outside the feed's `[2.80, 3.90]` viability window will relocate
the probe feed off a DAC that never actually moved. The fix belongs in the adjuster's
cancel path.

**`checkSwitchPosition`'s DAC0-swap fallback is dead code.** Its guard and
`gpioDroopCurrentEstimate()`'s only failure path test the same condition
(`s_gpioPowerIdx < 0`), so whenever a GPIO feed is live the droop estimate always
succeeds and the swap never runs. If it is ever revived, note the second latent bug it
carries: with DAC0 user-claimed while the feed is GPIO-sourced, the swap reads whatever
the user's DAC0 load draws.

---

## One loose end

A self-test run late in the session reported `crossbar: FAIL rowFail:1[20] gpioFail:1` —
row 20 reading 1.715 V instead of ~2.5 V, GPIO 6 loopback reading high 2.026 V / low
-0.733 V, and *every* row sagging (~2.42 V vs ~2.51 V in the passing runs just before).
It reproduced twice, then the board left the USB bus before it could be A/B'd against the
previous firmware.

The generalized sag plus a negative loopback reading is the signature of an external
load, and the runs bracket a physical event: the probe switch moved from measure to
select between the last pass and the first failure. No firmware change in that window
touches routing, DAC or ADC paths. Most likely environmental — but it is unconfirmed, so
re-run `` `self_test `` with nothing connected before trusting a crossbar pass again.

---

## Commit trail

| Commit | What |
| --- | --- |
| `c826a53` | InfraPaths goes permissive: user connections always win, locks yield |
| `3145681` | Forced candidates don't yield — fixes the droop-phase teardown |
| `2da17a3` | Self test judges the cable current window on the RAW shunt current |
| `a0184a0` | Probe power feeds from DAC0 — the only path switch sensing can see |
| `e6ef352` | Legacy sweep: retire every stale probe-power decision path |
| `422ad2e` | Hand the UI back to the firmware when a Python script exits |
| `af3ddb6` | One reading renderer, pinned above the input line; decode fixes |

Concurrency fixes from the same investigation (rotary encoder bounds, NetVoltageScan tap
budget, wavegen-aware OLED gating, bounded `fs_mutex` acquires, config-save storms) landed
on `main` earlier and are not part of this branch.

(These commits sit on `usb-audio-uac2`, which is simply where the tree was; they are
unrelated to the USB-audio work also in flight on it.)
