# Overnight ledger — 2026-08-27, 01:32–morning (autonomous shift)

Kevin's brief at 01:32: "I actually flipped the transistor for testing. I'm
going to bed, so spend the next 7 hours scanning for bugs and continuing our
tasks." The transistor-flip confession closes the E/C mystery for good: the
firmware measured the flipped part correctly all along.

## The bench

The debug probe was back on the bus at 01:35 (both cores examined). Solo all
night — every peer agent offline — which made tonight the first clean shot at
two standing watch-items: the full HIL suite with no bench contention, and
the USB-drop theory (SWD contention vs firmware). **Zero spontaneous USB
drops all night** under heavy serial + repeated SWD flashing: consistent with
the contention theory, still not proof.

## Flashed and hardware-verified

The full working tree (RAM pass + encoder detent work + the part-ID stack)
went onto the board at 01:40, then twice more as fixes landed. Verified on
hardware, in order:

- **Slot 3 integrity across flash**: bridge set byte-identical before/after.
- **Ownership clustering + placed-span naming** (321a876 + tonight): the
  placed-state Auto Scan reports `rows 10-17: 74153 (placed)`,
  `rows 17-19: 2N3906 (placed)`, `rows 40-47: 74153 (placed)` in 10s.
  Census hit-set identical across builds (also clears the halved CH446Q DMA
  buffer as a scan-path suspect). Two bugs found and fixed on the way:
  - a lone census hit on an owned row said "one leg of something?" instead
    of naming the part (the 2N3906 with E and C user-wired shows only its
    base to the census — the record fills in the rest now);
  - span naming now prints the record's claimed in-half row range, not just
    the census-visible slice.
- **Discovery mode** (2N3906 records + row 17/19 wires removed in software):
  first run reported `rows 18-19: DIODE 0.76V` — the *stale 74153 record*
  claims row 17 (its 2Y pin) and ownership clustering handed the emitter row
  to the chip's span; the DIODE→BJT upgrade then looked only at strictly
  EMPTY neighbors and went the wrong way (row 20). Fixed: a
  **pair-conducting (flag 5) neighbor outranks an empty one, ownership
  notwithstanding** — conduction into the span is electrical evidence, a
  record is just data, and the hFE test referees. Verified:
  `rows 17-19: BJT_PNP 0.64V`.
- **The S-paste parts loss** (found because the discovery restore FAILED):
  `board_state_restore` brought back bridges but dropped ALL EIGHT placed
  parts, deterministically. Root cause: `readPastedBlock` trims every line
  under a comment claiming "the parsers do not depend on indentation" — a
  claim that died when the indent-hardened `parts:`/`overlays:` scanners
  arrived. The same bytes restore perfectly through the file loader. Fixed
  (trailing-only trim); capture→paste→verify round-trip now preserves all 8
  parts. This hole sat under every HIL suite's "leave the bench as we found
  it" promise.
- **test_part_id.py: PASS (39 checks)** on the new build.
- **run_all.py full suite**: 7/10 passed on the first complete
  contention-free run. The two failing files were both test-side:
  - `test_slot_files` (4 checks): /config.txt now serializes `boot_mode` as
    a NAMED enum (`last_active`) — the test parsed digits only; and the
    no-write canary tried to arm inside `/slots/slot2.yaml`, which doesn't
    exist on a bench where slot 2 was never used (empty slots have no file
    since slots became files). Test fixed (named-enum parser + seed via
    `nodes_save()`): **PASS (96 checks)** on rerun.
  - `test_projects` (22/277 in-suite): rerun standalone (see below).
  - `test_encoder_ui`: SKIP (needs an OpenOCD :4444 session; the jl_input.py
    ADDR table was regenerated for tonight's build — `inClickMenu` is a
    direct global now, no pointer-cell deref).

## The review pass (two Opus reviewers, all findings triaged)

