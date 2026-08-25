# Handoff — 2026-08-24: ambient parts, the 0.0 mA hunt, and the repo diet

## LATE-NIGHT UPDATE (22:30): the current-ants arc, BENCH-VERIFIED ✔

Kevin drove a live A/B session (multimeter in series) through four more
commits, each layer pinned by its own counter, ending with "yes it's
smoother":

- **f86b243 CONFIRMED on hardware** — taps ok climbed (69k+), noroute
  residual ~1%, currents back everywhere.
- **cb6ac6f pair coherence**: the four-capture `i!` fingerprint showed
  every flipping ant was single-ended and every pair-tapped path rock
  solid → GND became pair-eligible (nodeVoltage[GND] stays pinned;
  measured ground rides pathPairDv only), hop-asymmetric pairs refused
  (Kevin's ruling: dv only cancels what is common to both routes),
  seqPairTap = both ends back-to-back on ONE channel as universal
  fallback, retry-priority in the round robin.
- **d4a41cc selectable strategy**: debug.net_scan_pair_taps 0=off /
  1=SEQ (default, "how we had it originally") / 2=true pair. A/B result:
  both modes match the multimeter within ~1%; seq is quieter (flips:0
  vs path9:6) → seq stays default.
- **3b073e7**: ants persist through stale windows (hold cleared by
  routingGeneration, not a timer); sub-mA snapping is ANTS-ONLY now -
  netCurrentInfo carries the raw EMA (OLED shows 0.3 mA, not 0.0).
- **b0a64dc**: the residual fast-then-slow chop was the RENDER BUDGET -
  ~9 animated paths overran 250us/frame and skipped paths lurched on
  rotation return. Budget → 1200us, deltaSeconds clamped to 100ms,
  `overruns:` counter in the [ants] line. Bench: overruns:0, flips ~0,
  "smoother".

## BUG SWEEP (23:15): 17 confirmed findings, 15 fixed in 11b5a20

A 33-agent adversarial sweep (6 dimensions -> dedup -> refute-biased
verification) over the whole branch. 10 findings refuted; 17 confirmed;
15 fixed (see 11b5a20's message for the full list - highs: bg-tick GC
bound, bg root pointer across soft reboot, ant hold re-latch after
rebuild, the Wire1 setSDA panic class -> DisplayService is ALWAYS
soft-I2C until real Wire1 arbitration exists). ALL BENCH-PENDING - the
bg fixes especially want a Ctrl-D-after-bg_start test and a
place/rewire/pull-component pass on the ants.

Two confirmed-but-accepted (not fixed):
- One cosmetic stale ant frame when waitCore2 times out mid-frame during
  a refresh (rare, self-healing next frame).
- Dropped-duplicate lane-claim leak in couldntFindPath (verifier rated
  it plausible-only; would shrink the reserved K rows if real).

Watch-items left open from the session data:
- Path 28→GND read 19 mA vs ~10 on its siblings all night - if the
  meter disagrees, check that path's crosspoint-count assumption (2xp).
- One capture showed net-7 paths' ant geometry flap (staple → rows,
  ceded:10) - likely the i! continuity print tearing on core 1's
  mid-frame filledPaths rewrite; if visual chop ever returns, add a
  `relocations:` counter to convict/acquit real geometry relocation.
- HIL: zero-load displayed-mA assertions need a tolerance now (raw EMA,
  not deadband-gated 0.0).

Branch: **dev** (the old projects-guided-placement, renamed per Kevin's
cleanup ruling). Every commit builds green V5+OG. Plan of record:
`~/.claude/plans/jumperlos-codedocs-guidessimplification-velvet-bird.md`;
design status lives in `CodeDocs/guidesSimplification.md` (STATUS section).

## What this session landed (after the first bench pass)

- **38f13cd StepViewer bench manners** — Kevin's verdict on flash 1 was
  "that's a mess": wheel hijacked everywhere, VIEWER spam per detent, no
  steps on the OLED, font too big, no off-ramp. Now: the wheel is the
  viewer's only when its screen is the panel content AND the wheel has no
  other job (no menus / BLOCKING / probe mode / highlighted net); first
  turn while yielded reclaims the screen; turns are serial-silent;
  click-and-HOLD on the steps screen exits; Andale Mono 5pt 8 px layout
  (the showMultiLineSmallText recipe); the interactive open hands the
  panel back so steps actually appear.
- **2a3b1e5 + 230b759 Parts menu + picker** — "Guides" retired as a menu
  concept (projects live on via `z` + Files). Class → part → tap row for
  pin 1 → labels bloom → app exits. partdbInstantiate (connect=-1
  everywhere). `justReadProbe(true)` for taps (the Probing service is NOT
  inner-set; its cache is stale in modal loops — Menus.cpp:3027 precedent).
- **7b6c870 B-M5** — `driver:` round-trips (PartDefinition.driverKey,
  serializer+parser same commit); `partdbResolveDriver` is the ONE binding
  authority; the temporary partId-prefix resolver died; 0.91" SSD1306
  128x32 joined the seed DB (`# VERIFY` on its header order).
