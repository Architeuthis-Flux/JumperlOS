---
name: Routable GPIO on-board control
overview: "Consolidate all routable-GPIO handling into a new RoutableGpio module (register-truth pin functions, single state funnel), then build on-board GPIO configuration: a highlight+encoder options carousel, a BCD counter (menu, highlight, and logo-pad driven), a parts-style stay-in-menu GPIO settings app, and an OLED mirror of the \"Choose GPIO\" chooser."
todos:
  - id: create-module
    content: Create src/hardwarestuff/RoutableGpio.h/.cpp; move GPIO code from Peripherals.cpp/.h and PersistentStuff.cpp; single applyPinConfig funnel; central OG guard; drop dead code
    status: completed
  - id: register-truth
    content: Replace gpio_function_map[] with live routableGpioFunction() register reads; update all consumers; delete resync loop; add routableGpioAvailable() gate (infra/OLED/Python/probe claims, UART traffic-since-boot)
    status: completed
  - id: highlight-carousel
    content: Encoder-turn on highlighted GPIO net opens Direction/PWM/Pulls/BCD carousel modal; keep connect-toggle and remove-unhighlight
    status: completed
  - id: bcd-core
    content: Counter state (start, width 1-10, binary/BCD mode, value) in ConfigState + YAML; bit map incl. UART top bits; bcdApply/bcdIncrement; bcdSelfCheck assert
    status: completed
  - id: bcd-adjust
    content: bcdAdjust encoder counter modal + padActionTable bcd_up/bcd_down idle actions
    status: completed
  - id: menu-app
    content: "Parts-style GPIO settings app: menuTree rows, getActionCategory arm, apps[] entries, gpioSettingsLauncher with stay-in-menu loop"
    status: completed
  - id: oled-chooser
    content: OLED 128x32 chooser bitmap in chooseGPIO() + clearPrintShow mirror in chooseGPIOinputOutput()
    status: completed
  - id: verify
    content: Build V5 + OG targets, run bcdSelfCheck, bench-check highlight/menu/pad flows
    status: pending
isProject: false
---

# Routable GPIO: on-board settings, BCD counter, and module consolidation

## What exists today (from codebase recon)

- GPIO code spans [src/Peripherals.cpp](src/Peripherals.cpp) lines ~320–1810 (arrays, `initGPIO`, `setGPIO`, `readGPIO`, `printGPIOState`, `gpioReadWithFloating`), 2026–2196 (`probeToggle`/`toggleGPIO`), 2374–2476 (`anyGpio*Connected`), 2929–3417 (fast + slow PWM) — plus `updateStateFromGPIOConfig`/`updateGPIOConfigFromState` and the `readSettingsFromConfig` GPIO block in [src/remembering/PersistentStuff.cpp](src/remembering/PersistentStuff.cpp).
- `gpio_function_map[10]` drifts from the real registers in 4 confirmed places (`stopPWM`, `stopSlowPWM`, `oled.cpp` teardown, probe buffer-power claim), and the two `PersistentStuff` sync gates silently no-op when it's stale. `gpioDef` is defined in the header (per-TU copy, ODR-mismatched `extern` in `States.cpp:57`).
- Your `$GPIO$`/`$BCD$` menuTree entries are dead: `getActionCategory()` has no GPIO arm → `NOCATEGORY` → nothing happens. The `>n3`/`>n4` lines parse as node pickers, not number dials.
- The "adjust" model is highlight + **click** → blocking `VoltageAdjuster::adjust()` modal (Peripherals.cpp:3526); a bare turn is consumed by `encoderNetHighlight()` row scrolling. `Highlighting::wantsToHandleButtonPress()` (Highlighting.cpp:2381) already has an explicit "GPIO — don't handle yet" fall-through, and GPIO-output nets already get the 15 s persistent highlight.
- The "separate thing like parts" pattern: childless menuTree row → `getActionCategory` "Parts" arm → `APPSACTION` → `runApp()` → `partsAppLauncher()` (PartsApp.cpp:1433) — nested `while` loops, cursor memory, `inClickMenu = 1` re-asserted.
- `Probing::chooseGPIO()` (Probing.cpp:5305) prints the ASCII chooser + breadboard LEDs but never touches the OLED; sibling `chooseIsense()` already mirrors via `oled.clearPrintShow`. OLED is 128x32, `oled.displayBitmap()` = Adafruit GFX MSB-first row-major.
- Logo-pad idle dispatch exists: `runPadIdleAction()` (Probing.cpp:7162), `padActionTable` (configManager.h:521, 29 entries: GPIO toggle/high/low ×8, DAC ±0.25 V ×4).

## Phase 1 — New module `src/hardwarestuff/RoutableGpio.h/.cpp`

Move (not rewrite) into the new module:

