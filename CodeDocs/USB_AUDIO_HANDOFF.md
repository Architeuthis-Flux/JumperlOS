# USB Audio Class microphone — design, status and the crash that was

**Branch:** `usb-audio-uac2` → merged into `dev`.
**Board:** Jumperless V5 only (RP2350B). OG/RP2040 is gated out via `USB_AUDIO_ENABLE 0`.
**Status (2026-08-15):** feature complete, streaming crash **root-caused over SWD and fixed**,
soak-tested (see "Verified" below). Remaining hands-on items are in the checklist at the end.

---

## What this is

Two ADC channels streamed to the host as a USB Audio Class 2.0 microphone
(2 ch / 16 kHz / 16-bit), so QuickTime, Audacity, a DAW or `ffmpeg` can record a
live circuit with no host software. Route any two rows to the audio channels
(`connect(ADC0, 20)`) and they become left and right.

Two independent layers, which is what makes "hidden unless asked for" work
without fighting the USB spec:

| Layer | Controlled by | Effect |
|---|---|---|
| **Visibility** | `usb_audio_setup()` / `usb_audio_teardown()`, or the `M` console command | Adds/removes the UAC2 function from the config descriptor, then re-enumerates |
| **Streaming** | The **host** selecting AudioStreaming alt-setting 1 | Starts/stops the ADC DMA |

MicroPython never pumps samples — it cannot keep up with the sample clock. A
C-side DMA engine does.

**Note for macOS users:** once the mic is enumerated, CoreAudio tends to open it
on its own (it becomes an available input; several system services hold the
default input open). `usb_audio_status()['host_open']` tells you. That means the
ADC is owned by the audio path most of the time the mic is enabled — see "ADC
sharing" for what that implies for probing.

---

## The streaming crash — root cause (proven) and fix

**Symptom was:** mic enabled and streaming → all 4 CDC ports vanish within
10–60 s with no user interaction; recovery only by SWD reflash.

**Mechanism (caught live over SWD, pre-fix debug build):** the ADC capture is
two DMA channels chained to each other, ping-ponging into `g_half[2][]`, with the
completion IRQ (`DMA_IRQ_1`, **core 1**) resetting each channel's write address
after every burst. On RP2350 a chain-trigger **reloads TRANS_COUNT but keeps the
current WRITE_ADDR**, so the pair is self-sustaining without the IRQ — and the
IRQ was the only thing keeping the pointer inside the buffer. Every flash
erase/program parks core 1 with interrupts masked (`rp2040.idleOtherCore()`
spins inside the doorbell IRQ handler with PRIMASK set — `RP2040Support.h:143`),
tens of ms per sector. During that window the DMA walked its write pointers
straight out of `g_half[]` at ~128 B/ms each.

Evidence, one `jfs` write of 3000 bytes after streaming had started:
`ch5.write_addr = 0x20081c50` (SCRATCH_Y — core 0's stack, **443 KB** past
`g_half`), `ch6.write_addr = 0x20082002` (top of SRAM, `CTRL.AHB_ERROR|WRITE_ERROR`),
every .bss flag full of ADC-sample bytes, core 0 in `isr_hardfault` with
`CFSR=UNDEFINSTR` (it popped ADC samples as a return address), core 1 spinning on a
`pauseCore2` that now read 10. `late_irq` was 0 and `adc_overrun` 0 right up to
the write — the earlier 30 s "stable" observations were simply the time until
the first flash write (macOS's own MSC housekeeping writes are one; the config
save 2 s after a probe session is another).

Why probe arbitration made it 6× faster: `Probing::service()` polls the pad-sense
channel (ADC5) at 100 Hz **at idle**, so the old "any read of ADC5/7 hands the ADC
back" rule paused and restarted capture every ~300 ms — each restart aborting the
chained channels — on top of never letting the mic actually run.

