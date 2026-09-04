# Handoff 2026-09-02 — virtual ADC slots: planned, not started

Kevin's call at 16:27: "I don't think we should implement this today, but I
think I will in the future." Nothing from the plan is in the tree. This doc is
what a future session needs to start it cold.

## Read in this order

1. `CodeDocs/PLAN_VIRTUAL_ADC_SLOTS_2026-09-02.md` — the implementation plan.
   Three parts, each ships alone with a bench gate: **A** virtual slots with
   dedicated channels (A1-A7), **B** shared slots sampled through fresh dynamic
   routes (B1-B3), **C** the pickers go away (C1-C5). Every task names the dev
   file:line it touches (verified on dev at `a42ba8f`), the code to write, and
   a HIL check in `test/hil/test_virtual_adc.py` (a new file the plan creates).
2. `CodeDocs/adc_design_reports/0_design_note_from_chat.md` — the design note
   with the two rulings and the mechanisms verified in code.
3. `CodeDocs/adc_design_reports/1_tap_primitive_and_tdm.md`,
   `2_path_lifecycle.md`, `3_adc_site_inventory.md` — the three design-agent
   inventories the plan is built on. Report 1's recommendation is the "dynamic
   routing" Kevin asked for: shared slots use the net-voltage scan's
   `fastConnectPath` tap (route built per poll, claimed as `EPHEMERAL_PATH_NET`,
   torn down after the read), never stored-hop TDM channels.
4. Memories (auto-memory dir): `adc-virtual-slots-ruling`,
   `micropython-api-backward-compat`, plus the older
   `sense-tap-design-rulings` and `stacking-per-class-defaults` the plan leans on.

## The rulings (Kevin, 2026-09-02, both reaffirmed)

- `ADC_n` is a virtual slot; physical ADC0-3 are an internal pool nobody
  names; `ADC_7` stays the probe tip; ADC4 (0-5 V, chip L) stops being a user
  node; old slot files, `connect(5, ADC0)` and `adc_get(0)` keep working.
- Dedicated while free, multiplex under pressure: the first three slots keep a
  physical channel and a static route exactly as today; extras (or a slot
  whose path fails to route) fold onto a shared channel sampled round-robin
  with a fresh route per tap. Degrade only under real pressure.
- MicroPython compatibility (added 16:15): every spelling the module accepts
  today (`ADC1`, `ADC_1`, `ADC1_8V`, node objects, ints, strings) keeps working
  and now means the slot. Plan Task A7 is the contract with a HIL row per
  spelling. Never renumber the exported node objects (110-114); the single
  bridge funnel `JumperlessState::addConnection` remaps them.

## What today's routing work means for the plan

Two commits landed on dev before the plan was written, and the plan assumes them:

- `51db95e` — failed primaries can no longer close phantom y6 crosspoints
  (`sendPath` filters `< 0`; `couldntFindPath` wipes a failed primary's
  coordinates). A shared slot's `SENSE` path has all `-1` coordinates and is
  skipped by every stage, so it can never reach the wire either way.
- `a42ba8f` — chip-K y-row budget: a net's second K row never starves another
  net's first; a rescue pass re-runs the three routing phases with the budget
  off for whatever is still unrouted. Plan Task A4 must add `SENSE` to the
  budget's `computeKNeedingNets` skip, to `pathFullyRouted` (return true) and
  to the rescue pass's `anyUnrouted` scan — otherwise every rebuild with a
  shared slot runs the rescue pass. Both are called out in A4 step 4.

With the budget in place, a dedicated slot only fails to route when chip K is
genuinely out of rows for it; demotion (A4) turns that into a shared slot and
re-routes once so the row goes back to whoever needs it.

## Decisions still open (ask Kevin before starting)

- Execution approach: subagent-driven (fresh agent per task) or inline with
  checkpoints at the three bench gates. Kevin's subagent-economy rule favours
  inline.
- Whether measure mode should take slot 15 by name or keep using the pool
  directly under `INFRA_ADC_MEASURE` (the plan keeps the pool; slot 15 is
  reserved but unused).
- `chooseADC()` in Probing.cpp becomes dead code in Part C; delete it there or
  in a later sweep.

## Bench notes from today (state the board was left in)

- The board runs the dev build from `a42ba8f` (fingerprint-verified). Kevin's
  slot 0 from before the flash was restored and then cleared by his own probe
  "clear nodes" pass; at 16:27 the live netlist held only the probe feed.
- The undo history contains four of my test transactions (connect/disconnect
  ADC_4-28, connect/disconnect GP_7-D1); an undo replayed one of them once.
- The `b` printout losing its head: the printer's bytes are clean (captured
  over port 7: 2956 bytes, no escapes). The only scroll-region setters are the
  LED terminal mirror (`R`, Graphics.cpp:4504, `\033[15;999r`) and the live
  crossbar view (`c!`, CH446Q.cpp:1314). A region whose top is not row 1
  discards scrolled-out lines, so a 55-line dump in a ~45-row region loses its
  head. Discriminator: `R` twice or `c!` twice. Possible firmware fix, Kevin's
  call: long dumps (`b`, `n`, `c`) reset the region, print, re-pin the image.

## How to verify when the time comes

- Build: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og`
  (system pio is Python-3.14-gated; recreate the venv, ~1 min).
- Flash: flush with `jumperless.nodes_save(-1)` over port 5, 1200-baud touch
  on port 5, `picotool load -x <uf2 by absolute path>`.
- Bench with Kevin's client on port 1: port 5 `test/hil/jl.py: jl_exec`, port 7
  `test/hil/port7.py` (`N` netlist JSON, `b` path table, `V` ADC volts).
- Regression: `test/hil/test_routing.py`, `test_net_currents.py`, and the new
  `test_virtual_adc.py` from the plan.
