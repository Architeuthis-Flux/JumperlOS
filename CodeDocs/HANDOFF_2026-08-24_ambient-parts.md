# Handoff — 2026-08-24: ambient parts, the 0.0 mA hunt, and the repo diet

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
open (b1ce6fb) turned it from a corner into the steady state. Confirmed
independently by a 10-agent workflow (branch diff holds NO gate mechanism)
and a route-walk agent (algorithm walked against the pasted occupancy).

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
5. `python3 test/hil/run_all.py` incl. the new test_ambient_parts.py.
6. Seed-DB `# VERIFY` editorial pass (gates B-M1 freeze).

## Software remaining

B-M3 polish (post-place move/accept), B-M5 leftover (custom labeling
CUST_T/CUST_B), B-M6 I2C auto-detect, B-M7 user /partdb/, B-M8+C-M2
Detect-Driver UX, C-M3 JDI wrap, C-M4 40RGBX160 bench characterization
(slice gate), C-M5+D-M2 MP display surface, A-M7 completion (run the new
suite, retire caveats). Plus the two pre-existing routing findings above.