**The fix (`src/USBAudio.cpp`):**
- **Memory-safe DMA, no CPU involved:** each half is a 1 KB, 1 KB-aligned region
  and the channel's write address is ring-wrapped to it
  (`channel_config_set_ring(&c, true, 10)`). A late or missing IRQ now costs a
  glitch, never memory. `static_assert`s pin the geometry.
- **Late-IRQ detection:** if the completed channel is already BUSY again at IRQ
  time (sibling chained back), count `late_irq`, don't publish that half, request
  a resync. Publishing is held while a resync is pending so a rotated interleave
  never reaches the host.
- **RP2350-E5-safe stop:** clear `CTRL.EN` on both channels before
  `dma_channel_abort()`, then W1C the completions the aborts raise; the start path
  clears stale ints before enabling the IRQ.
- **Probe arbitration redesigned:** "probe wins" stays, but the *signal* is now an
  explicit `usb_audio_probe_activity()` stamped where a touch is genuinely decoded
  (`readProbeRaw()` when the ladder reads above `minimum_probe_reading`, and
  `MeasureMode::service()` while measuring). While audio owns the ADC the two probe
  channels read as 0 (never a smeared mean into the row decoder — a wrong-row select
  is worse than 10 ms of latency); a pad with the tip on it (sweep mean over the
  threshold) hands the converter to the probe within one core-1 pass. Resume 300 ms
  after the last stamp, and only if the host still has the mic open (`g_hostAlt`).
  Idle: `probe_pauses` stays flat, capture runs continuously.
- Also: no `Serial` I/O from core 1 anywhere on this path (the SWD-confirmed
  core-1 CDC wedge, `main.cpp:~1500`); `irq_set_exclusive_handler` is pre-checked
  with the non-panicking getters; a failed resync/start releases the ADC lock
  (`usbAudioReleaseAdc()`); `usb_audio_yield_adc()` runs the pump inline when
  called on core 1 (the logic analyzer arms from `loop1`) instead of waiting 250 ms
  for a pump that can't run; `usb_audio_resume_adc()` restarts capture after LA /
  self test / DAC calibration only if the host still has the mic open;
  `serviceUSBAudio()` honours `pauseCore2` like its neighbours; the DMA IRQ handler
  is verified flash-free (`objdump`: only calls `usbAudioDcBlock`, no `memset`).

---

## Verified on hardware (2026-08-15, this build)

- Enumerates as **"JL Audio In"**, 2 ch / 16 kHz; macOS binds it and opens it.
- Boot-restore: `Ms` → reboot → `bcdDevice` 257 from power-on, no port drop.
- **The killer experiment, post-fix:** three back-to-back `jfs` writes while
  streaming → all 4 ports alive, host frames climbing at 16 000/s,
  `late_irq`/`resyncs` ticked (expected), `adc_overrun = fifo_overflow = 0`,
  DMA write pointers inside their halves.
- 15-min streaming soak: `jfs` writes, connect/disconnect (slot autosave), full
  config saves (`usb_audio_save()` yields → saves → resumes), `probe_read()` polls,
  measure-mode episodes, DAC sets — no drop, no overrun, frames climbing throughout,
  `probe_pauses = 0` at idle (arbitration no longer thrashes), `claim_fail = 0`.
- Config persistence through the incremental writer (`MAX_CONFIG_SIZE` raised to
  8192 so the in-place rewrite is the normal path again).
- Both `jumperless_v5` and `jumperless_og` build clean.
- The crash log (below) caught a deliberate BusFault end-to-end.

---

## Architecture, and why each piece is shaped the way it is

Every one of these was learned the hard way on hardware. Do not "simplify" them
without reading the reason.

### Descriptor toggling
Two `static const` config descriptor arrays (`desc_fs_configuration` and
`desc_fs_configuration_audio`) plus two device descriptors differing only in
`bcdDevice` (0x0100 / 0x0101, so Windows re-runs composite enumeration).
`__wrap_tud_descriptor_configuration_cb` picks on a runtime flag. Two arrays
rather than one rebuilt buffer **on purpose**: `audiod_open()` stashes a raw pointer
*into* the descriptor and dereferences it for the whole session. No logging in the
descriptor callbacks — they run in TinyUSB task context.

