# Pending-findings verification — 2026-08-26 (overnight polish pass)

The 53 findings RELEASE_AUDIT_2026-08-25.md left "plausible but unverified"
(the doc said 50; journal reconstruction found 53), each adversarially
verified against the current dev tree: two lenses (refute-biased +
reproduce-by-trace) for every high/medium, one refute lens for lows with
escalation on promotion. 40 confirmed, 9 refuted, 4 mixed. Machine-readable:
scratchpad pending_verdicts.json.

Risk legend: safe-tonight = fixable from code reading alone; needs-bench =
requires hardware iteration; BP = the fix preserves all observable behavior.


## #21 — CONFIRMED (2/2) [high] PWM API not OG-gated - jl_pwm_setup on OG drives the CH446Q chip-select, WS2812 data, and ADC p

- location: `src/Peripherals.cpp:3081` — changes behavior, safe-tonight
- fix shape: Add the existing initGPIO()/setGPIO() guard idiom -- `#if defined(OG_JUMPERLESS)\n    return -1; // the pins-20-27 bank is the V5 routable-GPIO map; on OG these are CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and ADC inputs (26-27). No PWM-able routable GPIO exists on OG until the Phase 2 map lands.\n#endif` -- as the first statement of the four functions that actually touch hardw
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTATION FAILED on every avenue; the defect is present in the current tree.