Reviewer A (part-ID stack) — all nine addressed:
1. **partScanBegin's -2/-5 exits destroyed lifted user wiring** (HIGH): the
   lift lands on the fabric before the ADC/GPIO claims; those two failure
   returns skipped the restore and markDirty had already queued the deletion
   for auto-save. Both now exit through the partScanEnd funnel.
2. **partScanPairSweep drove DAC0 with no guards** and left the hardware at
   0V while state claimed the user's voltage. It now refuses when the probe
   is fed from DAC0 or user wiring touches DAC0/ISENSE (census-only scan,
   printed), and restores DAC0 to state on exit.
3. **Span-close off-by-one**: a span whose last hit is row 28/58 was never
   closed — a part on 26/27/28 scanned clean and was reported as nothing.
4. **partsCommitPlacement removed the old part before validating the new**:
   geometry now validates first (it is pure), and the replace victim is kept
   for resurrection if applyPartPlacement refuses (bridge table full).
5. **legAdd failures were ignored** — an incomplete fixture measured a real
   diode as EMPTY. A sticky per-session flag turns any staging failure into
   a clean legsBuild false.
6. DIODE→BJT annexation could double-report the annexed row (skip recorded).
7. Fragmented placed parts printed identical lines repeatedly (deduped).
8. OG part_identify stub emits the same token set as V5 (`lifted=` added).
9. place_part help text taught the 7th arg (part_id) and replace-on-update.

Reviewer B (RAM pass + encoder) — encoder work verified clean (native
detent-anchor check mirrors the firmware token-for-token; PASS). RAM-pass
findings, all addressed:
1. **The UART overflow witness could never fire on RP2350**: trans_count
   0xFFFFFFFF arms ENDLESS mode (the count never decrements), so the delta
   was identically zero. Rewritten on the write-pointer delta (correct on
   both RP2350 and RP2040).
2. **CH446Q mid-list flush released the mailbox requester after word 512**
   with the rest of the list unsent, and stamped a bogus short latency
   (xbarLatSendDone latches). A partial-flush flag suppresses completion;
   only the final kick completes.
3. **The 2 KB undo-blob cap can't hold a parts-bearing board** (~4.7 KB on
   this bench) — clear-all undo silently restored nothing. Cap now 8 KB.
4. **A failed blob append still recorded the op with offset 0**, and undo
   then magic+CRC-validated whatever stale blob sat at offset 0 — silently
   replacing the user's board with an older one. size==0 now refuses, loudly.
5. z-check's heap script is aborted out of GuideChecks before free (the
   calloc move made a dangling ck.script possible; nothing dereferenced it
   yet).
6. Two stale `< 50` guards → ROW_ANIMATION_COUNT.
7. **assignedAnimations[-1] out-of-bounds write** on every out-of-range GPIO
   net (pre-existing; order inverted + bounds added).
8. *(informational)* encoderNetHighlight's divider default change also slows
   the terminal net-list live view — consistent with intent.

Plus the RAM doc's work-listed **rowAnimations off-by-one**: all four tail
blocks stamped `.index` into slot N and wrote the animation into N+1 —
warningNet rendered a zeroed dummy (DARK) and brightenedNet rendered the
warning animation. Fixed to the keeper loop's idiom; layout now matches the
comment (33 warning, 34 hl-net, 35 hl-row, 36 probe). NOT yet
hardware-verified (needs a warning-condition trigger; morning item).

## Continued build-out