### The UAC2 descriptor is hand-written
TinyUSB 0.20 deleted both the `TUD_AUDIO_MIC_*` composites and the
`TUD_AUDIO_DESC_*` building blocks. 118 bytes, `static_assert`-checked.
`wMaxPacketSize` **must be 196**, not 192 — `audiod_calc_tx_packet_sz()` asserts
`(48+1)*2*2 <= ep_in_sz`, and 192 makes alt-1 stall (enumerates, never streams).

### Two TinyUSB trees are in the build
The audio driver that links is Adafruit 0.20, but the core also builds
`sdkoverride/{hid,midi,msc,ncm}_device.c` against the pico-sdk's TinyUSB **0.18**
headers, which `#error` without `CFG_TUD_AUDIO_FUNC_1_DESC_LEN`, `_N_AS_INT`,
`_CTRL_BUF_SZ`. Those are defined purely to compile the core's files and are
inert for the driver that runs. This is why `JL_AUDIO_DESC_LEN` lives in
`usb_interface_config.h`.

### Cross-core split (critical)
- **Core 1** — `serviceUSBAudio()` from `loop1()`, and the DMA completion IRQ
  (`DMA_IRQ_1`). Converts samples, publishes to a lock-free SPSC ring.
- **Core 0** — `tud_audio_tx_done_isr()` drains the ring into TinyUSB.

`tud_audio_write()` is called **only** from core 0. Calling it from the core-1
DMA IRQ deadlocked the board against core 0's USB IRQ holding the same TinyUSB
FIFO mutex — the cross-core twin of the CDC livelock documented in
`custom_tusb_config.h`. `DMA_IRQ_1`, not 0: MicroPython's `rp2.DMA` owns IRQ 0.
`irq_set_exclusive_handler`, not `irq_add_shared_handler` — the shared-handler API
took the board off the bus.

### No panic-on-failure APIs anywhere in this path
A panic halts only the calling core, so the symptom is the device silently
vanishing with no log. `dma_claim_unused_channel(false)`, pre-checked
`irq_set_exclusive_handler`, decline instead.

### ADC sharing
The stream holds the global `readingADC` lock for its whole session. `readAdc()`
and `updateLazyAdcReadings()` short-circuit on `usbAudioOwnsAdc` and are served
from the DMA sweep means (all 8 channels ride the round-robin). ADC5/ADC7 (probe
pad-sense / tip) read 0 while owned — see arbitration above. Long-running real-ADC
users wrap themselves in `UsbAudioAdcYield` (self test, DAC calibration); the logic
analyzer yields on arm and resumes on stop.

---

## Debug-flow traps (RP2350 + OpenOCD) — read before flashing over SWD

1. **OpenOCD's reset does not stop peripherals.** After `reset halt` the DMA, ADC
   and SIO doorbells keep their state. If the mic was streaming, the ADC DMA keeps
   writing samples into RAM — including OpenOCD's flash work area — and you get
   `** Verify Failed **` with ADC-looking bytes in flash. Use
   `test/hil/swd/flash_swd.sh`, which stops the ADC and then aborts all DMA
   channels after `reset halt`, before programming. (Watch the register:
   `CHAN_ABORT` is `0x50000464` on RP2350 - the RP2040 offset `0x...444` is
   `DMA_TIMER1` here and aborts nothing while appearing to succeed.)
2. **Stale SIO doorbells wedge the boot.** A SYSRESETREQ-style reset (debugger)
   that lands while `idleOtherCore()` has a doorbell rung leaves that bit set;
   arduino-pico's core 1 then parks itself forever at boot and core 0 hangs in
   `setup()` waiting for `core2initFinished`. `main.cpp` now clears both cores'
   doorbells from a static constructor (before core 1 is launched). Power cycles,
   watchdog reboots and BOOTSEL flashes were never affected.