- From `Peripherals.cpp/.h`: `gpioState/gpioReading/gpioNet/gpioReadFloating` arrays, `gpioDef` (fix: define once in the .cpp, `extern const` in the header; delete the stray extern in `States.cpp:57`), `initGPIO`, `setGPIO`, `readGPIO`, `printGPIOState`, `gpioReadWithFloating`, `getGPIOIndexFromPin`, `gpio_function_name_for_pin`, `probeToggle`/`toggleGPIO`, `anyGpio*Connected`/`anythingInteractiveConnected`, `erattaClearGPIO`, the whole PWM + slow-PWM family. `Peripherals.h` keeps `#include "hardwarestuff/RoutableGpio.h"` so existing callers don't churn.
- From `PersistentStuff.cpp`: `updateStateFromGPIOConfig`, `updateGPIOConfigFromState`; the `readSettingsFromConfig` GPIO block becomes one call into the module.
- One `applyPinConfig(int idx)` funnel: every mutation writes hardware + `gpioState[]` + `globalState.config.*` + `markDirty()` in a single place, and fixes `toggleGPIO`'s `gpioOutputFound == -2` indexing bug in passing.
- One central OG_JUMPERLESS guard at the module boundary instead of the seven scattered copies (drop the unreachable `pwmUnavailableOnOG` duplicates).
- Drop dead code while moving: `convertPullToJumperless`, `printPWMState`, `gpioOutput[]`, `gpioIdleColors/Hues`, `highlightInteractable[]`, `handleHighlights` (no callers).

**Register truth:** replace `gpio_function_map[]` with `gpio_function_t routableGpioFunction(int idx)` reading `gpio_get_function(gpioDef[idx][0])` live. Update its consumers (`anyGpio*` predicates, the two PersistentStuff gates, `Highlighting.cpp` UART/function detection, `JsonState.cpp`, `DisplayBus.cpp`, `oled.cpp`, `AsyncPassthrough.cpp`, `PartsApp`/`PartMeasure` save-restore — those save/restore the live function via `gpio_set_function` instead). `jl_gpio_claim_pin`'s map-as-lock trick goes away; `gpioPythonOwned[]` already is the lock. Delete the `rebuildShownReadings()` resync loop — nothing left to resync.

**Pin availability:** one `routableGpioAvailable(int idx, char* ownerOut)` helper that every assignment surface (BCD range setup, settings app picker, carousel) consults. A pin is unavailable when:

- `infraOwnsNode(gpioDef[idx][1])` — InfraPaths feed claim ([src/routing/InfraPaths.h](src/routing/InfraPaths.h) line 61)
- `gpioState[idx] == 6` or DisplayBus `pinClaimed()` — OLED on crossbar GPIO 7/8 or soft-I2C on GP24/25
- `gpioPythonOwned[idx]`, or `idx == probeGpioPowerClaimIdx()` — MicroPython / probe buffer-power claims
- UART Tx/Rx (indices 8/9): unavailable only if `uartTrafficSinceBoot()` — a new accessor in AsyncPassthrough over the existing `s_last_usb_to_uart_data_time` + its UART→USB twin. A DTR toggle that phantom-connects the passthrough moves no bytes, so untouched UART pins stay assignable; assigning one re-muxes it to SIO through `applyPinConfig()`.

Unavailable pins render greyed/skipped with the owner named (e.g. "7 OLED", "2 routing").

## Phase 2 — Highlight + encoder: GPIO options carousel

In `encoderNetHighlight()` mode-1 (Highlighting.cpp:576), before the row scroller consumes UP/DOWN: if `brightenedNet` carries a routable GPIO (input, output, or UART pin), hand the turn to `RoutableGpio::optionsCarousel(gpioIdx)` — a blocking modal in the `VoltageAdjuster::adjust` idiom (`inClickMenu = 1`, `rotaryDivider = 8`, cancel on HELD / probe-remove, confirm on click / probe-connect, re-highlight the net on exit exactly like `adjustDACVoltage` does at Highlighting.cpp:2563-2585).

- Carousel items: **Direction — PWM — Pulls — BCD**, rendered on OLED (`clearPrintShowRich`: "GPIO 3" header + option row) and LED matrix (`b.print`).
- Sub-editors: Direction = Input/Output; Pulls = Down/Up/None/Keeper; PWM = Enabled On/Off, Freq and Duty via encoder-accelerated integer/float adjust (reuse `EncoderAccelerator`, live `setupPWM`/`stopPWM`); BCD = jump to Phase 3's counter (or range setup if no range defined).
- Probe connect-tap toggle of output GPIOs (`probeToggle`) and remove-to-unhighlight stay untouched.

## Phase 3 — BCD counter