STALE? No. setupPWM is at src/Peripherals.cpp:3081 (claim's 3089 is mid-function), setupSlowPWM 2922, setSlowPWMDutyCycle 2987, setSlowPWMFrequency 3021, stopSlowPWM 3049, setPWMDutyCycle 3143, setPWMFrequency 3175, stopPWM 3207. None carries an OG_JUMPERLESS guard. The only validation in any of them is `gpio_pin < 1 || gpio_pin > 8`.

MAP IS V5-ONLY. src/Peripherals.h:167 defines `const int gpioDef[10][3]` hard-coding pins 20..27 with no #if. src/boards/og/board_og.cpp:75 enumerates the OG's routable GPIO as exactl

**Lens 2:** CONFIRMED against the current tree. Full trace, OG target (env jumperless_og, -DOG_JUMPERLESS):

ENTRY: `import jumperless; jumperless.pwm(1, 1000)`. The native module is compiled and registered on OG (platformio.ini [env:jumperless_og] `extends = env:jumperless_v5`, same build_src_filter incl. `+<../modules/jumperless/>`), and CodeDocs/OG_BACKPORT.md:244-283 records `import jumperless` verified working on real OG hardware over SWD+pyserial.

HOP 1 — modules/jumperless/modjumperless.c:6729 registers `MP_QSTR_pwm` -> jl_pwm_obj. jl_pwm_func validates gpio_pin 1-8 (c:2369), freq range (c:2373), 

</details>

## #16 — CONFIRMED (2/2) [medium] GPIO pin claims are never released after file-run scripts or soft reboot (only the REPL-exit pa

- location: `src/snakes/Python_Proper.cpp:5072 (file-run session end, in executePythonFileContent) and src/JumperlessMicroPythonAPI.cpp:1477 (jl_soft_reboot) — both missing the release; claimed :1509 is stale` — changes behavior, needs-bench
- fix shape: Two insertions, both mirroring the already-proven REPL-exit sequence at JumperlessMicroPythonAPI.cpp:1589-1600. (1) In `executePythonFileContent` (Python_Proper.cpp), NOT in `onPythonSessionEnd` — put it beside the `jl_reset_python_display_prefs()` call at ~:5077, which is already the documented file-run-only cleanup slot, so the raw-REPL path at MpRemoteService.cpp:400 is untouched. Order matters
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED by end-to-end trace against the current tree (HEAD 486eb28). The claimed line 1509 has drifted; the two missing-release sites are now Python_Proper.cpp:5072 and JumperlessMicroPythonAPI.cpp:1477.

CLAIM SIDE (both targets — platformio.ini:104 build_src_filter is inherited verbatim by env:jumperless_og at platformio.ini:258, and lib/micropython/port/ + modules/jumperless/ are built for both):
1. Script does `machine.Pin(20, Pin.OUT)` (or any `Pin(n)`, or `.init()`).
2. lib/micropython/port/machine_pin_jl.c:295 (no-mode ctor) or :230 (init helper) calls `jl_gpio_claim_pin(self->id)`.
3

**Lens 2:** Refutation failed on every avenue; the finding survives against the current tree.

Claim sites: machine.Pin is the only thing that sets the bits — lib/micropython/port/machine_pin_jl.c:230 (obj_init_helper) and :295 (bare constructor) both call jl_gpio_claim_pin(), which sets globalState.config.gpioPythonOwned[index]=true (src/JumperlessMicroPythonAPI.cpp:807-830). The jumperless-module gpio_* helpers do NOT claim, which narrows exposure but does not refute.

Release sites (exhaustive grep over src/, lib/, modules/): jl_gpio_release_all_pins is defined at src/JumperlessMicroPythonAPI.cpp:850 a

</details>

## #17 — CONFIRMED (2/2) [medium] The mphalport.c cooked-stdout stub wins the link over Python_Proper.cpp's real implementation —

- location: `lib/micropython/port/mphalport.c:245 (shadowing stub); shadowed real implementation at src/snakes/Python_Proper.cpp:486, UART capture block at src/snakes/Python_Proper.cpp:553` — changes behavior, needs-bench
- fix shape: One definition must survive, and it must contain the UART capture. Do NOT simply delete the mphalport.c stub and let Python_Proper.cpp:486 win — that swaps in 6 months of untested code at once (a `yield()` + `mp_hal_check_interrupt()` every 50 chars mid-output, a different locked-stream rule — `!= &USBSer2` vs mphalport.c:73's `!= global_mp_stream_ptr` — a per-char `jl_cdc_wait_writable(need=2)` a
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED, and the claimed line is not stale — the stub is still at lib/micropython/port/mphalport.c:245.

TWO STRONG DEFINITIONS, BOTH STILL PRESENT
- lib/micropython/port/mphalport.c:245 `void mp_hal_stdout_tx_strn_cooked(...)` → tail-calls `mp_hal_stdout_tx_strn` (mphalport.c:69) → `arduino_serial_write` (mphalport.c:78).
- src/snakes/Python_Proper.cpp:486 `extern "C" void mp_hal_stdout_tx_strn_cooked(...)` (file moved from src/ to src/snakes/ since the finding was written; the finding's "Python_Proper.cpp:486" is exact).
- platformio.ini:99 `-Wl,--allow-multiple-definition` silences the cl

**Lens 2:** REFUTATION FAILED. Every avenue was checked and none holds.

1) Both strong definitions still exist in the current tree:
   - `lib/micropython/port/mphalport.c:245` — `void mp_hal_stdout_tx_strn_cooked(const char*, size_t)` that just tail-calls `mp_hal_stdout_tx_strn()` (same file, :69).
   - `src/snakes/Python_Proper.cpp:486` — `extern "C" void mp_hal_stdout_tx_strn_cooked(...)`, the full version whose tail (Python_Proper.cpp:553-555) is the UART capture.
   `nm` on the two objects from today's build confirms both are `T` (strong): `.pio/build/jumperless_v5/lib/micropython/port/mphalport.c.o`

</details>

## #18 — CONFIRMED (2/2) [medium] Stale duplicate micropython_embed.c (without the jl_bg_reset_entry GC-root fix) is compiled in;

- location: `lib/micropython/port/micropython_embed.c:153` — behavior-preserving, safe-tonight
- fix shape: Sync the source-of-truth file up to the generated one rather than deleting it. In lib/micropython/port/micropython_embed.c add `extern void jl_bg_reset_entry(void);` to the extern block at :70 (next to the existing machine_pin_irq_init/deinit externs), add `jl_bg_reset_entry();` immediately after `machine_pin_irq_init();` at :153 in mp_embed_init, and add the same call after `machine_pin_irq_init(
<details><summary>verifier reasoning</summary>

**Lens 1:** I tried hard to refute this and could not — but I also found the finding's proposed fix is backwards, which changes what "fixing" it means.

WHAT SURVIVES REFUTATION:

1. The two files still differ by exactly the GC-root fix. `diff lib/micropython/port/micropython_embed.c lib/micropython/micropython_embed/port/micropython_embed.c` yields only three added lines in the latter: `extern void jl_bg_reset_entry(void);` (micropython_embed/port/micropython_embed.c:71), `jl_bg_reset_entry();` after `machine_pin_irq_init()` in `mp_embed_init` (:160), and the same in `nlr_jump_fail`'s recovery path (:421

**Lens 2:** REAL as a build defect; the runtime harm chain does NOT reproduce. Downgrading medium -> low.

WHAT IS CONFIRMED IN THE CURRENT TREE
1. Two tracked copies, diverged by exactly the GC-root fix. `diff lib/micropython/port/micropython_embed.c lib/micropython/micropython_embed/port/micropython_embed.c` = 3 hunks: the extern decl (micropython_embed/port/micropython_embed.c:71), the post-mp_init call (:160), and the nlr_jump_fail call (:421). The stale copy's mp_init site is lib/micropython/port/micropython_embed.c:149-153 -- `mp_init(); ... machine_pin_irq_init(); return 0;` with no reset. Claimed 

</details>

## #19 — CONFIRMED (2/2) [medium] USB audio mic streams pure silence - PCM ring consumer was deleted in T2.1 and never reinstated

- location: `src/tubes/USBAudio.cpp:125` — changes behavior, needs-bench
- fix shape: Reinstate the missing strong definition of `tud_audio_tx_done_isr` in src/tubes/USBAudio.cpp, in the "TinyUSB audio class callbacks" section (after tud_audio_set_req_entity_cb, before the closing `#endif // USB_AUDIO_ENABLE` at line 691). Body, adapted from the version deleted in 557203e (recoverable via `git show 557203e^:src/USBAudio.cpp`, lines 859-879): mark it `extern "C" bool __not_in_flash_
<details><summary>verifier reasoning</summary>

**Lens 1:** Every refutation avenue failed; the finding is confirmed, including at the binary level.

1. No consumer in source. Repo-wide, `tud_audio_write` appears only in a comment at src/tubes/USBAudio.cpp:117 and in CodeDocs/USB_AUDIO_HANDOFF.md:153. There is no `tud_audio_tx_done_isr`, `tx_done_pre_load_cb`, or `tx_done_post_load_cb` anywhere in src/, include/, or lib/. The consumer index `g_ringTail` (src/tubes/USBAudio.cpp:125) is only ever RESET to 0 (lines 263 in usbAudioStreamStart, 569 in usb_audio_set_dc_block) and never advanced. `g_statFramesSent` (line 132) is declared and read out in usb_a

**Lens 2:** CONFIRMED by full end-to-end trace. The PCM ring has a producer and no consumer.

TRACE (V5 only; OG is excluded at include/usb_interface_config.h:45-47, `#if defined(OG_JUMPERLESS) #define USB_AUDIO_ENABLE 0`).

1. User enables the mic: `usb_audio_set_device_enabled(true)` from the X-menu (src/SingleCharCommands.cpp:3868/3913), MicroPython (src/JumperlessMicroPythonAPI.cpp:385), or restored at boot (src/main.cpp:609 -> `usb_audio_boot_enumerate()` -> src/tubes/USBAudio.cpp:340). Sets g_usb_audio_enabled, re-enumerates with the UAC2 function appended (src/tubes/usb_descriptors.cpp:127, TUD_JL_

</details>

## #20 — CONFIRMED (2/2) [medium] initINA219 V5 path boot-wedges forever if either INA219 does not answer (OG path is bounded, V5

- location: `src/Peripherals.cpp:1831 (and the twin at src/Peripherals.cpp:1839)` — behavior-preserving, safe-tonight
- fix shape: Bound both V5 waits exactly as the OG path at src/Peripherals.cpp:1801-1804 already does. Replace `while (INA0.getConversionFlag() == false) { tight_loop_contents(); }` at src/Peripherals.cpp:1831 with `uint32_t start0 = millis(); while (INA0.getConversionFlag() == false && (millis() - start0 < 100)) { tight_loop_contents(); }`, and the same shape with its own `start1` for INA1 at src/Peripherals.
<details><summary>verifier reasoning</summary>

**Lens 1:** Reproduced end-to-end on the V5 target.

Entry: src/main.cpp:466 calls initINA219() from setup() on core0 (sole call site; src/main.cpp:421 is commented out, and grep over src/ finds no runtime re-init).

Branch: src/Peripherals.cpp:1785 gates on `#if defined(OG_JUMPERLESS)`; env jumperless_v5 compiles the `#else` at src/Peripherals.cpp:1808.

Defect: src/Peripherals.cpp:1809 `if (!INA0.begin() || !INA1.begin())` only prints "Failed to find INA219 chip" at src/Peripherals.cpp:1811 and falls through. Execution then reaches src/Peripherals.cpp:1831 `while (INA0.getConversionFlag() == false) { ti

**Lens 2:** CONFIRMED against the current tree; the claim survived every refutation avenue I could construct.

The code is still there, shifted ~12 lines: `src/Peripherals.cpp:1783` `initINA219()`. The OG branch bounds its wait at `src/Peripherals.cpp:1802` (`while ( INA0.getConversionFlag() == false && (millis() - start < 100) )`), while the `#else` (V5) branch has two unbounded waits at `src/Peripherals.cpp:1831` (INA0) and `src/Peripherals.cpp:1839` (INA1). `git log -L` on those lines shows the V5 loops are untouched legacy code (last cosmetic edit in 205b40c); the 100 ms bound was written later for th

</details>

## #22 — CONFIRMED (2/2) [medium] setGPIO drives output pins HIGH unasked - readGPIO stamps gpioState=4 on unrouted pins and setG

- location: `src/Peripherals.cpp:843` — changes behavior, safe-tonight
- fix shape: Two small edits, both needed. (1) In readGPIO, stop stamping the input-pulldown enum onto output pins: src/Peripherals.cpp:1318-1321 becomes `if ( gpioNet[i] == -1 ) { if ( gpioState[i] != 6 && globalState.config.gpioDirection[i] != 0 ) { gpioState[i] = 4; } }` — this preserves an output pin's commanded 0/1 across the unrouted window and also stops the silent output->input+pulldown config flip if 
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED on V5 (jumperless_v5); not reachable on OG.

Mechanism, verified end to end in the current tree:

1. `gpioState[]` is both the display enum AND setGPIO's output level. src/Probing.cpp:6592-6597 says so verbatim: "gpioState[] doubles as the display state AND setGPIO()'s output value, and several writers can zero it ... after which the next refreshConnections drives the pin low with the claim bridge still in place." A self-heal (`reassertGpioBufferDrive`, Probing.cpp:6598-6616) exists for the probe-power claim pin ONLY.

2. readGPIO (core 2, gated to one pass per 1000 us by readGPIOInt

**Lens 2:** CONFIRMED — every hop reproduces in the current tree. Claimed line :833 is stale; the put is at src/Peripherals.cpp:843.

TRACE (V5, env jumperless_v5):
1. Entry / precondition. User configures a routable GPIO as OUTPUT. Default is input (src/routing/States.cpp:515 `gpioDirection[i] = 1`), so this is an explicit user action — probe GPIO menu (src/Probing.cpp:5491-5495) or terminal/JSON (src/routing/JsonState.cpp:741). At boot, src/remembering/PersistentStuff.cpp:675-678 sets `gpioState[i] = 0` for output pins, so the 0xff-at-boot half of the claim is already closed. gpioState[i] == 0 (output L

</details>

## #23 — CONFIRMED (2/2) [medium] PWM slice sharing: GPIO pairs 1/2, 3/4, 5/6, 7/8 clobber each other's frequency and stopPWM kil

- location: `src/Peripherals.cpp:3119` — changes behavior, needs-bench
- fix shape: Two independent halves; the sibling index is simply `gpio_index ^ 1` (physical = 20 + gpio_index, and 20 is even, so XOR-1 pairs exactly the two channels of one slice for indices 0-7).

(a) stopPWM (src/Peripherals.cpp:3226) — the clean, low-risk half. Replace the unconditional `pwm_set_enabled(slice_num, false)` with: if `gpioPWMEnabled[gpio_index ^ 1]` is true, leave the slice running and just `
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED by end-to-end trace on the current tree (HEAD 486eb28). No audit commit touched setupPWM/stopPWM; the claimed line 3107 is stale, the defect now sits at src/Peripherals.cpp:3119-3120 and :3226.

HOP 1 — entry point is real and user-facing.
modules/jumperless/modjumperless.c:2352 `jl_pwm_func` accepts pin 1-8 (or node consts GPIO_1..GPIO_8 = 131-138, remapped at :2365) and calls `jl_pwm_setup` at :2381. src/JumperlessMicroPythonAPI.cpp:2739 forwards straight to `setupPWM`. `pwm` is registered in the shipped MicroPython surface (src/snakes/Python_Proper.cpp:4739, EkiloEditor.cpp:106 au

**Lens 2:** REFUTATION ATTEMPTED AND FAILED. The finding survives on the current dev tree (line numbers shifted ~by the audit commits, but the code is unchanged).

1) The hardware premise is confirmed on BOTH targets. src/Peripherals.h:167-178 maps user GPIO 1..8 to physical pins 20..27. The SDK macro at ~/.platformio/packages/framework-arduinopico/pico-sdk/src/rp2_common/hardware_pwm/include/hardware/pwm.h:95-103 is `slice = (gpio >> 1) & 7` for any gpio < 32 — identical on RP2040 and RP2350 in this range. So 20/21 -> slice 2, 22/23 -> slice 3, 24/25 -> slice 4, 26/27 -> slice 5, with channel = gpio & 1 

</details>

## #27 — CONFIRMED (2/2) [medium] cmd_printMicrosPerByte calls Serial1.begin(), seizing uart0 from the DMA passthrough bridge unt

- location: `src/tubes/ArduinoStuff.cpp:883` — behavior-preserving, safe-tonight
- fix shape: Delete ONLY the three-line `Serial1.begin(baudRateUSBSer1, makeSerialConfig(numbitsUSBSer1, paritytypeUSBSer1, stopbitsUSBSer1));` statement at src/tubes/ArduinoStuff.cpp:883-885. Keep everything else in the block: the `USBSer1.begin(...)` above it, the `microsPerByteSerial1` recompute, and the `serConfigChangedUSBSer1 = 0` latch reset. I am deliberately diverging from the finding's fix sketch, wh
<details><summary>verifier reasoning</summary>

**Lens 1:** Every refutation avenue failed; the finding survives, though its headline mechanism needs one correction.

CODE IS STILL PRESENT. `/Users/kevinsanto/Documents/GitHub/JumperlOS/src/tubes/ArduinoStuff.cpp:883-885` still contains `Serial1.begin(baudRateUSBSer1, makeSerialConfig(...))` inside `checkForConfigChangesUSBSer1()`. Unchanged since the Oct 2025 wholesale import (git log -L: c18f8d2); none of the four landed audit commits touched it.

REACHABILITY IS CONFIRMED, NOT ASSUMED.
- Only caller of `checkForConfigChangesUSBSer1` in the tree is `printMicrosPerByte()` at ArduinoStuff.cpp:495 (grep:

**Lens 2:** CONFIRMED — every hop reproduces in the current tree, and the claimed line number is still accurate (git log -L 875,890 shows the block untouched since the initial import c18f8d2; none of the four audit commits touched it).

ENTRY POINT (ungated)
- src/SingleCharCommands.cpp:720-722 registers '_' -> cmd_printMicrosPerByte. It is the ONLY registration for '_' now (the shadowing bug called out at SingleCharCommands.cpp:116 and :586 was fixed, so '_' really reaches this callback).
- src/SingleCharCommands.cpp:164-172 executeCommand() does no menu-level or category filtering — it just does getComm

</details>

## #28 — CONFIRMED (2/2) [medium] usb_cdc.ignore_dtr=1 turns any unread CDC port into an infinite-spin core-0 wedge

- location: `src/tubes/usb_descriptors.cpp:612` — changes behavior, needs-bench
- fix shape: Bound the fake connectivity inside the single choke point instead of touching 7000+ print sites. In `__wrap_tud_cdc_n_connected` (src/tubes/usb_descriptors.cpp:608): bounds-check `itf < CFG_TUD_CDC`; if `!g_usb_ignore_dtr` keep `__real_...` exactly as today; else, if `!tud_ready()` return false; if `__real_tud_cdc_n_connected(itf)` is true, clear that instance's stall timestamp and return true (re
<details><summary>verifier reasoning</summary>

**Lens 1:** I set out to refute this and could not; every avenue closed against me.

CHAIN, verified in the current tree:

1. The wrap is unchanged and unguarded. src/tubes/usb_descriptors.cpp:607-618: `__wrap_tud_cdc_n_connected(uint8_t itf)` returns `tud_ready()` whenever `g_usb_ignore_dtr` is set — it ignores `itf` entirely and never consults the real DTR. `tud_ready()` is mounted && !suspended, which stays true for an enumerated device whose host-side tty is closed. The claimed line 612 is exactly the `if (g_usb_ignore_dtr) {`.

2. The wrap is link-wide on BOTH targets. platformio.ini:78 `-Wl,--wrap,t

**Lens 2:** CONFIRMED against the current tree. Every hop of the chain traces cleanly; none of the landed audit batches touched this wrap.

Hop 1 — the flag can be set and persists. `bool ignore_dtr = false;` (src/config.h:211) is the default, but it is a documented, persisted config key: parsed at src/configManager.cpp:814, written back at :1193, in the change-detector at :1412, and shipped as `ignore_dtr = 0;` in CodeDocs/configExample.txt:131. The shipped MicroPython Excel example actively instructs the user to turn it on: scripts/ex/excel_listener.py:311 / src/snakes/micropythonExamples.h:556 print "U

</details>

## #29 — CONFIRMED (2/2) [medium] Ser3 ':every' buffered gpio capture busy-waits interval x count with no abort and no upper clam

- location: `src/tubes/Ser3Backchannel.cpp:900` — changes behavior, safe-tonight
- fix shape: Do NOT take the finding's own fix sketch verbatim — polling `USBSer3.available()` inside the `__not_in_flash_func` wait loop at line 904 defeats the entire reason that loop exists: `tud_cdc_n_available` is a flash-resident call behind the `__usb_mutex`, so it injects XIP-miss-sized jitter into the one path whose contract is exact micros() spacing. Put the guard OUTSIDE the timed loop instead. Mini
<details><summary>verifier reasoning</summary>

**Lens 1:** CODE IS UNCHANGED AND THE LINE NUMBER IS EXACT. `git log -- src/tubes/Ser3Backchannel.cpp` shows the newest commit touching it is 071a3f0, an ancestor of the audit batch 57cf63d; `git diff HEAD` on the file is empty. `usbSer3_captureGpioLoop` is still at src/tubes/Ser3Backchannel.cpp:900.

THE MECHANISM, RE-TRACED IN THE CURRENT TREE:
- Ser3Backchannel.cpp:885-895 `usbSer3_parseInterval` — bare number multiplies by 1000 (unit = ms), "s" suffix by 1e6, and there is NO upper clamp, only `us < 1 ? 1 : us`.
- Ser3Backchannel.cpp:983-984 `usbSer3_runEvery` — `if (intervalUs < minUs) intervalUs = mi

**Lens 2:** CONFIRMED by full trace on the current tree (audit commits 57cf63d/47325d3/dec1b45/4e5b0d0 did not touch this file — `git log -- src/tubes/Ser3Backchannel.cpp` last lands at 071a3f0).

Chain, hop by hop:
1. Core 0 loop → src/main.cpp:1122 `Jerial.service()` → src/Jerial.cpp:1103 `singleCharCommands.serviceUSBSer3()` → src/tubes/Ser3Backchannel.cpp:1072.
2. src/tubes/Ser3Backchannel.cpp:1112 accepts `:`, reads a line (1114), and at :1134 `low.startsWith("every:")` → `usbSer3_runEvery(line.substring(6))` at :1136. No access gate: the SER3_ALLOWED/`getBackchannelAccess` check at :1169 is only on 

</details>

## #31 — CONFIRMED (2/2) [medium] StepViewer eats wheel detents it cannot use - wheel goes dead when another screen holds the idl

- location: `src/eyecandy/StepViewer.cpp:285` — changes behavior, safe-tonight
- fix shape: Reorder the two statements in StepViewer::service() so the eligibility test runs before the ack. Compute the yielded-and-ineligible case first and bail without touching the encoder: replace lines 283-286 with `if (encoderDirectionState == UP || encoderDirectionState == DOWN) { if (!showing && gui.idleScreen() != &viewerScreen) return ServiceStatus::IDLE; bool up = (encoderDirectionState == UP); en
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED against the current dev tree; the claimed line numbers are exact.

Code as it stands, src/eyecandy/StepViewer.cpp:283-299: the detent is acked (`encoderDirectionState = NONE;` at :285) before the reclaim-eligibility test (`if (gui.idleScreen() != &viewerScreen) return ServiceStatus::IDLE;` at :295). git blame shows :285 is original (ee21472) and :290-295 was inserted by the later sweep commit 11b5a20 — the guard landed on the wrong side of the consume, so its own comment ("the turn falls through to Highlighting instead") describes behavior the code does not produce.

Refutation avenu

**Lens 2:** CONFIRMED against the current tree; line 285 is exact, not stale (last touch of the file was 11b5a20, before the four audit commits, which did not modify this path).

Trace, V5 (jumperless_v5):

1. Arm. ProjectsApp.cpp:1006 calls `stepViewer.arm(guideSource, cursor)` on project open (or `z steps on` via SingleCharCommands.cpp:869). StepViewer.cpp:217 does `OledGui::getInstance().activate(&viewerScreen, /*persist=*/true)` -> OledGui.cpp:569 sets `idleScreen_ = &viewerScreen`, :562 `active_ = &viewerScreen`, :564 `ownsPanel_ = true`. `active = true` (StepViewer.cpp:213).

2. Idle slot is taken a

</details>

## #36 — CONFIRMED (2/2) [medium] teardownOldOledBus skips Wire1.end() when leaving type 2, but i2cScan leaves Wire1 running - la

- location: `src/oled.cpp:4524 (teardownOldOledBus; call site 4546) — with the state-leaking half at src/Apps.cpp:1931-1945` — behavior-preserving, safe-tonight
- fix shape: Minimal, local fix: give teardownOldOledBus the destination type and end Wire1 whenever the new type will reconfigure it. `static void teardownOldOledBus(int oldConnectionType, int newConnectionType) { if (oldConnectionType != 2 || newConnectionType != 2) { Wire1.end(); delay(50); } }`, and change the call at oled.cpp:4546 to `teardownOldOledBus(oldConnType, newConnectionType)` (newConnectionType 
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED in the current tree; the audit commits did not touch either half.

Leftover state: src/Apps.cpp:1931-1945 (the external-scan tail) does `WireScan->end()`, then gates the pin restore on `jumperlessConfig.top_oled.enabled && connection_type != 2`, then calls `WireScan->begin()` UNCONDITIONALLY. `WireScan` is `&Wire1` for every external scan (Apps.cpp:1845-1848). On a rev-7 board with connection_type == 2 the restore branch is skipped, so Wire1 is left `_running` on the scan pins. Every external entry point passes 26/27: SingleCharCommands.cpp:2810, :2843, :2874, the Apps.h:22 defaults 

**Lens 2:** CONFIRMED by full trace against the current tree. The claim's line number is stale (4519 lands in the comment block); the live gate is src/oled.cpp:4525.

Hop-by-hop, V5 rev-7 board with the shipping default connection_type == 2 (src/configManager.cpp:428-437 = internal I2C0, GPIO 4/5; src/oled.cpp:4503-4507 makes 2 the rev>=7 default):

1. User runs an external I2C scan — App menu "I2C    Scan" (src/Apps.cpp:81 -> :72 -> i2cScan() with header defaults sdaRow=-1, sclRow=-1, sdaPin=26, sclPin=27, leaveConnections=0, internalScan=0, src/Apps.h:22-24), or the 'i' command paths (src/SingleCharComm

</details>

## #38 — CONFIRMED (2/2) [medium] Runtime OLED reinit on shared I2C0 unguarded against core-1 wavegen streaming (cross-core bus c

- location: `src/oled.cpp:4566` — changes behavior, needs-bench
- fix shape: Add the same shared-bus policy the frame path already has, at the reinit entry points. Minimal version: at the top of applyOledConnectionType() (src/oled.cpp:4532) and oled::init() (src/oled.cpp:551), compute the TARGET bus (connection_type == 2 -> Wire) and, if it is Wire and wavegen.isRunning(), either (a) refuse with a one-line message ("stop the waveform before switching the OLED bus") and ret
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED on V5, with a corrected mechanism (the finding's "no cross-core bus lock exists" is now only half true — an I2C0 arbiter landed, but it does not cover the calls this path makes).

Full trace, current tree:

1. Wave starts (core 0, terminal/menu/REPL): WaveGen::start() sets _running=true (src/hardwarestuff/WaveGen.cpp:284) then _dmaStart() (WaveGen.cpp:872, 900-931) arms chA/chB1/chB2 + a DMA pacing timer that pushes MCP4728 frames straight into i2c0_hw->data_cmd. Core 1's loop1() only monitors it (src/main.cpp:1605-1607); the bus traffic is autonomous hardware, so it keeps running no

**Lens 2:** VERDICT: real, but the finding's stated mechanism is stale for the shipping V5 path, and the residual harm is a wave glitch, not corruption/wedge. Downgrade medium -> low.

WHAT IS STILL TRUE (the gap is real):
- src/oled.cpp:551 `oled::init()` has no wavegen gate. It calls initDisplayForConnectionType (oled.cpp:119), connect() (oled.cpp:4258) -> initI2C (Peripherals.cpp:619), checkConnection() (oled.cpp:740) -> oledI2cPing (oled.cpp:94), and getDisplay().begin(...) — all of which touch I2C0 when connection_type == 2 (the rev-7 default, oled.cpp:4503-4507).
- src/oled.cpp:4532 `applyOledConnec

</details>

## #43 — CONFIRMED (2/2) [medium] new (std::nothrow) aborts on OOM in both shipped images - every C++ allocation fallback in the 

- location: `src/eyecandy/BitmapEditor.cpp:181` — changes behavior, needs-bench
- fix shape: Do NOT use the finding's sketch as written — defining a malloc-backed plain `operator new` would convert STL OOM (std::vector/String growth, the ~789 unchecked internal allocations) from today's diagnosable abort+crashlog into a silent nullptr deref, and it forces the risky co-requirement of auditing every unchecked `new` in the firmware in one commit. Use the narrow variant instead: add a ~20-lin
<details><summary>verifier reasoning</summary>

**Lens 1:** Every refutation avenue failed; the finding survives against the current tree.

STALE-BUILD REFUTATION FAILS. Both ELFs are provably current: `.pio/build/jumperless_v5/firmware.elf` and `.pio/build/jumperless_og/firmware.elf` are both dated Aug 26 00:56, newer than HEAD 486eb28 (2026-08-25 21:59) and newer than every `.cpp` under `src/` (`find src -newer ...firmware.elf -name '*.cpp'` returns nothing). The finding's addresses (_Znwj @ 0x100fc458) are from an older build; mine are 0x100fc938 — a later build, same result.

DISASSEMBLY CONFIRMS BOTH TARGETS. V5 (`operator new(unsigned int)` @ 0x1

**Lens 2:** CONFIRMED on both shipped images, by disassembling the current ELFs (rebuilt since the finding was written — addresses moved, the property did not).

Trace, hop by hop:

1. Entry: file browser -> `FileManager` opens an image file. `src/remembering/FilesystemStuff.cpp:1344` and `:1402` both call `launchBitmapEditor(path)`; that reaches `BitmapEditor::loadFile()` at `src/eyecandy/BitmapEditor.cpp:64`. Line number in the claim is still exact.

2. Size gate: header path caps at width/height <= 256 -> `expectedSize` <= 8192 (`BitmapEditor.cpp:126-137`). Raw path rejects `fileSize > 32768` (`BitmapE

</details>

## #45 — CONFIRMED (2/2) [medium] Stale REQ_BYPASS/REQ_SEND serviced mid-rebuild momentarily disconnects the entire crossbar

- location: `src/Commands.cpp:367` — changes behavior, needs-bench
- fix shape: Minimal fix, two lines. Extend the pre-clear wait in refreshLocalConnections to the same condition waitCore2() already uses, so a posted-but-unserved bypass is drained before the path arrays are zeroed: change `while (core2busy)` at src/Commands.cpp:367 to `while (core2busy || !core1req::allIdle())`, keeping the existing 200ms timeout-and-proceed body untouched. Apply the identical change to the t
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED against the current tree. The claimed line is exact and unchanged by the landed audit commits.

Full trace, hop by hop:

1. Entry. `jl_nodes_connect()` (MicroPython `connect()`) at src/JumperlessMicroPythonAPI.cpp:1333 calls `addBridgeToState(node1,node2,duplicates,true)`. Probe strokes take the same path (src/Probing.cpp:3711 comment: "refreshLocalConnections() is already called inside addBridgeToState()"), as do the batched app setups (src/Apps.cpp:1341-1348, four back-to-back calls).

2. `addBridgeToState()` at src/remembering/FileParsing.cpp:230; with `autoRefresh` true it calls 

**Lens 2:** I tried to refute this five different ways and every avenue closed.

**1. Is the guard sufficient?** No. `refreshLocalConnections` (src/Commands.cpp:344) has exactly one pre-clear guard: `while (core2busy)` at src/Commands.cpp:367 (200 ms timeout-and-proceed at :377). `core2busy` is raised only immediately *around* an actual send — main.cpp:1550/1553, main.cpp:1706/1709, and inside `sendPaths` (CH446Q.cpp:709/754). A REQ_BYPASS that has been *posted but not yet taken* leaves `core2busy == false` by definition, so the guard is structurally blind to precisely the hazard. The very next statements

</details>

## #49 — CONFIRMED (2/2) [medium] eepromReconcileAfterConfig re-applies raw EEPROM calibration AFTER loadConfig's sanitizers, per

- location: `src/remembering/PersistentStuff.cpp:66` — behavior-preserving, safe-tonight
- fix shape: In applyStoreToConfig() (src/remembering/PersistentStuff.cpp:66), replace the raw copy with the identical sanitizer configManager.cpp:2741-2743 applies: assign g_store.measure_mode_output_voltage, then `if (jumperlessConfig.calibration.measure_mode_output_voltage < 3.0f || > 3.6f) = 3.33f;`. Three lines, one file, no call-order change. Nice property: no EEPROM write is needed in the fix -- the nex
<details><summary>verifier reasoning</summary>

**Lens 1:** MECHANISM CONFIRMED IN CURRENT TREE. src/main.cpp:303 calls loadConfig(), which clamps the field at src/configManager.cpp:2741-2743 ("< 3.0f || > 3.6f -> 3.33f", comment citing the hardware-observed 5.2V servo save). src/main.cpp:311 then calls eepromReconcileAfterConfig(), which at src/remembering/PersistentStuff.cpp:147-150 runs applyStoreToConfig() whenever g_storeValid, and src/remembering/PersistentStuff.cpp:66 copies g_store.measure_mode_output_voltage into jumperlessConfig with no sanitizer. Neither call is behind an OG ifdef, so both targets run it. Every saveConfig() then mirrors the 

**Lens 2:** CONFIRMED by trace against the current tree. The git archaeology is the clincher: the store-wins reconcile (applyStoreToConfig / eepromReconcileAfterConfig, blame c74aef2e, 2026-06-01) PREDATES the clamp Kevin added to defend against the hardware-observed 5.2V corruption (blame 29e7483a, 2026-08-13). So the loadConfig clamp was born defeated — it has never once repaired a unit that has a valid EEPROM store, which is every unit flashed since June.

Chain, hop by hop:
1. Ingress. src/selfreflection/SelfTest.cpp:940 `set = constrain(set, 2.5f, 5.2f)` — the tip servo is explicitly allowed up to 5.

</details>

## #1 — CONFIRMED (1/1) [low] Behavior-stating docs contradicted by today's code: 'VCC/GND never routed', 'one bus transactio

- location: `src/displays/DisplayTypes.h:15` — behavior-preserving, safe-tonight
- fix shape: Comments and markdown only — zero code changes. The code is the truth here and must not move: reverting PartLabels periodUs to 20 ms would re-break tap-to-inspect (PartLabels.h:41-46 documents exactly that failure), and unwinding the burst loop would re-introduce the tearing 8d8feca fixed. So the fix is a one-pass doc correction at eight sites: DisplayTypes.h:15-17 and DisplayService.h:14-15 — rep
<details><summary>verifier reasoning</summary>

**Lens 1:** Refutation failed on every avenue; all four claimed contradictions are still present in the current tree (dev @ 486eb28), and I found three more sites the finding missed.

(1) "VCC/GND are NEVER routed" — src/displays/DisplayTypes.h:15-17 still reads "VCC/GND pins are NEVER routed: the user wires power", and src/displays/DisplayService.h:14-15 still reads "VCC/GND never touched - the user wires power". I tried to refute this by hoping display parts skip the power-route path. They do not: src/PartsApp.cpp:381-414 (partsCommitPlacement) auto-bridges pinClass==1 && role==PARTDB_ROLE_VCC to TOP_RA

</details>

## #2 — CONFIRMED (1/1) [low] assignTermColor() with default startIndex=0 reads railTermColors[-1] and stamps nets[0].termCol

- location: `src/routing/NetManager.cpp:1129 (function assignTermColor at :1124; default arg at src/routing/NetManager.h:116)` — behavior-preserving, safe-tonight
- fix shape: One-line clamp on the rail loop only, exactly as the finding sketches: change `for (int i = startIndex; i < 6; i++)` at NetManager.cpp:1128 to `for (int i = (startIndex < 1 ? 1 : startIndex); i < 6; i++)`, restoring the pre-2ca2215 invariant that this loop never indexes railTermColors below 0. Leave the second loop at :1138 alone — starting it at 6 again would change the rails' printed colors, whi
<details><summary>verifier reasoning</summary>

**Lens 1:** The OOB read is REAL but every downstream claim in the finding is REFUTED. Bench harm is zero.

WHAT IS TRUE (src/routing/NetManager.cpp:1124-1136, current tree, unchanged by the audit commits):
  void assignTermColor(int startIndex) {           // 1124
    for (int i = startIndex; i < 6; i++) {         // 1128
      globalState.connections.nets[i].termColor = railTermColors[i - 1];   // 1129
`int railTermColors[5]` is at NetManager.cpp:1121; NetManager.h:116 declares `assignTermColor(int startIndex = 0)`. All eight call sites pass nothing (Commands.cpp:306/465/515, CH446Q.cpp:1047/1190/1329, 

</details>

## #3 — CONFIRMED (1/1) [low] netNameConstants[MAX_NETS] initialized with only 50 entries - nets 50-59 get nullptr names

- location: `src/routing/MatrixState.cpp:30` — behavior-preserving, safe-tonight
- fix shape: Extend the initializer on src/routing/MatrixState.cpp:30 by ten literals — (char*)"Net 50" through (char*)"Net 59" — moving them from the commented-out tail into the live brace so the array is fully populated to MAX_NETS. Cost is 10 extra pointers (40 bytes of .data) plus ~80 bytes of .rodata, negligible even on the RAM-tight OG build. That single edit is sufficient: because every consumer already
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED as a defect, but the finding's stated consequence ("null-deref crash") is refuted — every current consumer is guarded, so the real harm is cosmetic/API-nullness only.

Fact check of the array itself: src/routing/MatrixState.cpp:30 declares `char* netNameConstants[MAX_NETS]`. I counted the initializer programmatically (regex over the literal): exactly 50 entries, "Net 0".."Net 49"; the remainder is commented out after the closing brace. src/JumperlessDefines.h:182 sets `#define MAX_NETS 60` for BOTH targets (no per-board override — the OG block at :190-196 only changes MAX_BRIDGES/MAX

</details>

## #4 — CONFIRMED (1/1) [low] deserializeBridges attaches a color: to the wrong bridge when the YAML line duplicates an exist

- location: `src/routing/States.cpp:1873` — behavior-preserving, safe-tonight
- fix shape: Do NOT use the finding's own fix sketch verbatim ("only write when numBridges grew") — that regresses the adjacent-duplicate case, where today the color correctly lands on the just-referenced bridge and the sketch would drop it entirely. Instead, resolve the index by searching for the pair that addConnection actually touched. Replace the body of the `if (success && color != 0xFFFFFFFF)` block at S
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTATION ATTEMPTED, FAILED. The claimed code is unchanged by the audit commits.

CODE AS IT STANDS TODAY
- `src/routing/States.cpp:1867-1877` (deserializeBridges): `bool success = addConnection(n1, n2, errorMsg, dup);` then `if (success && color != 0xFFFFFFFF) { int bridgeIdx = connections.numBridges - 1; ... connections.bridgeColors[bridgeIdx] = color; }`. The comment on 1872 literally asserts "The bridge was just added, so it's at index (numBridges - 1)".
- `src/routing/States.cpp:609-624` (addConnection): the dup scan `return true`s at 623 WITHOUT touching numBridges. So `success == true`

</details>

## #5 — CONFIRMED (1/1) [low] checkDoNotIntersectsByNet off-by-one: loops run i <= MAX_DNI over doNotIntersectNodes[8] and j 

- location: `src/routing/NetManager.cpp:1019, :1024, :1056, :1061 (function checkDoNotIntersectsByNet begins at :1014)` — behavior-preserving, safe-tonight
- fix shape: Change the four loop bounds in checkDoNotIntersectsByNet from `<=` to `<`: `for (int i = 0; i < MAX_DNI; i++)` at src/routing/NetManager.cpp:1019 and :1056, and `for (int j = 0; j < MAX_NODES; j++)` at :1024 and :1061. That is the whole fix — four characters, matching the `<` already used by checkDoNotIntersectsByNode (:1096), checkIfBridgeExistsLocal (:634/:645), combineNets (:526/:527/:576/:585/
<details><summary>verifier reasoning</summary>

**Lens 1:** The four `<=` bounds are literally present in the current tree, but the finding's harm scenario is refuted.

CODE AS IT STANDS: `checkDoNotIntersectsByNet` now lives at src/routing/NetManager.cpp:1014 (claimed :1000 is stale). Bounds: `for (int i = 0; i <= MAX_DNI; i++)` at :1019 and :1056 over `int16_t doNotIntersectNodes[8]` (MatrixState.h:25, MAX_DNI=8, JumperlessDefines.h:197); `for (int j = 0; j <= MAX_NODES; j++)` at :1024 and :1061 over `int16_t nodes[MAX_NODES]` (MatrixState.h:17). Same on both targets — NetManager.cpp has no OG/V5 ifdefs; MAX_NODES is 40 (V5) / 24 (OG), MAX_DNI is 8 o

</details>

## #7 — CONFIRMED (1/1) [low] probe_revision <= 3: button decode contradiction between Probing.cpp's internal convention and 

- location: `src/JumperlessMicroPythonAPI.cpp:2661 (also :2708 and src/PartsApp.cpp:30)` — changes behavior, safe-tonight
- fix shape: Make the raw-to-user-facing flip unconditional at all three sanctioned sites instead of gating it on probe_revision > 3. In jl_probe_button_nonblocking (src/JumperlessMicroPythonAPI.cpp:2661), jl_probe_button_blocking (:2708) and partsProbeButton (src/PartsApp.cpp:30), drop the "if (jumperlessConfig.hardware.probe_revision > 3)" wrapper and keep the body: 1 -> 2, 2 -> 1, 0 unchanged (and the -999 
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED, and the claimed line is not stale (src/JumperlessMicroPythonAPI.cpp:2661 still holds the swap).

Chain of evidence in the current tree:

1. The decoders NORMALIZE raw across revisions. src/Probing.cpp:876-878 (PIO IRQ handler) and src/Probing.cpp:1388-1392 (checkProbeButtonHardware CPU path) map all-high to 2 on rev>=4 and 1 on rev<=3, and all-low to the reverse. The upstream V5 firmware confirms the intent: JumperlessV5/RP23V50firmware/src/Probing.cpp:2850 comments the all-high branch "// disconnect Button" and the all-low branch "// connect Button" (that is the pre-swap rev<=3 phy

</details>

## #9 — CONFIRMED (1/1) [low] checkPads indexes probeRowMap with unclamped map() extrapolation (negative index OOB read)

- location: `src/Probing.cpp:7120` — behavior-preserving, safe-tonight
- fix shape: Hoist the map() result into a local and apply the exact guard the two sibling decode sites already use, then bail the same way the mapMin guard above does. In checkPads, replace `probeReading = probeRowMap[ map( probeReading, mapMin, mapMax, 101, 0 ) ];` with: `int padIdx = map( probeReading, mapMin, mapMax, 101, 0 ); if ( padIdx <= 0 || padIdx >= (int)( sizeof( probeRowMap ) / sizeof( probeRowMap
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED, but the harm ceiling is much lower than "OOB read" sounds — the read lands inside the same singleton object for every realistic calibration.

Current code (line numbers refreshed; claim said 7111, it is now 7120):
- `src/Probing.cpp:7120` — `probeReading = probeRowMap[ map( probeReading, mapMin, mapMax, 101, 0 ) ];` with the only guard above it being `src/Probing.cpp:7111` `if ( probeReading <= mapMin ) { checkingPads = 0; return; }`. There is no upper guard. Confirmed unfixed by the four audit commits.
- `map()` is the stock Arduino non-clamping one: I grepped `src/` and `lib/` for

</details>

## #13 — CONFIRMED (1/1) [low] L/S paste reader accumulates unbounded String on a 171KB no-PSRAM heap

- location: `src/SingleCharCommands.cpp:1868` — behavior-preserving, safe-tonight
- fix shape: Two-part, both inside readPastedBlock (src/SingleCharCommands.cpp:1842-1916). (a) Stop discarding the allocator's answer: add a `bool overflow = false;` to the lambda's capture and replace each bare `out += X` at :1858, :1865, :1868, :1869 and :1911-1913 with `if (!out.concat(X)) overflow = true;`. (b) Add an explicit ceiling matching the constant the file-read twin already uses — `static const un
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTATION ATTEMPTS, ALL FAILED:

1) "Already fixed by the landed audit commits." No. `git log -S readPastedBlock -- src/SingleCharCommands.cpp` returns only f5a6cd0 (the blank-line fix that created it). The function still lives at src/SingleCharCommands.cpp:1842-1916 and its three accumulation sites — `out += lineBuf` / `out += "\n"` at :1858, :1865, :1868-1869 and the untermintaed-tail flush at :1911-1913 — carry no length test of any kind. The claimed line 1862 is inside `completeLine`; the real anchor is :1868.

2) "There's an upstream cap in the CDC/Jerial layer." No. readPastedBlock bypa

</details>

## #14 — CONFIRMED (1/1) [low] DMX app allows address 0, overwriting the DMX start code

- location: `src/Apps.cpp:3593` — behavior-preserving, safe-tonight
- fix shape: Two-line change in the 'a'/'A' case at src/Apps.cpp:3593. Change the guard `if ( config.dmx_address >= 1 )` to `if ( config.dmx_address > 1 )` so the floor becomes address 1, and fix the now-wrong else message at src/Apps.cpp:3598 from "Already at minimum address (0)" to "(1)" — the guard change alone leaves the message stating a floor the code no longer has. Nothing else needs to move: init is al
<details><summary>verifier reasoning</summary>

**Lens 1:** Every refutation avenue fails; the finding survives.

1. The code is present and unchanged by the audit commits. `src/Apps.cpp:3591-3599` (case 'a'/'A'): `if ( config.dmx_address >= 1 ) { config.dmx_address--; ... } else { Serial.println("... Already at minimum address (0)"); }`. `git log -L 3585,3620:src/Apps.cpp` shows the block last touched in a1b2f7d, long before 57cf63d/47325d3/dec1b45/4e5b0d0 — the audit commits did not go near it. Claimed line 3590 is only ~3 lines stale.

2. The clobber is real. `updateDMXUniverse()` (src/Apps.cpp:3012) writes `universe[0] = 0x00` FIRST, then `universe

</details>

## #24 — CONFIRMED (1/1) [low] Bus-keeper (and I2C-state) GPIO readings stick at 'low' forever - gpioReadWithFloating leaves t

- location: `src/Peripherals.cpp:979` — behavior-preserving, safe-tonight
- fix shape: Fix at the consuming site rather than at gpioReadWithFloating's exit, so the RP2350-E9 discharge dance is left completely untouched. In readGPIO (src/Peripherals.cpp), add gpio_set_input_enabled(gpioDef[i][0], true) immediately before the bare gpio_get() in the two branches that can be entered with a stale-disabled buffer: the gpioState==7 bus-keeper branch (:1364-1368) and the gpioReadFloating==0
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED, V5-only, with a narrower trigger window than the finding implies but a broader consumer set.

Mechanism, current tree:
- src/Peripherals.cpp:848 gpioReadWithFloating(). Main path ends with the pad input buffer DISABLED: gpio_set_input_enabled(pin,false) at :960 (reading-high branch) and :979 (reading-low branch), then :988 gpio_set_pulls, :996 unlock, :998 return. No exit path re-enables IE. The only IE-on exits are the early returns at :913 (pullup pin already pulled low) and :932 (pulldown pin still high).
- The common bench case lands on the IE-off exit: the default pull config i

</details>

## #25 — CONFIRMED (1/1) [low] Legacy WaveGen fallback streams Wire from core 1 with no cross-core lock - I2C0 arbiter only co

- location: `src/hardwarestuff/WaveGen.cpp:369` — behavior-preserving, needs-bench
- fix shape: Extend the arbiter to cover the legacy loop instead of only the DMA stream. Add two volatile members, _legacyPauseReq and _legacyParked. In WaveGen::busPause() (src/hardwarestuff/WaveGen.cpp:784), after the existing _dmaStreaming check fails, add a second branch: if (_running && !_dmaAvailable) set _legacyPauseReq = true, spin bounded (about 2 ms, __sev/tight_loop_contents) until _legacyParked goe
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED for V5's DMA-fallback path only; REFUTED for OG.

The mechanism is intact in the current tree. WaveGen::service() (src/hardwarestuff/WaveGen.cpp:333-342) dispatches to _serviceLegacy() whenever !_dmaAvailable, and service() is only ever called from loop1() — core 1 — at src/main.cpp:1607 (loop1 defined src/main.cpp:1538; core-0 loop() at src/main.cpp:795). The legacy loop at src/hardwarestuff/WaveGen.cpp:369-403 calls _dac.setChannelValue(), which is _wire->beginTransmission / write / endTransmission on the shared global Wire (src/hardwarestuff/MCP4728.cpp:454-456).

The arbiter genu

</details>

## #26 — CONFIRMED (1/1) [low] failedToSetDac0 counter reset unconditionally - persistent DAC0 write failure is fully silent

- location: `src/Peripherals.cpp:1588` — behavior-preserving, safe-tonight
- fix shape: Do NOT apply the finding's sketch as written (see caveat in reasoning — it spams OG). Minimal correct fix, two coupled edits inside setDac0voltage: (1) delete the unconditional `failedToSetDac0 = 0;` at line 1588 and put the reset in the success path instead — i.e. add an `else { failedToSetDac0 = 0; }` to the `setChannelValueCached(...) == false` check at 1577-1587; (2) gate the failure accountin
<details><summary>verifier reasoning</summary>

**Lens 1:** MECHANISM CONFIRMED (line drift: it lives at 1588, not 1576 — 1576 is now a commented-out `// delay(10);`).

src/Peripherals.cpp:1555 `int failedToSetDac0 = 0;`
src/Peripherals.cpp:1577-1587 — if `mcp.setChannelValueCached(MCP4728_CHANNEL_A, ...) == false`: `failedToSetDac0++`, then `if (failedToSetDac0 > 10) { Serial.println("Failed to set DAC0 value"); failedToSetDac0 = 0; }`
src/Peripherals.cpp:1588 — `failedToSetDac0 = 0;` OUTSIDE the failure branch.

There is no early return between function entry and 1588, and no other writer to the symbol anywhere in the tree (grep: only 1555/1582/1585/

</details>

## #30 — CONFIRMED (1/1) [low] CDC1 line-coding callback reconfigures the UART synchronously in USB IRQ context, racing the th

- location: `src/tubes/AsyncPassthrough.cpp:1551` — behavior-preserving, needs-bench
- fix shape: Delete the `AsyncPassthrough::processPendingLineCoding();` call at AsyncPassthrough.cpp:1551, leaving the callback as pure state capture: `s_line_coding = *p; s_apply_line_coding_pending = true;` — exactly what its own comment at :1544 already promises. task() already drains the flag in thread context at :2019 on the very next pass, ahead of `bridge_usb_to_uart()` in the same pass, and `syncUartTo
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTE lens failed on every avenue; the finding stands as written, and two extra harms make it slightly stronger than claimed.

1) Not stale / not fixed. `tud_cdc_line_coding_cb` is at /Users/kevinsanto/Documents/GitHub/JumperlOS/src/tubes/AsyncPassthrough.cpp:1545 and still ends with a bare `AsyncPassthrough::processPendingLineCoding();` at :1551, directly under its own comment "Cache new line coding and apply it in main context" (:1544). `git log -S` shows the call predates the folder move (e795630, 2026-08-18) — old code, untouched by the audit commits.

2) It really is IRQ context. arduino

</details>

## #33 — CONFIRMED (1/1) [low] Rail/DAC adjuster's post-adjust re-highlight is a no-op: clearHighlighting() zeroes highlighted

- location: `src/eyecandy/Highlighting.cpp:2117 (also 2126, 2178, 2193)` — changes behavior, safe-tonight
- fix shape: Latch the highlight identity before clearing it, at all four sites. In adjustRailVoltage() and adjustDACVoltage(), immediately before each `clearHighlighting()` add `int net = highlightedNet; int node = brightenedNode;`, then after the clear restore `brightenedNet = net; brightenedNode = node;` and call `highlightNets(0, net, 1)`. Restoring highlightedNet alone (the finding's sketch) is enough to 
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED as dead code, but the finding's described symptom is materially overstated; the real harm is narrower.

Mechanism confirmed in the current tree. src/eyecandy/Highlighting.cpp:2117-2118 (confirm), 2126-2127 (cancel), 2178-2179, 2193-2194 do `clearHighlighting(); highlightNets(0, highlightedNet, 1);`. Inside a Highlighting member, `clearHighlighting` resolves to the member at Highlighting.cpp:282, which sets `highlightedNet = -1` (line 294) before the next statement reads it. In highlightNets (line 1000) the guard is `encoderNetHighlighted > 0` (line 1011), so -1 falls to `brightenNet(

</details>

## #34 — CONFIRMED (1/1) [low] encoderNetHighlight mode 0 walks the node index out of bounds on a full net

- location: `src/eyecandy/Highlighting.cpp:367-368 (UP path); mirror defect at src/eyecandy/Highlighting.cpp:435-446 (DOWN rescan)` — behavior-preserving, safe-tonight
- fix shape: Two clamps plus one loop bound, all inside encoderNetHighlight's mode-0 block. (1) UP: fold the upper bound into the advance test at line 368 — `if ( highlightedNet >= 0 && highlightedNet < numberOfNets && ( currentHighlightedNode >= MAX_NODES || globalState.connections.nets[highlightedNet].nodes[currentHighlightedNode] <= 0 ) )` — so a full net ends at index MAX_NODES exactly as it would at a zer
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTE lens applied; every avenue failed. Current tree (dev, post-4e5b0d0) still has the unclamped code — the audit commits did not touch this function (git log for src/eyecandy/Highlighting.cpp tops out at 57cf63d, which left this block alone).

CURRENT CODE. `Highlighting::encoderNetHighlight` is at src/eyecandy/Highlighting.cpp:339; the claimed :356 is stale (that line is now a comment). The mode-0 UP body is 358-416. The defect is at 367-368:
  367: currentHighlightedNode++;
  368: if ( highlightedNet >= 0 && highlightedNet < numberOfNets && globalState.connections.nets[highlightedNet].nod

</details>

## #35 — CONFIRMED (1/1) [low] ImagesApp draws past the end of a short image buffer for unrecognized .bin sizes

- location: `src/eyecandy/ImagesApp.cpp:245` — behavior-preserving, safe-tonight
- fix shape: Clamp at the allocation, not at the draw site, so both draw paths are covered by one change. After the dimensions are final (right after the guess block at ImagesApp.cpp:145, before the alloc at 154), compute what the renderer will actually touch: `size_t drawBytes = (size_t)((width + 7) / 8) * (size_t)height;` then allocate `size_t allocSize = max(bitmapSize, drawBytes);`, zero it (`memset(bitmap
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTE lens failed on every avenue; the defect stands in the current tree.

NOT STALE: src/eyecandy/ImagesApp.cpp:245 is exactly the claimed call, `getDisplay().drawBitmap(0, 0, bitmapData, width, height, SSD1306_WHITE)`. `git log -- src/eyecandy/ImagesApp.cpp` shows only e795630 (the src/ folder move); none of the landed audit commits (57cf63d, 47325d3, dec1b45, 4e5b0d0) touched this file.

THE TRACE: defaults `width=128, height=32` (ImagesApp.cpp:93-94). Header path (98-125) only sets dimensions when `fileSize - 4 == (w*h+7)/8`; otherwise it rewinds and sets `bitmapSize = fileSize`. The size

</details>

## #39 — CONFIRMED (1/1) [low] setSmallFont maps small-font enums to stale fontList indices (5pt fonts resolve to 9-12pt fonts

- location: `src/oled.cpp:3824` — behavior-preserving, safe-tonight
- fix shape: Minimal, tonight: correct the seven stale cases in the switch at src/oled.cpp:3824-3851 to the indices verified against the current fontList literal — SMALL_FONT_UBUNTU->30 (ubuntu5pt7b), DOTGOTHIC->31 (DotGothic16_Regular4pt7b), IOSEVKA_REGULAR->24 (IosevkaSS08_Regular9pt7b), IOSEVKA_5PT->32 (IosevkaSS08_Light5pt7b), PRAGMATISM_5PT->16 (Pragmatism5pt7b), FREEMONO_5PT->33 (FreeMono5pt 5pt entry), 
<details><summary>verifier reasoning</summary>

**Lens 1:** CONFIRMED as a code defect, but the finding's one live-harm mechanism is REFUTED — it is unreachable today.

1) The stale indices are real, verified programmatically. I dumped the `fontList[]` literal at src/oled.cpp:184-230 by parsing the array in source order: 12=ANDALEMO5pt7b, 16=Pragmatism5pt, 18=Pragmatism7pt, 20=Pragmatism9pt, 21=Pragmatism10pt, 22=Pragmatism11pt, 23=Pragmatism12pt, 24=IosevkaSS08_Regular9pt, 25=IosevkaSS08_Regular11pt, and the actual 4-5pt small fonts at 30=ubuntu5pt, 31=DotGothic16_Regular4pt, 32=IosevkaSS08_Light5pt, 33=FreeMono5pt, 34=EnvyCodeRNerdFont_Regular5pt. Th

</details>

## #40 — CONFIRMED (1/1) [low] stillWriteToFramebuffer guard is constant-false; draw paths write through a never-allocated NUL

- location: `src/oled.h:423 (the pinned flag); the reachable unguarded write sites are src/oled.cpp:2170 (print), 2249 (clear), 2984/1523/1776 (drawText/rows/text paths), 543 (displayBitmap), and clearFramebuffer — first triggered from src/Apps.cpp:1850-1853 (i2cScan)` — behavior-preserving, safe-tonight
- fix shape: Add one private helper to the oled class — `bool fbReady() const { return _displayPtr != nullptr && _displayPtr->getBuffer() != nullptr; }` (needs a non-static accessor for _displayPtr, or just a free `bool oledFramebufferReady()` in oled.cpp next to oledGetDumpBuffer, which already has _displayPtr in scope) — and add `if (!oledFramebufferReady()) return;` as an additional early-out to the wrapper
<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTE lens applied; the mechanism survived every refutation avenue, but two parts of the finding's framing do not.

FACTS CONFIRMED IN THE CURRENT TREE (line numbers are current, not stale):
1. `bool stillWriteToFramebuffer = true;` at src/oled.h:423 is the ONLY occurrence outside comparisons. A repo-wide grep (excluding .pio/.git/.cache) returns exactly one non-comparison hit — it is never assigned. So all ~40 `(!oledConnected) && stillWriteToFramebuffer == false` tests in src/oled.cpp are constant-false and never short-circuit.
2. The GFX buffer really is NULL until begin(): .pio/libdeps/ju

</details>

## #41 — CONFIRMED (1/1) [low] new Adafruit_SSD1306 and begin() results unchecked - OOM leaves null display pointer / NULL buf

- location: `src/oled.cpp:646` — behavior-preserving, safe-tonight
- fix shape: Fix ONLY the begin() half; skip the nullptr fallback entirely (dead code — operator new aborts). At src/oled.cpp:646, capture the return: `if (!getDisplay().begin(SSD1306_SWITCHCAPVCC, address, false, false)) { oledConnected = false; return 0; }` — bailing before the setRotation/setTextColor/invertDisplay/setFont/clearDisplay sequence at 649-657, so nothing touches the NULL framebuffer. Returning 
<details><summary>verifier reasoning</summary>

**Lens 1:** HALF REFUTED, HALF SURVIVES — and the refuted half is the headline.

REFUTED: the `new Adafruit_SSD1306` claim (src/oled.cpp:156, the claimed line 150 is stale). The scenario's premise — "with -fno-exceptions, heap exhaustion returns nullptr" — is factually wrong for this toolchain. Disassembling the shipped .pio/build/jumperless_v5/firmware.elf: `operator new(unsigned int)` at 0x100fc938 calls `__wrap_malloc`; on NULL it calls `std::get_new_handler()`, and with no handler installed (nothing in src/ calls set_new_handler) it falls through to `bl __abort_veneer` at 0x100fc958. `abort()` (0x2000

</details>

## #42 — CONFIRMED (1/1) [low] OLED autodetect runs on OG and can permanently promote hardware.revision to 7

- location: `src/oled.cpp:4441` — behavior-preserving, safe-tonight
- fix shape: Add a board-capability early-return as the first statement of `autoDetectAndConfigureOled()` in src/oled.cpp:4441: `if (!board::currentBoard().caps.hasOled) return false;` (caps.hasOled is false on OG per src/boards/og/board_og.cpp:110, true on V5 per src/boards/v5/board_v5.cpp:113). One guard covers both call sites — the boot path at src/main.cpp:474 and the per-second calibration hotplug poll at
<details><summary>verifier reasoning</summary>

**Lens 1:** THE FINDING'S STATED HARM IS REFUTED. The chip-mapping claim is simply wrong:

- `revisionNumber` is write-only across the entire repo. Grepping `src/ lib/ modules/ include/` (and the whole tree minus .pio/.git) yields only the definition (src/Peripherals.cpp:340), the extern (src/Peripherals.h:104), and four assignments (src/remembering/PersistentStuff.cpp:127,140,153 and src/oled.cpp:4466). Nothing ever reads it. Setting it to 7 is a no-op.
- `jumperlessConfig.hardware.revision` has exactly two functional consumers. src/LEDs.cpp:136 (`revision <= 3`) sits in the `#else` arm of a `#if defined

</details>

## #46 — CONFIRMED (1/1) [low] Core-1 renders read globalState nets mid-rebuild - torn frame plus TOCTOU OOB read of nodesToPi

- location: `src/LEDs.cpp:2465` — behavior-preserving, safe-tonight
- fix shape: Do ONLY the local-copy half of the sketch. At the top of the `for (int j = 0; j < MAX_NODES; j++)` body in lightUpNet (src/LEDs.cpp:2423), read the field once into a local — `const int n = globalState.connections.nets[netNumber].nodes[j];` — and replace every `globalState.connections.nets[netNumber].nodes[j]` inside that iteration with `n`: the `<= 0` break (2424), the `<= NANO_A7` guard (2465), t
<details><summary>verifier reasoning</summary>

**Lens 1:** I tried hard to refute this and could not. The mechanism survives, but only one half of the claim is real; the harm ceiling is genuinely low.

WHAT IS STILL TRUE IN THE TREE

1. The guard/use split is unchanged. `lightUpNet()` at src/LEDs.cpp:2401 checks `globalState.connections.nets[netNumber].nodes[j] <= NANO_A7` at src/LEDs.cpp:2465 (NANO_A7 = 93, src/JumperlessDefines.h:385), then RE-READS the same `int16_t` field (src/routing/MatrixState.h:17) as the `nodesToPixelMap[120]` index at src/LEDs.cpp:2620, 2646, 2650, 2657, 2661, 2664, 2667, 2670, 2673. Node values legitimately exceeding 119 do

</details>

## #47 — CONFIRMED (1/1) [low] Core-0 LED animations can run leds.show() concurrently with core 1's in-flight frame - same DMA

- location: `src/Menus.cpp:5476` — behavior-preserving, safe-tonight
- fix shape: Minimal and reuse-only: at the four core-0 exclusive-strip sites (src/Menus.cpp:5454 and :5476, src/SingleCharCommands.cpp:3954, src/Apps.cpp:1718) replace the bare holdCore1Frames()/releaseCore1Frames() pair with pauseCore2ForFlash(25)/unpauseCore2ForFlash(...) — that helper (externVars.cpp:210-236) already does exactly the fix sketch's "hold, then spin bounded on core2busy while pumping TinyUSB"
<details><summary>verifier reasoning</summary>

**Lens 1:** Every refutation avenue I tried failed, but the harm is smaller than the finding claims.

WHAT I TRIED TO REFUTE WITH, AND WHY IT FAILED:

1. "holdCore1Frames() must drain core 1." It does not. src/coredination/externVars.cpp:26-30 is nothing but `core1FrameHoldDepth[core]++; __dmb();`. No wait on core2busy, no FIFO handshake. The only helper that drains is pauseCore2ForFlash() (externVars.cpp:210-236), and none of the cited LED sites use it.

2. "Core 1 re-checks the hold right before it touches the strip." It does not. src/main.cpp:1734-1739 checks core1FramesHeld() after acquiring core_sync

</details>

## #50 — CONFIRMED (2/2) [low] needsReset restore path in updateConfigFromFile drops usb_cdc and usb_audio sections (silent co

- location: `src/configManager.cpp:959` — behavior-preserving, safe-tonight
- fix shape: Minimal: append the two missing assignments after src/configManager.cpp:959, mirroring the sibling at 907-908 — `jumperlessConfig.usb_cdc = savedConfig.usb_cdc; jumperlessConfig.usb_audio = savedConfig.usb_audio;` — and move/duplicate the "KEEP THIS LIST COMPLETE" comment (901-906) onto this copy too. Better, and still small: hoist both lists into one `static void restoreAllConfigSections(const st
<details><summary>verifier reasoning</summary>

**Lens 1:** CODE DEFECT: confirmed, still present, unchanged by the landed audit commits.

`struct config` has exactly 14 members (src/config.h:56 firmware, 99 hardware, 126 dacs, 184 debug, 191 routing, 204 slots, 212 usb_cdc, 235 usb_audio, 327 calibration, 335 logo_pads, 361 display, 374 serial_1, 384 serial_2, 408 top_oled). The needsReset restore list at src/configManager.cpp:948-959 names 12 of them; a mechanical diff of the two lists returns exactly {usb_cdc, usb_audio}. The sibling isNewerFirmware list one block up already carries both (src/configManager.cpp:907-908) plus the "KEEP THIS LIST COMPL

**Lens 2:** CONFIRMED in the current tree, but the blast radius is smaller than "medium" implies — it is a one-time, self-healing wipe on an already-degenerate config file.

The defect, verified:
- `struct config` (src/config.h:52-412) has exactly 14 members: firmware, hardware, dacs, debug, routing, slots, usb_cdc (207-212), usb_audio (228-235), calibration, logo_pads, display, serial_1, serial_2, top_oled. No top-level scalars.
- The needsReset restore list at src/configManager.cpp:948-959 names 12 of those 14. usb_cdc and usb_audio are the only two missing.
- The sibling isNewerFirmware path at 889-908

</details>

## #6 — MIXED (1/2) [high] Stack buffer overflow: sprintf("%s cleared") into node1Name[12] on every named-node clear

<details><summary>verifier reasoning</summary>

**Lens 1:** STALE — fixed by landed audit commit 57cf63d. `git log -L 3736,3742:src/Probing.cpp` shows that commit removing exactly the construct the finding describes: `- char node1Name[ 12 ];` (plus the dead node2Name/bothNames block that read it uninitialized) replaced by `+ char node1Name[ 24 ];` with the comment "24 covers the longest short name + \" cleared\" + NUL (\"NANO_3V3 cleared\" = 17) - the old [12] was a stack smash on every named-node clear". Current tree: src/Probing.cpp:3740 declares `char node1Name[ 24 ];`, src/Probing.cpp:3741-3742 fills it with `snprintf( node1Name, sizeof( node1Name 

**Lens 2:** TRACE RESULT: the defect was genuine, but it is ALREADY FIXED in the current tree by commit 57cf63d ("Release audit: 30+ confirmed findings fixed across the codebase").

Pre-fix evidence (57cf63d^:src/Probing.cpp): `char node1Name[ 12 ];` at old line 3727, and old line 3823 `sprintf( node1Name, "%s cleared", definesToChar( nodesToConnect[ 0 ] ) );` — exactly the claim, at exactly the claimed line. So the finding was correctly reported against the tree it was written on.

Current tree, walked hop by hop:
- src/Probing.cpp:3602 — clear-mode entry: `if ( node1or2 >= 2 || ( s.setOrClear == 0 && no

</details>

## #32 — MIXED (1/2) [medium] _DIRECT_PIXELS_ overlay is not session-only: per-pixel dirty marks drive autonomous flash write

<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTED — already fixed in the current tree by audit commit 57cf63d ("Release audit: 30+ confirmed findings fixed across the codebase").

The finding's whole mechanism rests on `_DIRECT_PIXELS_` being absent from `overlayIsSessionOnly()`. It is present now:

src/eyecandy/GraphicOverlays.cpp:368-373
```
static bool overlayIsSessionOnly(const char* name) {
    return strcmp(name, "_SELFTEST_") == 0 ||
           strcmp(name, "_PARTS_") == 0 ||
           strcmp(name, "_DIRECT_PIXELS_") == 0 ||   // scratch surface (sweep)
           strncmp(name, "_GUIDE_", 7) == 0;
}
```
`git log -L 368,373:src

**Lens 2:** ALREADY FIXED — the defect was genuine, but the chain is broken in the current tree by audit commit 57cf63d.

Trace of the claimed scenario against current code (src/eyecandy/GraphicOverlays.cpp):
1. Entry: MicroPython `set_pixel` → src/JumperlessMicroPythonAPI.cpp:3764 `graphicOverlayState.setPixel(row, col, color)`. (Second entry: the built-in Snake app at GraphicOverlays.cpp:687/689 calls setPixel per frame.)
2. `GraphicOverlayState::setPixel()` at GraphicOverlays.cpp:236. Bounds gate at :237 passes for 1..10 / 1..30.
3. :244 `findByName("_DIRECT_PIXELS_")`. On the first pixel of a session 

</details>

## #44 — MIXED (1/2) [low] jl_fs_open_file: unchecked `new File` crashes (while holding fs_mutex) instead of raising Pytho

<details><summary>verifier reasoning</summary>

**Lens 1:** The cited code is still there verbatim (src/JumperlessMicroPythonAPI.cpp:3227: `File* file = new File( FatFS.open( path, sanitizedMode ) );`, inside the fs_mutex_acquire() at :3225, contract at :3251/:3262 mapping EBUSY/ENOENT/EMFILE, consumed by modules/jumperless/modjumperless.c:5607 and :5999 which do `mp_raise_OSError( jl_fs_open_errno( ) )`). The line number is accurate and nothing has been fixed. But four independent refutation avenues each kill this as a standalone finding.

(1) It is literally inside finding #43's scope, not a separate defect. #43's own fix_sketch says: "In the SAME co

**Lens 2:** TRACE (all cites from the current dev tree; line numbers are NOT stale — 3227 is still the exact line).

1. Entry. MicroPython `jfs.open(path, mode)` → modules/jumperless/modjumperless.c:5607 `void* file_handle = jl_fs_open_file(path, mode);` → :5611 `mp_raise_OSError(jl_fs_open_errno())` when the return is NULL. A second binding at :5999-6002 is identical, and `lib/micropython/port/mphalport.c:41` externs the same symbol for the VFS-mounted plain `open()`. The OSError contract the finding cites is genuine.

2. Reaching the line. src/JumperlessMicroPythonAPI.cpp:3183 (null args → EINVAL), :321

</details>

## #51 — MIXED (1/2) [low] FS wipe with intact EEPROM store permanently suppresses image re-provisioning (files_provisione

<details><summary>verifier reasoning</summary>

**Lens 1:** The mechanism is intact in the current tree; only the line number in the claim is stale (2576 is a comment inside performConfigMigrations).

Chain, verified:
1. `provisionFirmwareFiles()` has exactly two callers, both inside `checkAndHandleFirmwareUpdate()`: src/configManager.cpp:2618 (isFirstBoot) and :2641 (wasUpdated). No terminal command, no menu item, no filesystem-app path calls it (`grep -rni provision src/` returns only declarations at configManager.h:33-34 and unrelated example/project provisioning). The if/else chain ends at src/configManager.cpp:2645 with no third branch.
2. `isFirs

**Lens 2:** REFUTED — the claimed chain breaks at a step the finding never examined. The finding's load-bearing premise is "eepromReconcileAfterConfig() restores last_version from the EEPROM store, so checkAndHandleFirmwareUpdate sees neither isFirstBoot nor wasUpdated." On the wipe boot that restore is fed a store that has ALREADY been clobbered to last_version="".

Full trace of the documented [env:jumperless_v5_erase] recovery boot (V5; OG is moot — provisionFirmwareFiles is a no-op stub at src/configManager.cpp:2480-2487):

1. scripts/erase_fs_partition.py docstring (lines 26-30) confirms the erase co

</details>

## #0 — REFUTED (0/1) [refuted] PARTPICK sig= machine line lacks the \r\n prefix its siblings carry (fixed in uncommitted worki

<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTED — stale. The defect does not exist anywhere in the current tree; commit 57cf63d ("Release audit: 30+ confirmed findings fixed") is precisely the commit that fixed it.

Evidence:

1. Current code. src/PartsApp.cpp:687 reads `Serial.print("\r\nPARTPICK sig=");` — the prefix the finding says is missing is present. (Claimed line 649 is stale; the emit moved to 687.)

2. Direct provenance. `git log -L 687,688:src/PartsApp.cpp` shows the exact flip in 57cf63d:
   `-                    Serial.print("PARTPICK sig=");`
   `+                    Serial.print("\r\nPARTPICK sig=");`
   introduced o

</details>

## #8 — REFUTED (0/1) [refuted] rotaryDivider permanently left at 3 after any probe-mode connect/clear toggle

<details><summary>verifier reasoning</summary>

**Lens 1:** The code pattern is real but the harm is fiction — the finding is wrong at BOTH ends.

MECHANISM (confirmed present): src/Probing.cpp:2919-2920 inside PROBE_REARM does `s.savedRotaryDivider = rotaryDivider; rotaryDivider = 3;`. The mode-toggle paths (src/Probing.cpp:3407, 3437, 3491) set `s.state = PROBE_ARM/PROBE_REARM` and return, so PROBE_REARM re-runs and re-captures the already-overwritten 3. probeExitTail restores it at src/Probing.cpp:3967. probeMode() (src/Probing.cpp:4057-4082) has no outer save/restore. So yes, after a toggle the exit restores 3.

REFUTATION 1 — the restored value is

</details>

## #10 — REFUTED (0/1) [refuted] Menu-cancel exit posts the nets render BEFORE clearing inClickMenu, so core 1 can consume and d

<details><summary>verifier reasoning</summary>

**Lens 1:** The code the finding describes is present and unchanged in the current tree: src/Menus.cpp:553-573, the `returnedMenuPosition == -2` cancel path, does `b.clear()` (565) → `requestLedShow(-1)` (566) → `inClickMenu = 0` (567). So the ordering claim is factually accurate. The HARM claim is what fails, on two independent code-provable grounds.

(1) The CLEAR intent cannot be lost. `clearBeforeSend` is a plain global (src/main.cpp:1507). Core 1 sets it at main.cpp:1786 the moment the request is taken (`if (taken & LED_CLEAR) clearBeforeSend = 1;`) but only consumes/zeroes it at main.cpp:1829-1832, 

</details>

## #11 — REFUTED (0/1) [refuted] Advisory logoLedAccess flag silently drops cross-core logo/pad override writes instead of retry

<details><summary>verifier reasoning</summary>

**Lens 1:** The code the finding describes is still present verbatim — `setLogoOverride` gives up at src/LEDs.cpp:1448-1450, `clearColorOverrides` gives up at src/LEDs.cpp:1575-1577, and core 1's `logoSwirl` holds the flag (set at src/LEDs.cpp:3771, cleared on every exit path at 3789/3801/3816/3828/3838/3882). `logoSwirl` really does run on core 1 (called from `loop1()` at src/main.cpp:1792 and :1799; `loop1` starts at src/main.cpp:1538) while `clearColorOverrides` callers in Menus.cpp/Probing.cpp run on core 0. So the mechanism the finding names exists. Every consequence it draws from it, however, fails.

</details>

## #12 — REFUTED (0/1) [refuted] getActionFloat sets rotaryDivider=3 and never restores it on any exit

<details><summary>verifier reasoning</summary>

**Lens 1:** The CODE FACT is accurate but the HARM MECHANISM is refuted on both targets.

Code fact (current tree, unchanged by the landed audit commits): `getActionFloat` at /Users/kevinsanto/Documents/GitHub/JumperlOS/src/Menus.cpp:2862 does `rotaryDivider = 3;` at :2874 with no `lastDivider` save, and returns at :2994 (long-press/probe cancel), :3018 (confirm), :3027 (serial cancel) without restoring. (:3351 is unreachable — the `while(true)` never breaks.) Its siblings do save/restore: getActionInt :3389-3390 / :3446 / :3456 / :3465, getActionString :3662-3663 / :3711…, selectNodeAction :2632 / :2853,

</details>

## #15 — REFUTED (0/1) [refuted] deleteDirectoryContents called without fs_mutex by the configManager wipe flows, violating its 

<details><summary>verifier reasoning</summary>

**Lens 1:** The finding's premise — a "documented lock contract" that the configManager wipe flows violate — does not exist in the current tree.

1. The named contract-holder is gone. `provisionMicroPythonExamples` appears nowhere in the repo except inside the comment itself (`grep -rn "provisionMicroPython"` hits only src/remembering/FilesystemStuff.cpp:3221 and a stale worktree copy). Its successor is `initializeMicroPythonExamples` (src/remembering/FilesystemStuff.cpp:3541), and that function does NOT hold fs_mutex either — at src/remembering/FilesystemStuff.cpp:3593-3601 it performs the identical sequ

</details>

## #37 — REFUTED (0/2) [refuted] Internal I2C bus scan can never detect devices (0-length probe bit-banged with zero delay at 1 

<details><summary>verifier reasoning</summary>

**Lens 1:** MECHANISM: confirmed exactly as claimed. HOP 1 — entry: `@i` reaches src/SingleCharCommands.cpp:2788 cmd_i2cScan; `input.indexOf('i') != -1` at :2793 calls `i2cScan(0, 0, 26, 27, 1, 1)` at :2794, so internalScan=1. HOP 2 — src/Apps.cpp:1806 i2cScan: `internalScan != 0` at :1830 takes the no-bridge branch; :1845-1848 sets `WireScan = &Wire` (I2C0); the `internalScan == 0` block at :1856-1862 (which is the only place `setClock(100000)` is called) is skipped, so Wire keeps whatever clock it has. HOP 3 — the scan loop at src/Apps.cpp:1881-1882 does `beginTransmission(deviceAddr)` then `endTransmis

**Lens 2:** REFUTED by direct measurement on the connected V5, plus a fleet-wide code proof.

1) BENCH COUNTEREXAMPLE. The claimed-broken path is still in the tree unchanged (src/Apps.cpp:1881-1882, `WireScan->beginTransmission(deviceAddr); byte error = WireScan->endTransmission();`, reached with `WireScan = &Wire` when `internalScan == 1`, src/Apps.cpp:1845-1848). Its only caller is `@i` (src/SingleCharCommands.cpp:2793-2795, `i2cScan(0,0,26,27,1,1)`). I ran `@i` on /dev/cu.usbmodemJLV5port1 (after waiting out a sibling agent's HIL suite - never killed or interrupted it) and got:

    Scanning internal I

</details>

## #48 — REFUTED (0/1) [refuted] lastChipXY bookkeeping RMW happens outside the wire arbitration - a raw crosspoint can become u

<details><summary>verifier reasoning</summary>

**Lens 1:** The code the finding points at is still present and unchanged: `sendXYrawUnchecked()` at src/CH446Q.cpp:1592 does the `lastChipXY` RMW at src/CH446Q.cpp:1600-1606, and the wire-arbitration spinlock region is at src/CH446Q.cpp:1680-1697. So the finding is not stale. It is, however, a misdiagnosis on three independent counts, and its fix sketch would be a regression.

1) The fix provably cannot fix the named scenario. The finding's own trigger is `jl_send_raw` at src/JumperlessMicroPythonAPI.cpp:1731 — but that function already performs the identical unlocked RMW one frame up, at src/JumperlessM

</details>

## #52 — REFUTED (0/1) [refuted] top_oled.show_in_terminal (an int port selector, values 0-3) is flattened to 0/1 by both config

<details><summary>verifier reasoning</summary>

**Lens 1:** REFUTED. The code fact is true but the premise and the harm are both wrong.

The flatten exists in the current tree (line numbers stale): src/configManager.cpp:1186 (`saveConfigToFile`: `file.print(...show_in_terminal ? 1:0)`) and src/configManager.cpp:2152 (`saveConfigIncremental`: `snprintf(newLine, ..., "show_in_terminal = %d;", ... ? 1 : 0)`). Claimed line 2135 shifted to 2152.

But `show_in_terminal` is not a port selector in practice — nothing reads it as an index. Complete consumer set:
- src/oled.cpp:2271 (`oled::show`) and src/oled.cpp:1709 (priority-flush path): both gate on `> 0`, t

</details>