- **The diode/LED/zener result card** (Parts > Test), the BJT card's idiom:
  type + name / rows / A K roles / Vf (+ color guess or Vz), board painted
  in the standing A/K colors — **A red, K blue** (current in at red, out at
  blue, the same warm-to-cool read as E→C). Auto-scan discoveries paint the
  same colors. Needs-bench: no discrete diode on the board tonight (the
  2N7000/diode drawer is Kevin's).

## The stale-file incident (and the harness fix it bought)

After the review-fix flash the board rebooted into a DEGRADED slot 3 (7
bridges / 7 parts: 2N3906_3 gone, the transistor's wires gone, 116-109
gone). Root cause: `board_state_restore` heals the LIVE state only - the
slot file catches up on a later auto-save. The in-suite test_projects chaos
(old-protocol keystrokes leaking into i2cscrn's script, a forced save)
wrote a degraded slot3.yaml mid-run; every later suite captured-and-restored
that degraded FILE while verifying the (good) live state; the reboot then
materialized the stale file. Recovered from /slots/slot3.backup.yaml
(byte-identical fingerprint after). jl.py's board_state_restore now runs
`nodes_save()` after a verified restore so the file converges with the
bench - the class is closed for every suite at once.

## The sweep lifts now (second pass on a review fix)

The first cut of the pair-sweep guard REFUSED when user wiring touched
DAC0/ISENSE - and the very first placed-state scan on that build printed
"pair sweep skipped": Kevin's standing UART_TX->ISENSE_MINUS wire would
have sidelined the sweep on this bench forever. Second cut: the sweep
briefly LIFTS measurement-path wiring exactly like an identify session
(Kevin's ruling), restores it on every exit including the ADC-refusal
bail. Verified: sweep runs, bridge set byte-identical after, scan 10s.

## The suite catches up with the ambient guide (b1ce6fb)

test_projects' 22 in-suite failures were all one thing: its guide phases
spoke the `GUIDE step=/state=` protocol the blocking-guide removal (08-24)
retired, and the suite could never run to a verdict before tonight to say
so. Ported: phase (vi) asserts `VIEWER steps=<n>` + `noscript` + no
placement side effects; the CRITICAL i2c teardown needle now drives the
SAME i2cScan path through `z check step 4` (GuideChecks survived whole)
with DISP committed through the run-file door. Two real traps found on the
way: `port1_command`'s quiet-drain bailed inside the i2c sweep's silences
(widened), and the `<3` leave SCHEDULES a deferred save of the outgoing
context that landed after the test's fs_write twice - the injection now
re-reads and retries once. test_ambient_parts (the retired guide_flow's
salvage, a self-described skeleton) runs 7/9: its z-check needle was
unmatchable ("CHECK result=" never occurs; fixed to "result=") and two
checks are racy by construction (PARTWARN vs port1_command's input reset;
a hardcoded cursor 1/) - it stays OUT of run_all until those are fixed.

## An honest loss

Kevin's /projects/i2cscrn/i2cscrn_run.yaml (959 bytes - his saved circuit
from yesterday's i2cscrn session) was overwritten at ~03:28 by my FIRST
hand-reproduction of the i2c needle: I drove `z i2cscrn new` bare, without
the run_file_capture() discipline the suite itself uses. The suite runs
before and after all restored his bytes; the hand-drive didn't. No backup
survives (the .hilbak copies are deleted on restore). The file now holds a
fresh template launch. Lesson recorded: hand-driving a `new` on a shipped
project gets the same capture/restore wrapper the suites use, no
exceptions.

## Watch-items from the final scans

- Row 1 reports "one leg of something?" twice (census v0 2.03-2.11, just
  under the 2.2 threshold, v1 ~0 = instant discharge). Either something
  really touches row 1 or the first-scanned row settles differently -
  worth one glance at the physical row 1 and, if empty, a settle tweak.
- Row 39 pair-flagged once (v1 = -0.00, adjacent to the chip block at 40)
  - single occurrence, graceful "one leg of something?" output, watch.

## The closing sweep (03:57)

**run_all.py: PASS, 10/10 files** - the first fully-green complete sweep
this harness has ever produced, on the final overnight build:
micropython_fs, routing, net_currents, config, stress, paste_state,
slot_files (96), parts_roundtrip (181), projects (275), encoder_ui (5).
Also green tonight on the same build: part_id (39), ambient_parts 7/9
(stays out of run_all until its two racy checks are fixed). The encoder
suite had never run handless before: jl_input's inClickMenu had to become
Menus::getInstance()'s MEMBER (instance + 0x34 via gdb) - the bare global
of the same name is a separate legacy variable the menu system never
consults. Clear-all undo bench-verified (12 bridges cleared, undone
byte-identically - it restored NOTHING before the blob fix). Final bench:
NB 12 / NP 8, bridge set byte-identical to the canonical state, and
slot3.yaml CONVERGED on disk (4665 bytes) - the new save-after-restore
doing exactly its job. Zero spontaneous USB drops the whole night.

## The context-flip watch-item, resolved (04:30)

The evening ledger's suspect ("the breadboard display's beacon path
auto-opening its project") is INNOCENT: the beacon/attach/pollParts code
contains no context-switching call at all, and a sweep of every
loadSlotFromPath caller in the tree finds only user/serial gestures (the
`>` context command, the launcher and Files flows, MP load_project, and
the last_active boot). The firmware has no unattended path that moves its
own context. What actually happened yesterday evening: the old-protocol
test_projects i2cscrn drives ADOPT the run context ("now your active
circuit") and bench contention kept killing the suites before their
restores ran - two flips, externally driven. And "slot 3's file rewritten
to a DISP-only state" is the live/file divergence class witnessed
first-hand tonight (the stale-file incident) - a foreign context's state
auto-saved into slot3.yaml. Both halves of that class are now closed: the
S-paste parts flattening (firmware) and the restore-converge (harness).
An unattended board does not move its own context; last night's board was
not unattended.

## Verified after the closing sweep (04:10-04:30)

- Rows 1/39 census artifacts: both identify EMPTY, both 3/3 persistent -
  fixed (census lane prime + lone-hit interrogation, commit 6), 3/3 clean
  scans after, 14s, part_id 39/39 again.
- rowAnimations slot layout read off the RUNNING target over SWD
  (gdb offsets + openocd read_memory, no halt): numberOfRowAnimations=37,
  slots 33/34/35/36 = types 3/4/6/5 with matching indices, slot 37 empty.
  Commit 2's "LED-visible behavior unverified" narrows to "the renderer
  consumes what the verified slots hold" - and its consumers are the
  hardcoded 33/34 that now point at real animations.

## Not done / morning list

- `identify_part()` no-args autodetect front door (needs the scan logic
  factored out of the UI launcher — too big for a night edit).
- Capacitance value (ring τ fit), curve tracer, I2C module probe (needs a
  module on the bench), pot identify + FET branch still needs-bench.
- Hardware-verify: warning/highlight animation slots, the pair-sweep skip
  print, the undo clear-all blob on a parts board, partScanBegin -2/-5 lift
  restoration (needs an ADC-exhausted fixture), CH446Q partial flush (needs
  a >512-word send).
- Kevin's duplicate records (2N3906_2/_3, the stale 74153 overlapping the
  7400, SSD1306_32_I_2) are still his data — replace-on-identity prevents
  new ones; a glance and a few removes clears the museum.
- Stray repo-root files left alone: `end` (empty, Aug 25 — looks like a
  shell-redirect accident, delete at will), `finish-git-cleanup.sh` (Kevin's
  to run — it pushes remote branch deletions).
- `/slots/slot2.yaml` now exists on the device (seeded canonical empty-slot
  file during the canary fix — harmless, matches slot 2's live state).

---

# Morning shift (Kevin's 09:09 bench notes)

Kevin woke, drove the bench, and left four items. The board had been
reorganized overnight-his-side: `/slots` now holds only `slot0.yaml`
(2N3906 @17-19, LED @21/51, five bridges), `power_source = gpio_first`
(his workaround), and the 2N3906 is physically flipped BACK — identify
now reads E,B,C exactly where the record says.

## 1. Part testing failed under DAC0 probe power — fixed, bench-verified

Not interference: `partScanBegin` and `partScanPairSweep` each carried a
hard `if (infraProbePowerSource() == DAC0) return -2;`. Kevin's ruling
("just disconnect the probe power entirely when doing part testing")
replaced both refusals with a park: `infraSetProbePowerEnabled(false)`
at session start, restored at every exit, riding the refreshes the
session already makes (infraEvaluate runs at every rebuild head — zero
added refreshes). Verified under `dac0_first` on the bench: identify
clean (BJT hFE 457, LED Vf 2.17) where the same calls refused before;
feed restored after every session; bridges byte-identical; part_id
39/39. Config restored to his `gpio_first` — DAC0 works again whenever
he wants to flip back.

## 2+3. The overlay/highlight rework — the part is now a first-class thing

The "weird partial overlays" were PartLabels' `_PARTS_` layer: warned
pins painted a standing FULL 5-cell column (looks exactly like routing)
and pin markers were all class-blue (85% of DB pins are class signal).

- Role colors everywhere: pin NAME keys the palette (E/A/S red, B/G/W
  yellow, C/K/D blue — the result cards' warm-to-cool read), fallback
  to class colors, pin-1 marker kept.
- Warnings: edge+inward pair PULSING at 400ms — pointed, and
  unmistakably not a net. No more full column.
- clearTransients(): `x` and every parts-app exit retire the standing
  paint (warn mute until the warning changes or re-fires, inspect
  windows dropped, part highlight cleared). Blooms deliberately stay —
  placement exits INTO its bloom. `x` also gained the clearHighlighting
  stale-net guard cmd_loadNodeFile always had.
- Encoder scroll: a placed part is a stop BEFORE the wiring test (zero
  wires needed). First landing from either end = WHOLE part (all pins
  role-lit bright, OLED card: name / pin assignments / cached test
  data); further detents walk that half-span's pins in travel order,
  then the row scan resumes past it. A focused wired pin gets the
  full net treatment; an unwired one paints its whole row in role
  color through the overlay. Select-tap = just the pin (Kevin's spec).
- Cached test data: PartDefinition grew lastTestType/Value/Value2
  (RAM-only, measuredOhms rules) — written by Test Part and the
  place-flow identify, shown on the whole-part card. Auto Scan does
  not populate it yet (parity is a one-liner if wanted).
- Timeout interplay: part focus stamps highlightTimer and rides the
  persistent 15s class (a stale 1.8s timer from a previous net stop
  was killing the card mid-read — caught pre-flash).

Bench (SWD encoder injection + port7 :leds/:oled): UP walk = 2, 6, 10,
17 PART(whole), E, B, C, 21 PART(whole), A, 25, 30... 51 PART(whole),
K, 53, 56, GND; DOWN mirrors. LED snapshot at pin-C focus: 0018a8
bright pair at row 19 edge, full row in dim 00062a (unwired row light),
E/B dim red/yellow dots. scrollPartIdx/scrollPartPin watched live at
0x2000eb45/44.

## 4. The netlist flood / app lockup — root-caused, fixed

Not "printing too fast" in general: listNets' live-update loop reprints
the ENTIRE colored listing whenever any GPIO reading flips — and net 6
held a floating GPIO input, which flips constantly. Hundreds of full
reprints per second until the app's renderer choked; then Adafruit CDC
write() (spins while port open + FIFO full, never drops) hung core 0.
Closing the wedged terminal is what freed it — DTR drop releases the
write. Fix: 200ms sticky-changed reprint holdoff (~5/s cap, no lost
updates) + availableForWrite drain guards before the first burst and
every reprint (1s stalled = abandon live mode, zero goodbye bytes).
App-side confirmation is Kevin's — port1 was his app's all shift.

## Still Kevin's / open

- Whole-part OLED card typography + LED feel — his eyes, his call.
- Select-tap pin highlight — needs the physical probe.
- The netlist fix against the actual app — needs his port1.
- Warn pulse visual — no active warning on the bench post-reboot
  (pinsUnverified is RAM-only and cleared by the flash).
- Auto Scan under DAC0 (sweep park) — mechanism identical to the
  verified identify park; not driven end-to-end (menu nav over SWD
  not worth the risk with his app attached).
- test_parts_roundtrip and test_infra_paths need port1 (the app held
  it all shift) — infra_paths is the most relevant unrun coverage for
  the park, it drives probe_power_source both ways. Run both when the
  port frees up.
- The `row=17 reason=` garble in his paste = the known async-PARTWARN
  interleave, still on the deferred list.

---

## The afternoon shift (12:53 round) — 5dc1f9d

Five asks from Kevin's 12:53 paste, all landed and flashed.

**The panel spaces its own columns.** `OLED_ALIGN_JUSTIFY` (oled.h/oled.cpp
`clearPrintShowRich`) spreads a row's flowing segments from the left pad to
the right margin using measured pixel widths. The part card's pin columns
are now segments instead of one fixed 21-char line — that line was 8px of
left pad plus 21 monospace chars on a 128px panel, which is why "C 19" hung
off the right edge. Focus bars became bars-or-spaces: an unfocused cell
wears spaces where the `|`s go, so moving focus no longer shifts the layout
by two characters (Kevin's exact ask).

**Rail/DAC part pins keep their names.** `showSpecialPinReading`
(Highlighting.cpp, beside `showGpioPinReading`) paints "Q17 E" over "Top
Rail 3.80 V". Two things it does deliberately differently from the GPIO
twin: it matches the pin by `brightenedNode`, never first-pin-on-net (a rail
carries many parts' pins — the wrong name is worse than none), and it is
hooked at all SIX paint sites (rails + DACs, one-shot and live) because the
live updater re-arms `highlightedNet` through `highlightNets` and would
otherwise stomp the name back within a cycle.

**The sweep tries three layouts.** `partScanPairSweep` now runs adjacent
(n,n+1), across the center channel (n,n+30) and one-apart (n,n+2); each
pass only sweeps what the last left unexplained, so a mostly-found board
pays almost nothing. Drive went **1.0V → 3.0V**: the LED Kevin watched the
scan miss on 21/51 measures **Vf 1.96V** (bench-confirmed post-flash via
`part_identify(21,51)` — red, lifted 2 wires), so it could not physically
conduct at 1V. The higher drive also drops the deviation floor 0.35 → 0.15mA,
putting ~10k resistors back in range (they moved only ~0.1mA at 1V — under
the old threshold, silently unseen). Cross-gap hits return as explicit PAIRS
and are identified BEFORE span forming, then claimed via `crossUsed`: their
rows sit in different halves and would otherwise decay into two width-1
"noise" spans, one per half. A width-3 span whose middle row never hit now
asks the two-lead question first (that shape belongs to the one-apart
arrangement, not to a BJT). A pass with <5 pairs says so rather than
silently judging nothing.

**The scan senses I2C.** Bench-proven before a line was written: SoftI2C
over the crossbar on rows 3/4 ACKed **0x3C** first try and lit every pixel
of Kevin's SSD1306. `partsFindClusterPower` reads the supply pins out of
clamp **orientation** — GND is every clamp's common ANODE, Vdd every clamp's
common CATHODE, signal pins are mixed. Forward-drop magnitude cannot separate
them (measured 0.78 / 0.86 / 0.91 V all overlap); orientation can, every time.
Then `partsProbeClusterI2C` follows the design doc §9 exactly: ground first,
supply LAST, INA1 watchdog bails over 20mA, both SDA/SCL orderings tried,
full teardown on every exit. It borrows I2C1 on pins **22/23**, never 26/27
— pin 27 is the live probe-power pad (BUF_IN/GP_8 on this board) — and
refuses outright when the panel owns Wire1. Findings are named from the
doc's `i2c_addr` table (first page, in rodata): "I2C 0x3C on 3/4 —
SSD1306/SH1106 display". Hooked into BOTH chip verdicts: the wide-span walk
and the small clamp star, which is where Kevin's 4-pin module actually lands
("rows 1-3: a chip?" in his 12:28 trace).

**PARTSEL stopped scrolling the terminal** (6dad4b5, earlier the same round):
it rides a pinned status row above the live reading, repainted in place.

### Delivery changed today

SWD stayed benched, but BOOTSEL no longer needs Kevin's hands: **open port5
(CDC 2) at 1200 baud and close it**, then `picotool load -x`. That's the
touch `secondSerialHandler` already watched for. PICOBOOT only — no `/rp2350`
drive mounts, so picotool is the tool, not a UF2 copy. Board came back with
all 10 nets intact. (`machine.bootloader()` over the MP REPL does NOT work —
it just resets.)

### Still Kevin's, from this round

- The three-arrangement sweep end to end: does the 21/51 LED now show up as
  a finding, and does CONNECT place it? The physics is confirmed, the whole
  Auto Scan pass is not.
- The I2C interrogation against his module in situ (it needs the scan's
  wide/star verdict to fire).
- The justified card at 4 pins, and whether the spacing feels right.
- His message tail was cut off twice today ("And the…" at 11:37, "it" at
  12:28) — whatever those were never arrived.

### Post-flash review round — c04f8c2

The advisor pass caught two bugs that would have fired on Kevin's first
scan over his module, both fixed and re-flashed:

- **LED skipped the clamp-star guard** at both sites. A clamp's forward
  drop is a measurement, and Vdd→SCL on his module reads "LED 2.23V" — so
  the walk would have split the SSD1306 into a phantom discrete LED,
  starved the interrogation below its legsLeft threshold, and offered the
  phantom for placement. LED now joins DIODE/ZENER in both guards.
- **The I2C probe's Wire1 teardown re-claimed pin 27** on connection-type-2
  boards (Kevin's): config still carries default sda/scl 26/27, and the
  restore skipped i2cScan's `connection_type != 2` guard — so every probe
  would have ended by putting I2C function + pullup on the probe-power pad.
  Now it matches i2cScan's guard and leaves Wire1 ended when the panel
  isn't on it.

Bench state after re-flash: 10 nets incl. probe power (BUF_IN–GP_8), Q17
placed (the rest of the parts were already gone before 12:53 — Kevin's own
removals; his scan trace confirms nothing said "already yours"). The
cross-gap LED identifies as red, Vf 1.96V — physically impossible at the
old 1V sweep drive, comfortably inside the new 3V. PICOBOOT caveat: port7
can enumerate silent after picotool; one `machine.reset()` cures it.

### The 14:00 round — 99d5c83 + 28c797a

Kevin's 170s scan with three phantom parts, and "why isn't it finding the
7-seg on 23-29/53-59?" — both answered.

**The phantoms**: the 7400's pin trios passed the hFE test twice (Vbe 0.90
and 0.44 — both impossible for real silicon; the true 2N3906 reads
0.59-0.61) → a Vbe plausibility window [0.50, 0.80] now gates every BJT
acceptance. The phantom D17 was the real PNP's E-B junction, split because
its record was gone — Kevin's 13:52 placement was eaten by my 13:57
touch-flash (deferred-save trap). New discipline: `nodes_save()` over port5
BEFORE every touch. The walk also gained the diode→BJT upgrade (third leg
from the pair's neighbors, r+2 then r-1 — the span's first hit can be a
different part).

**The 170s**: most of it was full identifyTwoLead sessions asked questions
they were overqualified for. Cluster power and star tests now read
partScanJunctionMap triples (~2s covers three pairs); the speculative
gap-2 sweep pass is cut; the done→confirm double-press is gone. The
cross-gap LED also places now (a 30-apart pair IS the axial2 footprint —
inference built "sip31" and geometry refused it).

**The 7-seg**: bench-mapped before coding. Common-ANODE, common on row 59,
dp cathode on row 29, segments 23/24/28/53/55/56/58 light from 59 at Vf
~2.2V — and rows 29/30/59/60 were refused by the whole measurement layer,
so the part's only conducting partner was unmeasurable. All my earlier
sweeps drove segments HIGH (common-cathode assumption) — polarity, not
voltage, hid it. Bench also proved ADC0/DAC0/GND all route to x-pin rows
fine → partScanBegin accepts them now, the sweep gained an evidence-gated
edge stage (six columns beside each x-pin row, mirror window when an edge
row shows hits), and the launcher groups pairs sharing one edge row:
"rows 23,24,28,53,55,56,58 all light from row 59 - a 7-seg display?".
Placement of multi-pin displays stays the Parts menu's job.

Bench note: at 5V the INA1 standing load is ~3.3mA (V/1.5k) — an earlier
sweep briefly misread that as conduction; thresholds on INA1 must be
DELTAS against V/1500, never absolutes.

### The 15:19 round — adae6df/5d525fd + 19a9eaf

The 15:19 scan was the first COMPLETE arc: junction maps → "asking harder"
fallback → clamps named → **0x3C answered** → LED21 placed across the gap
(pin: 2, not offset: 30 — offsets may never cross the ravine, any
footprint) → Q17 re-assembled by the walk's diode→BJT upgrade at Vbe 0.59V.
The junction-map cluster finder's blind spot got its fix the same hour: a
module's decoupling charges slower than the 50k pull settles, so pairs
against VDD read clamped both ways and drop out — the finder now falls
back to full identifies when the map can't name both rails.

Then Kevin's three asks (19a9eaf): a found I2C module now PLACES on
CONNECT — address → partdb record (partdbCandidatesForI2cAddr), measured
rows → offset pins, DisplayService picks it up and the panel comes alive
exactly like a hand placement (his manual SSD1306 placement went ALIVE at
0x3C through the driver minutes earlier — the scan now does that whole
sequence itself). The probe bails after six consecutive timeouts (a wedged
ordering burned 15ms × 112 addresses; a healthy bus NACKs in ~0.2ms). And
partdb grew the led_direct family: led_7seg_ca / led_7seg_cc, dip10,
5161AS/BS pinout — placeable today, driven when the GPIO_PARALLEL bus
lands (C-M3+). 14-seg waits for a real part number: a wrong pinout in the
database is worse than a gap.

Phantom cleanup: Q10/D17/Q44 removed over port5 — the stale D17 had been
shadowing the real 2N3906 ("it reads the transistor as a diode"). Board
ends the day with LED21, Q17, SSD1306_I2C — all real, all scan-verified.

### The evening rounds — be8fe81, 9a2a341, 732520f

**The star walks to its edge (be8fe81).** The 7-seg moved one column off
the x-pins and degraded to "a chip?" — the interrogation built members from
FLAGS and a display's segments never flag. partsClusterFanOut sweeps the
candidate band around a proven common (both halves, x-pins included) with
junction-map triples; many LED-drop junctions all pointing one way IS a
display. Bench: 33s scan, census saw 3 rows, verdict named all 8 cathodes
+ polarity ("common anode"). The chip path now feeds from the fan too, so
the SSD1306 arc no longer depends on which rows census-hit.

**Found parts wire themselves (9a2a341).** Per-part CONNECT prompts
("ask per part, not all or nothing"), and the display's question is
"wire?": a found display places as a record whose pins CONNECT — segments
to free GPIOs, common to TOP_RAIL/GND — so the placement machinery builds
the bridges and removal cleans them. GPIOs parked segments-off before the
bridges land; then a two-pass segment chase maps GPIO→segment by eye.

**Remove works leg by leg (732520f).** jl_remove_part_pin: tap a leg,
that leg goes (bridge, net name, record entry); the last leg takes the
part ("if we remove every node from a part... remove the part"). Rmv and
Clear are always in the Parts menu now ("a menu whose rows come and go is
a menu you can't learn"), and `x` clears parts with the board via the
shared partsClearAllRecords.

**Bench note:** the 1200-baud touch does NOT fire while the board sits in
a modal app loop (core0 never reaches secondSerialHandler) — a Ctrl-C on
port1 pops the loop and the queued touch fires immediately. The flash
procedure is now: flush → Ctrl-C port1 → touch port5 → picotool.

**5.7.8.1 prep underway:** 6-reviewer adversarial audit over today's 34
commits + full HIL suite (port1 free tonight, first full run all day).