3. **Don't `pkill` OpenOCD.** An un-shutdown session can leave the reset
   vector-catch armed; the next reboot (even picotool's BOOTSEL path) halts
   silently in ROM with no USB. Always `shutdown` (the helper does).
4. `arduino-pico`'s OTA stub copies its own `.data` into `0x20000110..` on every
   boot, so `__uninitialized_ram()` is **not** reset-safe here — the crash log
   uses POWMAN/watchdog scratch registers instead.

---

## Crash log (`src/CrashLog.cpp`)

A HardFault used to be a bare `bkpt`. Now it records core, PC/LR/xPSR/SP,
CFSR/HFSR/BFAR/MMFAR into reset-retained scratch registers and reboots through
the watchdog; the record prints once to the first terminal that gets the menu:

```
[crashlog] The last reset was a HardFault on core 0 (uptime 12893 ms, fault #1 since power-on):
[crashlog]   PC=0x1001603E LR=0x1001633D xPSR=0x89100000 SP=0x20081C90 EXC_RETURN=0xFFFFFFE9
[crashlog]   CFSR=0x00008200 HFSR=0x40000000 BFAR=0x60000000 MMFAR=0x60000000
```

Symbolize with `arm-none-eabi-addr2line -C -f -e .pio/build/jumperless_v5/firmware.elf <PC> <LR>`.

---

## API

```python
usb_audio_setup(left=0, right=1, full_scale=8.0, dc_block=True)  # show + configure
usb_audio_teardown()          # hide
usb_audio_is_enabled()        # advertised?
usb_audio_active()            # capture running?
usb_audio_status()            # dict, see below
usb_audio_set_rate(hz)        # 8000-48000, 1 kHz steps
usb_audio_set_range(volts)
usb_audio_save()              # persist -> enumerated from boot, no port drop
```

`usb_audio_status()` → `enabled, streaming, host_open, left, right, full_scale,
dc_block, sample_rate` plus the health counters `frames_sent` (climbs at
`sample_rate`/s while streaming), `fifo_overflow`, `adc_overrun`, `late_irq`,
`resyncs` (tick once per flash write — expected, the DMA rides through those),
`probe_pauses` (once per probe use), `claim_fail`, `init_fail`
(0 ok / 1 no DMA channels / 2 DMA_IRQ_1 taken / 9 not tried).

Console: `M` toggles, `M23` picks ADC2/ADC3, `Ms` saves, `M?` prints the status
above. **Needs Enter** — the dispatcher waits for a terminator so arguments can be
typed.

---

## Hands-on checklist (needs a human)

- Listen test: DAC tone through the crossbar on ADC0's row → GarageBand/REAPER,
  no discontinuities except across a flash write.
- Probe while recording: tap rows, select/connect mode, measure mode — probe
  behaves normally, `probe_pauses` ticks per use, capture resumes ~300 ms after
  the tip lifts.
- Logic analyzer / JulseView session while the mic is open (LA yields the ADC,
  mic resumes after).
- Boot-restore on Windows (`bcdDevice` 0x0101 re-enumerates the composite).
- Kevin's board is left with the mic **saved disabled** (opt-in via `M`/`Ms`).

## Useful commands

```bash
pio run -e jumperless_v5 -t upload          # normal flash (picotool, 1200-baud touch)
test/hil/swd/flash_swd.sh                   # RECOVERY over CMSIS-DAP (DMA-safe, clean shutdown)
pio run -e jumperless_og                    # must stay green
bash scripts/build_micropython.sh           # after touching modjumperless.c, before pio run
python3 scripts/generate_micropython_examples.py
```

Host-side checks:

```bash
ioreg -p IOUSB -w0 -l | grep -A12 '"Jumperless V5"' | grep bcdDevice   # 257 = audio present
```
