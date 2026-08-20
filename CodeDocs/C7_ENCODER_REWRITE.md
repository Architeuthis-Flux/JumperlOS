# C7 — the encoder rewrite (task #30's hard prerequisite)

**Status 2026-08-19, end of session: C7 AND task #30 are DONE** — stage 1
rig `b0ee09e` (row 75), removal `87e30ef` (row 77), placement + config key
`6711653` (row 78), the placement registry `cc6bdd4` (row 79), and the
re-home `4867fe5` (row 81), shipped as **5.7.4.1**.

**The plan below is superseded in one important way**: this doc's premise
(inherited from the SCHEDULER doc) was that PIO0 cannot reach empty. Kevin's
call — "make PIO0's gpio base 16, users will use routable gpio" — dissolved
it: the routable GPIOs are RP2350 pins 20–27, all inside a base-16 window,
so BOTH other blocks can be base 0 and every base-0 firmware program fits
across them. Final layout: **PIO0@16 = cs-strobe + bb-strip on SM3/SM2
(SM0/SM1 + 20 words free for `StateMachine(0..3)` users)**, PIO1@0 =
shifter + merged probe (exactly 32/32), PIO2@0 = top-strip + sampler.
The "free word on a full PIO0" anomaly: the merged probe program applies
late, so the sampler's early pio0-first claim cost it the 22-word
contiguous fit — the registry (row 79) is the instrument that catches that
class from now on.

2026-08-19. Kevin approved task #30 (clear PIO0 for user programs) including its
honest bottom line: **PIO0 cannot reach empty** — the CH446Q shifter is pinned
there (base-0 pins 14/15 + the cross-block IRQ handshake with the PIO2 strobe),
so the floor is 10 instructions / 1 SM, leaving users 3 SMs + 22 instructions.
C7 goes first because the encoder's 24-instruction quadrature program is the
only thing whose shrink frees the room (`SCHEDULER_AND_HARDWARE_OFFLOAD.md`
§ "PIO block budget" has the arithmetic and the target layout).

**Scope decision (this session):** C7 replaces only the *raw-count source* —
the PIO program and how the CPU derives the count. Everything downstream
(detent hysteresis, paced emission, `encoderDirectionState`/`encoderButtonState`
semantics, every consumer) stays byte-identical. The § D `queue_t` event-queue
idea is a separate, later change; it is NOT needed for #30 and touching the
~150 consumer sites in the same commit as the PIO swap would make the hardware
gate unreviewable.

---

## The impact map (compiled 2026-08-19, against `ac24c8a`)

### The program being replaced

- `src/hardwarestuff/quadrature.pio` (+ generated `.pio.h`, the one included at
  `src/RotaryEncoder.cpp:16`). Stock Pi example: 24 instructions, `.origin 0`
  (`quadrature.pio.h:41`), a 16-entry computed-jump LUT at absolute address 0
  dispatched by `MOV PC, ISR` — **this is why it can't relocate and why
  `pio_can_add_program` fails on any block whose word 0 is taken**.
- Free-running: keeps the signed count in Y, `PUSH noblock` every ~4 SM cycles;
  RX FIFO always holds the newest absolute count. Autopush off
  (`quadrature.pio.h:87`), clkdiv 1.0.
- `quadrature_encoder_program_init()` hardcodes offset 0 twice
  (`quadrature.pio.h:82`, `:96`); wrap set as absolute 15/23 (`:47`).
  `offsetEnc` is stored (`RotaryEncoder.cpp:104`) but never fed back to init —
  a relocatable replacement must thread the real offset through config + wrap.

### The read path that stays

- `quadratureCountBounded()` `src/RotaryEncoder.cpp:198-205` — drains only the
  queued FIFO words into static `s_lastQuadCount`, never blocks. **Contract to
  preserve: same name, same "absolute count, ~8 raw counts per detent"
  semantics** (`RotaryEncoder.h:294-300` bakes 8/detent into
  `EncoderAccelerator::Slow()`; `rotaryDivider` default 8 at `:261`).
- Poll cadence: `rotaryEncoderStuff()` `:873-916` — core-1-only
  (`:886-888`), 500 µs throttle (`:868`). Heartbeat = the unconditional call at
  the top of `core2stuff()` (`src/main.cpp:1623`); extras at `:1790`, `:1919`,
  `:1957`. Init on core 1 via `setupCore2stuff()` (`src/main.cpp:595/622/642`).
- `getEncoderRawCount()` `:210-215` (core 0, only under `encoderOverride`) and
  its raw-count consumers `Debugs.cpp:2697`, `:2855` (encoder button analyzer
  does its own detent math against the raw count).