- **State** (new fields in `ConfigState`, States.h ~line 241, + YAML serialize/deserialize + accessors): `bcdStart` (bank index, −1 = off), `bcdWidth` (1–10), `bcdMode` (0 = binary, 1 = BCD nibbles), `bcdValue`. Persists with the slot like the rest of GPIO config.
- **Pin order:** bits map LSB-first from `bcdStart` through GPIO 8 (index 7), then continue into UART Tx (index 8) and Rx (index 9) as the top bits — so start = 0, width = 10 gives a full 10-bit counter on GPIO 1–8 + Tx + Rx. UART pins only join a range if available per the Phase 1 gate (no passthrough traffic since boot); range setup skips unavailable pins.
- **Encoding:** binary by default — value 0..2^n−1, wrap on overflow. `bcdMode = 1` switches to BCD nibbles (each group of 4 pins = one decimal digit 0–9, the two-digit 7447 case); a 4-pin binary range counting 0–9 already is single-digit BCD. `bcdApply()` forces the range pins to SIO outputs and drives them; `bcdIncrement(±n)` wraps.
- **Counter modal** `bcdAdjust()`: same shape as `VoltageAdjuster::adjust` but integer — encoder ±1 with acceleration, value live on the pins, LED matrix big number + `ReadingDisplay::show("BCD", value)` for OLED/serial, click confirms / hold cancels-restores.
- **Range setup**: two-step encoder pick (start pin, then width), from the carousel's BCD entry and the menu app.
- **Logo pad**: extend `padActionTable` with `bcd_up` = 29, `bcd_down` = 30 and add the two cases to `runPadIdleAction()` (increment/decrement + wrap + OLED toast, mirroring the `dac_0_up` pattern) — tap-to-count on any of the four assignable pads.
- **Highlight**: a highlighted GPIO inside the BCD range makes the carousel's BCD item open `bcdAdjust()` directly — "turn encoder to count".
- Ponytail check: a small `bcdSelfCheck()` assert run (encode/wrap round-trip) reachable from the serial test path.

## Phase 4 — Click menu: parts-style GPIO app (stay in menu)

Replace the dead `$GPIO$`/`$BCD$` block in [src/remembering/menuTree.h](src/remembering/menuTree.h) with the Parts 4-way coupling:

- menuTree rows: top-level `"GPIO"` with children `"-Set    \31Pins"` and `"-BCD    \31Counter"`.
- `getActionCategory()` gets a `"GPIO"` → `APPSACTION` arm (Menus.cpp:3992 chain); `apps[]` gets `{"Set Pins", …, gpioSettingsLauncher}` and `{"BCD Counter", …, bcdMenuLauncher}` rows (Apps.cpp:109 style).
- `gpioSettingsLauncher()` (lives in RoutableGpio.cpp, modeled on `partsAppLauncher` PartsApp.cpp:1433): nested pickers with cursor memory — level 1: pin (1–8, Tx, Rx; picker line shows live state, e.g. "3  out HIGH"); level 2: Direction / Pull / PWM / BCD; level 3: value editor. **After applying a value it returns to level 2** — the stay-in-menu behavior; hold backs out one level, serial byte exits, teardown restores `inClickMenu`, divider, `showJogo32h()`.

## Phase 5 — OLED mirror of the "Choose GPIO" chooser

In `Probing::chooseGPIO()` alongside the serial art (Probing.cpp:5319): render a 128x32 chooser bitmap via `oled.displayBitmap()` — breadboard outline, digits 1–8 in two rows, i/o markers (bitmap authored as `0b`-literal rows so the shape reads in source). Update the OLED when a selection lands, and mirror `chooseGPIOinputOutput()` with `oled.clearPrintShow("GPIO n\nInput / Output", …)` per the `chooseIsense` precedent (Probing.cpp:4920). Probe mode already owns the panel (`OledGui::renderNow` bails on `probeActive`/`ContextType::PROBING`), so no new arbitration is needed.

## Decisions baked in (flag if you disagree)

- File named `RoutableGpio.h/.cpp` in `src/hardwarestuff/` (repo CamelCase; you wrote "Routable_Gpio").
- Counter is binary by default (your "10 bit" ask), with a per-range `bcdMode` toggle for true BCD nibbles so two 7447 digits on 8 pins still work; wrap not clamp.
- UART Tx/Rx included everywhere (settings app, carousel, BCD top bits), gated by the traffic-since-boot rule.
- `gpio_function_map[]` removed outright rather than kept as a cache.

## Verification

- Build both `env`s (V5 + OG) — the OG guard consolidation is the riskiest move.
- `bcdSelfCheck()` assert for the encode/wrap logic.
- Bench: `g` command table still correct; highlight a GPIO output → connect-tap toggles, turn opens carousel; menu GPIO → change direction then pull without leaving the menu; pad tap counts BCD.