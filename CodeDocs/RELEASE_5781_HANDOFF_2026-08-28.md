# 5.7.8.1 release-hardening pass — overnight 2026-08-27→28

Kevin's brief at 22:35: "spend all night checking every bit of code and
making sure everything is rock solid for release." This doc is the pass
record and the morning hand-back.

## What shipped tonight (commits, in order)

- **732520f** — leg-by-leg removal (jl_remove_part_pin), always-visible
  Rmv/Clear, `x` clears parts. **This commit also accidentally deleted
  the display wire-up + per-part confirm work from 9a2a341** (edit
  collision when the previous session process exited mid-flight).
- **cd20423** — the restoration (wire-up + per-part confirms spliced
  back verbatim from 9a2a341) + nine verified audit findings fixed:
  - `x` saved the slot BEFORE removing parts → parts resurrected on
    reboot. Reordered; **bench-proven end to end** (place → x →
    "Cleared all connections and 1 part" → reboot → still 0).
  - Parts > Remove truncated pin names to 7 chars and resolved pins by
    name (two-GND parts removed the wrong leg) → 12-char buffer +
    rowHint disambiguation.
  - Part focus survived table compaction as a raw index →
    highlightingInvalidatePartFocus() called from every removal path.
  - Scan re-placement refused ("LED21 already exists") → scan placers
    replace their OWN artifact (partId == deterministic name) first.
  - clearLiveSerialLine had no CDC FIFO guard (core-0 stall on wedged
    host) → guarded like its siblings.
  - Latched serialNeedsRepin pushed a full OLED I2C frame every loop
    pass → serial-only repaint path added.
  - partsTapForRow's duplicate-row branch printed a bogus second error.
  - `x` paid for two full fabric rebuilds → partsClearAllRecords(false).
- **15464ea** — the 2026-08-26 PENDING_VERDICTS list re-verified against
  the current tree: #40 #20 #2 #3 #5 #9 #14 #34 and #6's sprintf site
  were ALREADY fixed by intervening work. The two live ones died:
  - **#21 [high]**: the eight PWM functions had no OG gate - on OG the
    pins-20-27 bank is CH446Q chip selects / RESETPIN / WS2812 / ADC.
    All eight now refuse with -1 on OG.
  - **#6 residual**: two probe display sites strcpy'd 13-byte node names
    into char[12]; widened to 24/52 matching the fixed sibling.

## Verification

- **HIL run 1 (on 732520f): PASS 9/10** (test_encoder_ui SKIP - no
  OpenOCD session; the MSC round-trip SKIP inside slot_files is the
  known needs-host-mount manual case). test_parts_roundtrip 181 checks,
  test_projects 275, test_slot_files 96.
- **HIL run 2 (on cd20423): running at write time** - final verdict
  appended below.
- Bench (port5 MP + port1): x-clears-parts + reboot persistence proven;
  remove_part_pin machinery exercised via the parts suite.
- The audit fleet lost 14/17 agents to a usage cliff; the three
  surviving reviewers' 11 findings were each re-verified BY HAND against
  the tree before fixing (one refuted-by-inspection: none - all real,
  one work-listed).

## Deliberately NOT fixed (work-list)

- remove_part/-pin delete a user's own bridge when it coincides with a
  part pin's declared connect (needs a per-pin "expansion added this
  bridge" flag - design decision, not a night fix).
- The 2026-08-26 pending list's needs-bench/behavior-change items (#16
  #17 #18 #19 #22 #23 #27 #28 #29 #31 #36 #38 #43 #45 #46 #47 #49 #13
  #24 #25 #26 #30 #33 #35 #39 #41 #42 #50 + mixed #32 #44 #51): all
  pre-existing at 5.7.6/5.8.0, none regressions from today. The list
  lives in PENDING_VERDICTS_2026-08-26.md with per-item fix shapes.

## Morning notes for Kevin

- **VERSION**: you asked for 5.7.8.1; the VERSION file said 5.8.0 and a
  5.8.0 tag exists from 2026-08-26. I set 5.7.8.1 as asked - if you
  meant 5.8.1, it's a one-line change before tagging.
- Your two scan-artifact records (from the evening bench) got cleared
  during x-testing - the physical parts are untouched; one Auto Scan
  re-finds them.
- The wire-up prompt ("wire? 8-seg display...") has bench-proven
  geometry and placement machinery, but the full prompt flow needs your
  probe press - one Auto Scan + CONNECT is the last mile.
- Never pushed; everything is local commits on dev.