- Downstream machinery that must not notice the swap: E9 pad-IE mitigation
  `:949-975` (pin-level, keep), click/rotation interlock `:1026-1111`,
  hysteresis/backlash/recoil `:1115-1188`, paced emission `:1192-1228`.
- Block search: `initRotaryEncoder()` `:48-121` walks `pio0/1/2`, skips
  non-base-0 blocks (`:71-77`). For #30's final layout the sampler should
  *prefer PIO1* (claim-order: base is settable only on an empty block —
  give PIO1 the merged probe, then this sampler, then the top strip, and move
  the main strip to PIO2 before anything else lands).

### Landmines

- **The six SWD-injected symbols must keep their names and linkage**:
  `encoderButtonState`, `encoderDirectionState`, `lastButtonEncoderState`,
  `encoderDirectionConsumed`, `buttonEventTimestamp`, `inClickMenu` —
  `test/hil/swd/refresh_jl_input_addrs.sh` resolves them from the ELF and
  `test_encoder_ui` injects through them. Rename = silent HIL breakage.
- The encoder button (`BUTTON_ENC` 11) is plain GPIO, not PIO — untouched.
- Comments/invariants that die with the old program (delete, don't port):
  the no-`pio_sm_restart` warnings `:472-479` and `:1011-1024` (OSR history —
  a stateless sampler has none), the blocking-drain rationale `:188-197`,
  the commented-out `resetPosition` block `:983-1000`.
- `smEnc` is `int` compared against `(uint)-1` at `:124`, `:185` — fix in
  passing.
- Second independent block-search that will notice freed space:
  `btnPioInit()` `src/selfreflection/Debugs.cpp:2172-2196`.
- `unInitRotaryEncoder()` `:129` and `printRotaryEncoderStatus()` `:218-225`
  name the old program symbol.
- No `queue_t`/`pico/util/queue.h` exists anywhere in the tree yet; the house
  cross-core pattern is `src/coredination/CoreMailbox.{h,cpp}` (core 0 → 1).
  Not needed for this pass.

### The design (chosen this session)

**A 1-instruction sampler**: `in pins, 2` wrapped on itself, autopush at 32
bits (= 16 AB samples per FIFO word), RX-joined FIFO (8 words deep), clkdiv to
~2.3-4 kHz sample rate. CPU decode runs at the existing 2 kHz poll: drain
words, walk the 16 two-bit samples through the same quadrature transition
table the PIO LUT encoded (+1/−1 per adjacent transition, 0 for invalid),
accumulate into `s_lastQuadCount` — identical semantics, fully relocatable
(no `.origin`, no computed jumps), and beats the plan's ~4-instruction
estimate. Buffering: 8 words × 16 samples ≈ 50+ ms of edge history at 2.3 kHz,
so a flash-park pause on core 1 no longer costs edges the way a naive
sample-per-word design would.

**Trade-off accepted**: the old program sampled at ~6 MHz and could not miss
an edge; the sampler subsamples at ~2.3-4 kHz. At 8 counts/detent even a
100-detent/s spin is ~800 edges/s — well under Nyquist, and the downstream
hysteresis (`rotaryDivider/2` backlash margin) absorbs occasional ±1 bounce
aliasing. The verification gate below is how we prove it by feel.

### The gate (Kevin's hands + HIL)

1. Detents 1:1 at slow, medium, and flick speeds, both directions, no
   direction reversals or missed steps; menu scroll at speed feels identical.
2. Encoder click / hold / hold-to-back unchanged (button path is untouched but
   the interlock reads the count).
3. `test_encoder_ui.py` 5/5 (after `refresh_jl_input_addrs.sh`).
4. The debug encoder analyzer (`Debugs.cpp`) still shows sane raw counts.
5. `X`: the PIO map shows the sampler's block/SM; instruction usage drops
   24 → 1 wherever it lands.

### Order of work for #30 once C7 verifies (from the approved plan)

1. C7 (this doc) — commit on Kevin's hands-on pass.
2. Re-home: PIO1 gets merged probe (22) + sampler (1) + top strip (4);
   main strip → PIO2; shifter stays on PIO0. Bases only on empty blocks —
   order matters.
3. Extend the PIO Status panel (`printPIOStateMachines()`
   `src/Peripherals.cpp:2628-2655`; the `X` panel at
   `src/SingleCharCommands.cpp:2838-2878` already prints bases) with
   owner + used-instruction count so #30 is checkable, not inferred.
4. Verify: probe LED + button (C5 gates), both strips, encoder, crossbar
   send, HIL suite.
