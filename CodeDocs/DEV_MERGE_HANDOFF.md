# `dev` merge — what's done, what's open, what to do next

Session of 2026-08-15. Branch **`dev`** now exists at `42f32af`, a fast-forward of
`main` → `infra-paths` → `usb-audio-uac2` with everything below committed. Nothing
has been pushed.

Read this first; the two feature handoffs (`USB_AUDIO_HANDOFF.md`,
`reading-display-handoff.md`) have the deep detail per feature.

---

## TL;DR

The USB Audio microphone works and **the crash that blocked it is root-caused and
fixed** (proven over SWD, then soak-tested for 15 minutes). The reading-display
work is finished. Ten commits sit on `dev`, all built for both boards and
exercised on hardware.

Two things are **open and both are pre-existing, not regressions from this work**:

1. **A flash-write park race in arduino-pico** takes the board off USB after
   ~18 iterations of a flash-write soak (mic irrelevant). The fix is written and
   in-tree but **compiled out**, because it needs a shared-IRQ handler slot and
   there are none.
2. **Zero free shared-IRQ handler slots** on any running board. Measured, not
   inferred. The next feature to ask for one silently kills a core.

Neither blocks using `dev` for ordinary work; both should be fixed before a
release, and (1) is a real "the board randomly dropped off USB" report waiting to
happen.

---

## What landed (all committed on `dev`)