- **d71d995 B-M4 slice + docs** — Clear-parts picker row (CONNECT
  confirms); guidesSimplification.md STATUS; DESIGN_GUIDED_PLACEMENT.md
  marked superseded-in-part.
- **c1b754d test_ambient_parts.py** — A-M7 skeleton over the new grammar
  (PARTS/VIEWER/PARTWARN/PARTPIN/CHECK). Not yet run against hardware.
- **f86b243 the 0.0 mA fix** — see below. **BENCH-PENDING.**

## The 0.0 mA current-sensing regression — root cause and fix

Kevin's `i!` paste was decisive: `taps ok:0 noroute:112862`, every dry-run
route refused. **Not a code break**: chip K's 8 y-rows are the only fabric
gateway to ADC0-3; his netlist (rails, GND, DAC, rows 29+59 all K-resident)
plus stacking duplicates (default stackPaths 2 / slot stackRails 3)
consumed all 8, and every tier of `buildEphemeralRoute` terminates on a
free K row. Pre-existing + HIL-documented (6821eec); the ambient project
open (b1ce6fb) turned it from a corner into the steady state. Decisive
evidence: Kevin's `i!` paste plus one route-walk agent (algorithm walked
against the pasted occupancy); a separate 3-tracer + verification workflow
independently established the branch diff holds no gate mechanism.

Verification honesty: the RESERVATION half had full caller-analysis
review; the SHARED-ROW FALLBACK half is verified by compiler + reasoning
only. Its true test is exactly Kevin's saturated circuit: view currents,
then confirm the circuit still works afterward - a teardown bug there
would present as "circuit breaks after viewing currents," which would not
obviously point at f86b243.

Fix (f86b243): **(1)** ephemeral builder two-pass — virgin K rows first,
then share a row wholly owned by the tapped node's net (pre-closed hops
filtered so teardown never opens user fabric); **(2)** duplicates refuse
the last 2 virgin K rows (pair-tap headroom), with a phantom-send
mitigation (see hazard below).

**Bench verification**: flash, wire the same circuit, `i!` — taps ok
should climb and per-path mA lines appear. Deep-check: `i?` self-check
still PASS; place/remove parts and confirm routing feels unchanged
(duplicates silently drop only when K is nearly full).

**Known hazard flagged for a future pass** (pre-existing, one layer up):
`sendPath` (CH446Q.cpp:1577) filters only `y == -1`; a FAILED PRIMARY
path left x-committed with y=-2 sends `(-2<<5)&0xE0 = 0xC0` — a phantom
crosspoint at y6, bypassing the K voltage-source guard. The fix wipes
refused DUPLICATES' coords; failed primaries still carry the hazard.

Also pre-existing + inert: `yPositionUsage` is never incremented, so
`canNetUseMoreYPositions` can't do its job — the Y-limit system that
should have softened this exhaustion is dead code.

## Repo cleanup (Kevin's ruling: 2 branches, 2 worktrees)

Done: **dev** = the guides/parts line (pushed, FF from old origin/dev);
**main** fast-forwarded to origin/main; primary checkout on dev; new
worktree `~/Documents/GitHub/JumperlOS-main` on main; 13 local branches
deleted (all patch-contained in main/dev; the 4 with unique commits —
all abandoned experiments by their own messages — are preserved as local
`archive/*` tags). The serene-burnell agent worktree's uncommitted
configManager fix was verified byte-for-byte already in dev before
discarding its diff.

**Left for Kevin — `finish-git-cleanup.sh` in the repo root** (the
sandbox refused directory-deleting steps): removes the two Oct-2025
Cursor scratch worktrees, the stable-5.7.5 checkout (submodules block
`worktree remove`; content fully merged into origin/main via PR #10),
the serene-burnell worktree, and the now-redundant remote branches.
Read it, run it, delete it.

**Floating uncommitted work, deliberately untouched**: 
`lib/Jadafruit_NeoPixel/*` — an RP2350 DMA latch-timing fix (endTime
armed in canShow when the TX FIFO idles, replacing the 5 %-short
numBytes×10 µs projection) from a previous session, polished but
uncommitted. Looks bench-pending; Kevin decides.

## Bench gates queue (in the order you'll hit them)

1. Flash dev → `i!` → taps climbing, mA back (f86b243).
2. `z 555` → steps on OLED in small font; wheel scrolls steps only while
   showing; tap a net → wheel scrolls it till highlight fades; hold exits.
3. Parts menu → Logic → 74HC00 → tap row → bloom → app gone.
4. SSD1306 via Parts → Displays → DISPLAY routed → power → alive.
5. First-ever run of test_ambient_parts.py (expect harness fixes, not
   firmware verdicts - it has never executed).
6. Seed-DB `# VERIFY` editorial pass (gates B-M1 freeze).

## Software remaining

B-M3 polish (post-place move/accept), B-M5 leftover (custom labeling
CUST_T/CUST_B), B-M6 I2C auto-detect, B-M7 user /partdb/, B-M8+C-M2
Detect-Driver UX, C-M3 JDI wrap, C-M4 40RGBX160 bench characterization
(slice gate), C-M5+D-M2 MP display surface, A-M7 completion (run the new
suite, retire caveats). Plus the two pre-existing routing findings above.