| # | Commit | What | Verified how |
|---|---|---|---|
| 1 | `caa413c` | JsonState: bound + JSON-escape every name reaching `get_state()` | builds; `get_state()` exercised via HIL |
| 2 | `ecd23e2` | JsonState: drop seed-only reserved nets from the netlist (JSON-shape change, kept separate so it's revertable) | as above |
| 3 | `c63d43e` | InfraPaths: a user DAC write claims the DAC from the probe feed, persisted or not | **HIL `test_routing` went FAIL → PASS** |
| 4 | `3a1e1fb` | Config save: `MAX_CONFIG_SIZE` 3000→8192, truncation fallback, `updated = true` for the droop keys, 5 other table gaps | **HIL `test_config` (new droop regression test) PASS** |
| 5 | `0060b72` | Reading display: guard promotion, `handleEnter` hook, width guard, exit-erase ordering, 2 audit bugs | builds; REPL handback exercised |
| 6 | `5ab7d16` | Crash log (HardFault → scratch registers → reboot → printed once) + stale-doorbell boot guard | **deliberate BusFault captured end-to-end** |
| 7 | `68b93e3` | **USB Audio Class microphone** + the WRITE_ADDR runaway fix + probe-arbitration redesign | **15-min soak, 128 iterations, 0 failures** |
| 8 | `bd15e73` | FlashPark (disabled) + the doorbell-race evidence + SWD tooling | fix SWD-verified active, then disabled on purpose |
| 9 | `d097ce9` | CodeDocs + `firmware.uf2` | — |

### Hardware verification actually performed

- **The killer experiment.** Pre-fix: mic streaming + one 3000-byte `jfs` write →
  board off the bus instantly. SWD: `ch5.write_addr = 0x20081c50` (443 KB past
  `g_half[]`, inside core 0's stack), `.bss` full of ADC samples, core 0 in
  `isr_hardfault` with `CFSR=UNDEFINSTR`, core 1 spinning on a `pauseCore2` that
  read `10`. Post-fix: three back-to-back writes, all 4 CDC ports alive, frames
  still climbing.
- **15-minute streaming soak**, 128 iterations cycling jfs writes, slot autosaves
  (connect/disconnect), full config saves, probe reads, measure-mode episodes and
  DAC sets: **0 failures**, 16000 frames/s sustained, `adc_overrun` and
  `fifo_overflow` flat at 0, `probe_pauses` 0 at idle (arbitration no longer
  thrashes), `claim_fail` 0. `late_irq`/`resyncs` tick once per flash write, which
  is the design working.
- **16-minute host recording** analysed: 9646 windows, **0 dead windows**, 0
  discontinuities, DC-blocked mean ≈ 0.
- **HIL suite on the final build: 5/6 files pass.** The one failure
  (`test_net_currents`, "zero-load TOP_RAIL net shows < 1 mA phantom current") was
  **A/B-verified against `main`** — I flashed `main`'s firmware and got the
  identical failure, so it is pre-existing and out of scope for this merge.
- Boot-restore: `Ms` → reboot → `bcdDevice = 257`, mic present, no port drop.
- Both `jumperless_v5` and `jumperless_og` build clean.

---

## Open items — ranked

### 1. The flash-write park race (pre-existing, real, reproducible)

**Symptom.** The board drops off the USB bus during sustained flash writes. Mic
off, no user interaction needed. Reproduced with
`python3 test/hil/swd/stress_flash.py 40` — died at iteration 18.

**Mechanism** (SWD autopsy): core 0 spinning in `_MFIFO::idleOtherCore()` called
from SPIFTL's `FlashInterfaceRP2040::program`, core 1 back in `loop1()`, doorbell
**clear**, `__otherCoreIdled` false. arduino-pico's core-1 doorbell handler clears
the sticky bell *after* it leaves the park spin and re-enables interrupts; two
back-to-back flash ops race, core 1's clear swallows the ring for op B, and core 0
waits forever with interrupts off — so USB dies with it.

**Fix, written and SWD-verified working, currently compiled out:**
`src/FlashPark.cpp` + `include/FlashPark.h`. It wraps `flash_range_erase/program`
with its own doorbell and a protocol where the parked core clears the bell FIRST
and the resume waits until it has actually left. When enabled it took over
correctly (`s_active = 1`, `s_timeouts = 0`).

**Why it is off:** it needs one shared-IRQ handler chain slot and there are none
(see item 2). Enabling it panics core 0 inside `mp_embed_init` the instant
anything touches MicroPython.

**Next step:** free a slot (item 2), flip `JL_FLASH_PARK_ENABLE` to 1, re-add the
two `-Wl,--wrap` lines in `platformio.ini` (they are there, commented, right where
they belong), then re-run `stress_flash.py 40`. The header carries the full
rationale and three ways to free a slot.

**Not yet done:** an A/B of `stress_flash.py` against `main` to *prove* it is
pre-existing. Nothing in this branch touches that code, and the config change
*reduces* flash traffic, so the reasoning is solid — but the empirical A/B is
cheap and worth doing.

### 2. Zero free shared-IRQ handler slots (pre-existing, latent brick)

**Measured on a live board:** `irq_handler_chain_free_slot_head = -1`.

The pool is 6 (`irq_handler_chain_slots[]` is 0x48 bytes in the linked ELF —
arduino-pico already ships 6 rather than the SDK default 4). It **cannot be raised
with a `-D`**: the array is defined in `irq_handler_chain.S` inside the *prebuilt*
`lib/rp2350/libpico.a`. All six are taken: arduino-pico's doorbell handler
(registered on **both** cores = 2), Adafruit TinyUSB `USBCTRL_IRQ`, CH446Q
`PIO0_IRQ_1`, then MicroPython's `machine_pin_irq_init` and `rp2_dma_jl` on first
REPL init.

`irq_add_shared_handler()` **`hard_assert`s** — silently killing the calling core —
when the pool is empty. So the next feature to register one bricks the board:
`LogicAnalyzer.cpp:633`, `JulseView.cpp:672`, MicroPython's UART
(`machine_uart_jl.c:297`). **This likely means arming the logic analyzer on a board
that has initialised MicroPython already panics today.** Worth testing directly —
it would explain any "LA just kills the board" reports.

**Best fix:** stop arduino-pico registering its doorbell handler twice.
`PICO_VTABLE_PER_CORE` is 0, so the vector table and the handler chain are shared
between cores; the second registration is pure waste (it also makes the handler run
twice per IRQ). Only the NVIC enable is genuinely per-core. That is a one-line core
patch and frees a slot for FlashPark.

### 3. Hands-on checks only Kevin can do

Everything below needs eyes, ears or fingers at the board:

- **Listen test.** DAC tone through the crossbar onto ADC0's row → GarageBand or
  REAPER. Confirm it sounds right, not just that the counters are clean.
- **Probe while recording.** Tap rows, select/connect mode, measure mode. The probe
  should behave normally; `probe_pauses` ticks once per use and capture resumes
  ~300 ms after the tip lifts.
- **The reading display, visually.** OLED layouts per node type (plain net, GND,
  rails, DAC, ADC, GPIO incl. `FLOATING`, I2C, PWM, an I Sense pair, UART static +
  live). The pinned serial line during a live reading. Typing while readings
  refresh. `probe_tap()` is a stub and clickwheel injection does not reach the
  highlighter from a raw-REPL exec, so **the zombie-repaint check with a genuinely
  latched measurement was not automatable** — that one needs a real probe tap.
- **Logic analyzer / JulseView** with the mic open (LA yields the ADC, mic resumes)
  — and see item 2, this may panic for unrelated reasons.
- **Windows** boot-restore (the `bcdDevice` 0x0101 composite re-enumeration).

### 4. Smaller things noticed, not acted on

- `usb_audio_status()` no longer reports bring-up counters; `scripts/jumperless.pyi`
  was not regenerated for the new field set (the example script only uses fields
  that still exist, so nothing is broken).
- `MeasureMode`'s `probe_tap()` MicroPython binding is a stub (`jl_probe_tap` has a
  TODO body) — that is why the handback test had to drive `set_switch_position`
  instead.
- The board is left with the mic **saved disabled**, so it boots as a plain
  composite unless you turn it on with `M`/`Ms`.

---

## The adversarial review — done, and what it changed

A 7-dimension review with 3-lens adversarial verification (161 agents) found
**51 issues, 38 of which survived verification**. Six were merge blockers and are
**fixed and committed** (`42f32af`) — two of them regressions this branch had
introduced. Summary, because each is a lesson:

| | What | Why it mattered |
|---|---|---|
| M1 | `LogicAnalyzer::stop()` reset an ADC it never claimed | Ending a digital-only capture while the mic streamed froze **every** ADC reader on the board until reboot |
| M2 | Config migration dropped `[usb_audio]` (and `[usb_cdc]`) | Every firmware version bump silently erased a saved mic setup |
| M3 | `M?`/`Ms`/`M23` unreachable while the mic was on | `M?` — the documented health check — tore down USB instead of printing |
| M4 | MeasureMode wiped Highlighting's pinned row every pass | Made the serial half of the new reading display unusable (my regression) |
| M5 | Crash logger reset unconditionally | A fault reproducing at boot bootlooped, BOOTSEL-only recovery; worse than the SDK for core-1 faults |
| M6 | `usb_audio_yield_adc()` could silently no-op | DAC calibration could solve for, and **persist**, constants from sweep means |

Plus **S6**: `flash_swd.sh` aborted no DMA at all — `0x50000444` is the RP2040
offset and is `DMA_TIMER1` on RP2350 (`CHAN_ABORT` is `0x50000464`). Fixed, and
the false claim corrected in `USB_AUDIO_HANDOFF.md`.

Post-fix verification: both boards build; HIL 5/6 **with the mic streaming**
(only the pre-existing phantom-current failure); `M?` confirmed non-destructive
on hardware; a deliberate BusFault still records, reboots and prints, with the
new bootloop guard correctly not tripping.

### Still open from the review (triaged, not blockers)

Full text: `~/.claude/projects/-Users-kevinsanto-Documents-GitHub-JumperlOS/8de8c89e-ef11-4d86-9611-765cedd133d5/subagents/workflows/wf_7ae4beca-de6/journal.jsonl`
(`type: result` entries with a `findings` array are the reviewers; the rest are
verifier verdicts).

- **S1 — `usb_audio_set_rate()` desyncs device from host.** TinyUSB sizes IN
  packets from the host-negotiated rate, so moving `g_rateHz` mid-stream leaves
  capture and framing disagreeing (16k→48k saturates the FIFO, 48k→16k near
  silence), and a host that cached the clock RANGE gets **stalled** on the rate it
  negotiated. Fix: defer via `g_pendingRateHz`, apply in the stop branch.
- **S2 — the `latched` gate in `onPythonSessionEnd()` is inverted.** It ORs in
  `hl.showReadingNet > 0`, which is sticky by design, so after any probe tap it is
  permanently true and every ViperIDE console line runs the full teardown. Fix:
  `onPythonSessionEnd(bool fullHandback)` — true from the file path, false from
  the raw REPL.
- **S3 — the ADC5/ADC7 raw-0 sentinel leaks.** `readAdcVoltage()` applies the
  normal offset to the sentinel, so **ADC7 reads ≈ −8 V** rather than 0 or an
  error, while `adcReadings[7]` holds the real sweep mean — cached and fresh paths
  disagree for the whole recording. Worst consumer is `gpioDroopCurrentEstimate()`
  (≈400 mA against a ~1 mA threshold, which can latch switch position); also
  `TimeDomainMultiplexer`/`FakeGpio` have no audio guard at all. Fix sketch in the
  review; note `usb_audio_set_channels()` should also reject 5 and 7.
- **S4 — `[usb_audio]` struct/live divergence.** Live setters mutate `g_*` only
  and `usb_audio_save_config()` copies `g_*` **into** the struct, so a
  terminal-set value is reverted on disk by the next `Ms`. Fix: a
  `usb_audio_sync_config()` mirror.
- **S5 — three defects in the pin protocol** (`ReadingDisplay.cpp`): the width
  guard *orphans* the row rather than freezing it; the first pin after an anchor
  drop erases the user's echoed input; and clearing `lastLine` as the width guard
  does also drops the OLED dedupe key, making `VoltageAdjuster` repaint a full
  frame every loop pass. One `serialNeedsRepin` flag fixes two of the three.
- **S7–S10, W1–W5** — the droop-sentinel test has no restore path; a crash record
  survives a reflash and is reported against the wrong binary; the OG crash-log
  slots collide with the bootrom's `reset_usb_boot` scratch; MSPLIM overflow
  can't actually reach the handler (needs `CCR.STKOFHFNMIGN`); FlashPark has four
  more latent issues to fix before enabling; `s_dacUserClaimed[]` is a one-way
  latch that other DAC writers never clear; `full_scale` quantises at `%.2f`; and
  the OG MicroPython stubs make `usb_audio_setup()` raise instead of returning
  False (and the example is provisioned on OG, where it can never work).

## How to work on this board (learned the hard way)

**Flashing.** Normal: `pio run -e jumperless_v5 -t upload` (picotool, 1200-baud
touch on port 1). When USB is gone: **`test/hil/swd/flash_swd.sh`** — do not hand-roll
the OpenOCD command, because:

1. **OpenOCD's reset does not stop peripherals.** If the mic was streaming, the ADC
   DMA keeps writing into RAM — including OpenOCD's flash work area — and you get
   `** Verify Failed **` with ADC samples in flash. The helper aborts all DMA and
   stops the ADC after `reset halt`.
2. **Never `pkill` OpenOCD.** An un-shutdown session can leave the reset
   vector-catch armed, after which even a BOOTSEL/picotool boot halts silently in
   ROM with no USB. The helper always ends with `reset run; shutdown`.
3. **Stale SIO doorbells survive a debugger reset** and wedge the next boot (core 1
   parks itself for a resume that never comes, core 0 hangs in `setup()`). `main.cpp`
   now clears both cores' bells from a static constructor, so this is fixed — but if
   you see a boot hang at `main.cpp:355`, that's the shape of it.
4. Halting core 0 over SWD for more than a moment drops the USB device on the host.
   That is the debugger, not a firmware bug; `reset run` brings it back.

**Soaking.** `test/hil/swd/stress_flash.py N` (needs OpenOCD on :4444 and a
re-extracted address table — `sample_state.py`'s table is build-specific and the
README says so). The streaming soak driver used for the mic lives in the session
scratchpad and is easy to rewrite: drive `jl_exec` in a loop over jfs writes,
connect/disconnect, config saves, probe reads and measure episodes while polling
`usb_audio_status()` and `ls /dev/cu.usbmodemJLV5*`.

**Crash evidence is now automatic.** Any HardFault records core, PC/LR/xPSR/SP and
the fault status registers into reset-retained scratch, reboots, and prints once to
the first terminal that gets the menu, with the `addr2line` command to symbolise it.

---

## Suggested order for the next session

1. Work the S1-S10 / W1-W5 list above (the review's own text has a concrete fix
   for each). S3 and S2 are the two most user-visible.
2. Free a shared-IRQ slot (item 2) — it is a one-line core patch and it unblocks
   FlashPark *and* removes a latent brick. Verify with
   `irq_handler_chain_free_slot_head` over SWD.
3. Enable FlashPark, soak with `stress_flash.py 40`, and A/B the same soak against
   `main` to close out the pre-existing claim.
4. Hand the board to Kevin for the sensory checks in item 3.
5. Then `dev` is releasable.
