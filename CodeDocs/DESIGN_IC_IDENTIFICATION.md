# IC Identification — consolidated handoff (2026-08-28)

**Purpose.** One self-contained brief for a NEW session whose job is: when the
Auto Scan finds an unnamed chip, figure out WHAT it is. Consolidates and
supersedes-as-index (originals stay authoritative for their own depth):

| Source | What it holds | Read when |
|---|---|---|
| `DESIGN_PART_ID_FOLLOWUP.md` | The landed Layer 0/1 design + §15 bench-measured hardware facts (loop R, E9, junction bands, 2N3906 ground truth) | touching PartMeasure/PartClassify |
| `REF_COMPONENT_TESTER_RESEARCH.md` | AVR TransistorTester tree with verbatim thresholds; I2C ident etiquette + WHO_AM_I tables (§6); whole-board scan theory (§8); V-I signatures (§8.7); safety rules (§7) | designing any new measurement |
| `PriorArtComponentID.md` | ICT guarding math, group testing bounds, CircuitSense/Toastboard, "don't invert the Laplacian" | multi-node topology questions |
| `FIXCHECK_2026-08-26.md` / `LATENCY_AUDIT_2026-08-26.md` | verification + latency discipline and ledger format | writing this session's ledgers |

Kevin's ask (2026-08-28): *"We should allow users to assign a chip when we find
a generic IC like that"* (LANDED — see §2.6) *"and write a doc for a new
session to identify ICs like that."* The identification itself is the new
session's work.

---

## 1. Vocabulary: the four tiers of "identify"

- **Tier 0 — presence + shape.** "A dipN chip occupies rows X-Y / (X-30)-(Y-30),
  its GND row is G and its VDD row is V." **LANDED** (see §2).
- **Tier 1 — unpowered fingerprint.** Clamp-diode topology + per-pin Vf + which
  pins are open-collector. Safe (no power applied), narrows family and pin-1
  corner. **LANDED 2026-08-28** — `partScanClampFingerprint` + partdb
  `fingerprint:` + orientation-aware matcher; bench truth in
  `FIXCHECK_2026-08-28_IC_IDENTIFY.md`.
- **Tier 2 — powered, passive.** Current-limited power-up, quiescent current
  (TTL mA vs CMOS µA), bus probes (I2C **LANDED** for modules; SPI/UART not).
- **Tier 3 — powered, active vectors.** Drive inputs, read outputs, match truth
  tables against footprint-matched partdb candidates. This is what NAMES a
  7447. **LANDED 2026-08-28** — `PartDbVectorSet` + `partsVectorIdentify`
  named the bench 7447 (`7447(r):pass, 74595(r):fail@1`), surfaces in the
  scan confirm pass + Parts > Test; HIL `test_ic_identify.py` PASS.
- **Tier 4 — exotic.** Huntron V-I per-pin signatures, LED-as-photodiode. Later.

---

## 2. Where the tree is TODAY (everything below is landed and flashed)

The Auto Scan pipeline (`src/PartsApp.cpp` `partsAutoLauncher`, all stages
bench-verified 2026-08-28 on Kevin's board: 7447 + 2N3906 + 7-seg + wires):

1. **Census** (`partScanCensus`, `src/sensing/PartMeasure.cpp`): per-row
   charge-share poke on the scan ADC, ~0.6 s for 56 rows. Physics ceiling: TTL
   inputs and open-collector outputs read EMPTY (the 7447 censused 4 of 8
   bottom pins dark — this is expected, not a bug).
2. **Pair sweep** (`partScanPairSweep`): 3 V through the shunt, both
   directions; current read as **sink-row voltage on the scan ADC**
   (`kSweepDevV` = 10 mV ≈ 0.15 mA through the ~67 Ω sink path) — the ants'
   Ohm's-law idea, replacing two 25-50 ms INA waits per direction. Median
   self-calibration per direction. Adjacent + cross-gap + evidence-gated edge
   stages.
3. **Span interrogation**: splits discretes out of spans (`identifyTwoLead`
   walks), reads cluster clamps (`partsFindClusterPower` — junction maps over
   triples, "asking harder" fallback = full identifies), probes I2C on
   clusters whose rails were named (`partsProbeClusterI2C`, powered
   current-limited, power pins last).
4. **Chip pairing + second look**: a bottom-half "a chip?" span adopts its
   whole far-side span (gap-1 tolerant), then every still-dark row inside the
   union — plus one column past each edge — is asked directly:
   `partsChipMemberProbe(row, gndRow, vddRow)` drives supply-pin↔candidate on
   the identify fixture (`partScanServo` reached-0.5 mA = conducts). Different
   physics than the poke; sees the pins the poke can't. Needs the rails named;
   when clamps stay unclear the union stands unprobed.
5. **Confirm pass**: per-finding yes/no (`partsConfirmYesNo` gesture set:
   CONNECT/click = yes, REMOVE/hold = no). A confirmed chip opens the
   **assignment picker** (§2.6).

**Timing** after the 2026-08-28 speedups: full scan on that board 110 s → 35 s
before the second look; INA-paced stages run at the hardware conversion
cadence during sessions (`inaFastPollMode`, `Peripherals.cpp` — poll gate
50 ms → ~9 ms, bus averaging 16 → 1 samples scan-scoped, CNVR cleared per read
so freshness is honest). A settled INA read is now ~15-20 ms; budget with that.

### 2.1 Supporting facts the new session inherits

- **Rotated DIPs**: `baseRow`'s half encodes orientation (bottom = dot
  bottom-left, top = rotated 180, pin 1 top-right; `PartPlacement.cpp
  nodeForPin`, exact reverse permutation). Two-tap placement (pin 1 + far
  corner) validates it. HIL: `test_parts_roundtrip.py` U7R case.
- **Generic chip records**: `partsPlaceFoundChip` places `IC<row>`, dipN, all
  legs listed (`P1..PN`), no connects. Real parts: persist, label, Remove.
- **partdb**: 114 records (`src/partdb/PartDbData.h`), `PartDbPinout` has
  footprint + physical pinCount + per-pin roles (`PARTDB_ROLE_VCC` drives the
  power auto-route in `partsCommitPlacement`). `PartDbI2cIdent` = 8 rows.
  `partdbCandidatesForI2cAddr()` exists. **There is no vectors/truth-table
  field anywhere yet.**
- **Cluster rails**: `ClusterPower {gndRow, vddRow, sig[6]}` from
  `partsFindClusterPower` — junction-map anode/cathode voting, full-identify
  fallback. This is the anchor every powered tier builds on.
- **Session machinery**: `ScanSession` + `partScanBegin/End` (lift user wiring,
  claim ADCs, roving GPIO, powered-row refusal, every-exit restore funnel).
  `partScanServo/Resistance/Hfe/FetProbe/JunctionMap/CapDetect` primitives.
  MP bindings: `part_identify(r1,r2[,r3])`, `place_part`, `list_parts`,
  `remove_part`.

### 2.6 Assignment picker (landed 2026-08-28, this closes the manual path)

After confirming "add dipN chip", the user scrolls a `partsPicker` list:
**Generic IC** (first stop, click-click keeps the fast path) + every partdb
record whose pinout is `PARTDB_FOOT_DIP` with that exact pinCount
(`partdbPlaceableHere`-gated). Picking a record prompts a **pin-1 tap**
(Kevin, 2026-08-28: the scan can't know which corner pin 1 is — assuming the
anchor placed a 7447 upside down); the tap's half encodes the orientation
and `partsCommitPlacement` places the record there — pin names, roles, power
auto-route, drivers, everything a hand placement gets, `partGeometryOk`
refusing impossible fits. The generic stop keeps the found anchor (its
P-numbers are positions, not pins — no wrong way up). Hold = skip the
finding; serial byte = end the pass.

**Known gap (CLOSED 2026-08-28)**: assignment happens only at find time. ~~A
placed `IC33` cannot be re-assigned later~~ — Parts > Test on a generic
`IC*` DIP now finds the rails, fingerprints, runs vectors, and offers the
winner through the same commit path (`partsTestLauncher`, the "this is
actually a ___" action).

---

## 3. What identification needs that the tree does not have

| Capability | Status (2026-08-28) | The gap |
|---|---|---|
| Chip presence, footprint, rails | LANDED | — |
| Manual naming (picker) | LANDED | — |
| Re-assign after placement | LANDED | UI gestures need eyes |
| Unpowered clamp fingerprint | LANDED | 9 seed fingerprints; grow with the bench |
| Quiescent-current signature | INA-in-feed measures it during vectors | nobody RECORDS it as a signature yet |
| I2C WHO_AM_I depth | schema widened (whoami2 + probe_order), 10 ident rows | the probe doesn't read WHO_AM_I registers at scan time yet |
| Truth-table vectors | LANDED (8 seed sets) | records without vectors can't auto-name |
| 5 V logic levels | 3.3 V GPIO drive proven for TTL + HC@3V3 (two-pass supply) | FakeGpio 5 V outputs are hard-disabled — HC@5V drive still out of reach |
| Chip power for vectors | DECIDED + landed, see §5.2 | — |

---

## 4. Prior-art digest — only what bears on ICs

(Full detail in the two research docs; these are the load-bearing facts.)

- **Unpowered chips are diode meshes.** Every real pin clamps to the supply
  pins through ESD structures; signal↔signal pairs read EMPTY and prove
  nothing (`REF` §6.5, bench-confirmed — it's why `partsChipMemberProbe`
  anchors on the rails). The clamp MAP is a fingerprint: which pins clamp to
  GND, which to VDD, which both, which neither (open-collector outputs — the
  7447's a-g!), plus per-pin Vf. TTL multi-emitter inputs vs CMOS gate
  protection read differently.
- **Stimulus below turn-on keeps chips invisible; above it, visible.** ICT
  shorts tests run 0.1 V for exactly this reason (`PriorArt` §1); the sweep's
  3 V deliberately crosses it. An identification pass can use both regimes.
- **CircuitSense (UIST '17)** classified ten 8-pin ICs at 100 % from
  unpowered transient signatures + a Random Forest, probing only the four
  opposite-row pairs (1,8),(2,7),(3,6),(4,5) — a pooling shortcut worth
  stealing for the fingerprint pass (`PriorArt` §6, `REF` §8.6).
- **I2C identification etiquette** (`REF` §6): scan 0x08-0x77, zero-data write
  probes only, never 0x00; WHO_AM_I conventions per family (ST 0x0F, Bosch
  0xD0/0x00, InvenSense 0x75, TI 0xFE/0xFF); module-level address-set
  correlation is the cheapest disambiguator. All directly extendable via
  `PartDbI2cIdent`.
- **The AVR tester's discipline** (`REF` §1-2, §7): every drive is
  set→dwell→measure→release with an unconditional release path; discharge
  before believing anything; classify by ratios/invariance, not absolutes;
  stop permuting the moment the picture is complete.
- **Safety rules already proven here** (`partsProbeClusterI2C`): power pins
  connected LAST, current-limited first power-up, INA watchdog, full teardown
  on every exit. The vector runner inherits this shape wholesale.

---

## 5. The plan for the new session

### 5.1 Tier 1 — clamp fingerprint (cheap, do first) — LANDED 2026-08-28

> As-built deltas: ONE servo at 1 mA per direction (the 0.05 mA ratio pass
> lives inside the fabric's transient floor and painted phantom paths —
> FIXCHECK_2026-08-28_IC_IDENTIFY.md); two 2-row sessions per pin, never
> 3-row (the roving-GPIO claim starves on GPIO-wired boards); rail feeds
> on the chip's supply rows are lifted for the whole run (an unlifted feed
> kept the chip powered and every reading was chip-internal garbage). The
> partdb field is a per-pin char string (G/V/B/N/T/-/?/C), matched with a
> 180°-aware compare. ~15 s for a dip16.

For a found chip with rails named: per pin, record
`(clampToGnd: none|fwd Vf, clampToVdd: none|fwd Vf)` using the existing
`partScanServo` membership fixture at two currents (the diode-vs-resistor
ratio law). ~0.2 s/pin at the fast INA cadence → ~3 s for a dip16. Match
against candidates sharing the footprint:

- corner rails (GND 8 / VCC 16 on a dip16) ⇒ classic 74xx family;
- pins with NO high-side clamp ⇒ open-collector outputs (7447: 7 of them —
  that alone separates it from a 74HC595 in the same package);
- all-symmetric CMOS clamps ⇒ HC/HCT-class.

Store the expected fingerprint per record — new optional partdb field
(generator: `scripts/generate_partdb.py`; keep the round-trip law: parser and
serializer land together).

### 5.2 Tier 3 — the vector runner (the naming step) — LANDED 2026-08-28

Schema per `DESIGN_PART_ID_FOLLOWUP.md` §9 (`vectors:` on partdb records or
FatFS `/partdb/` files). Runner shape:

1. Candidates = records matching footprint + rail corners + Tier-1
   fingerprint. Usually ≤ 3.
2. Power the chip. **DECIDED on the bench, 2026-08-28** (Kevin's 7447,
   correctly oriented after the pin-1-on-row-10 find):
   - **(a) TOP_RAIL as supply: WORKS and is the TTL path.** With the rail
     at 5 V through the crossbar, the 7447's VCC pin held 4.05-4.43 V under
     its own load and the chip demonstrably operated (outputs followed
     DCBA). Marginal vs the 4.75 V spec but functional; 74HC/HCT happier
     still. `TOP_RAIL -> ISENSE_PLUS`, `ISENSE_MINUS -> vccRow` routes fine
     and INA0 reads the feed (watchdog + Icc signature in one fixture).
   - **(b) DAC0 at 3.3 V stays the CMOS path** (the I2C-probe precedent).
   - **(c) When the rail isn't available and candidates are TTL-only,
     refuse and say so** — or enable the rail for the run behind the same
     confirm gesture, restoring after.
   - **NEW mandatory pre-check (the copper lesson):** physical jumper wires
     are invisible to state lifts — on the bench the demo's copper fed the
     chip AROUND the freshly built shunt fixture (INA read ~0 mA on a
     clearly live part; that near-zero-with-powered-pins reading IS the
     detector). After GND-first hookup, read the VCC pin BEFORE feeding:
     already high ⇒ **board-powered mode** (drive/read vectors against the
     user's own supply, never fight it); cold ⇒ build our feed. Power pins
     last, current-limited first touch, INA watchdog armed, teardown on
     every exit — unchanged.
   Bench facts for the runner: GPIO 3.3 V drive toggles TTL inputs through
   the fabric cleanly (row reads 0.095 V low / 3.3 V high); ADC legs read
   output levels; `gpio_set_dir(pin, True)` = OUTPUT (booleans, not ints -
   int 1 lands as INPUT).
3. Drive inputs: 3.3 V GPIOs suffice for TTL (Vih 2.0 V) and for 74HC@3V3;
   5 V CMOS inputs need FakeGpio 5 V outputs (nodes 150-157) — verify on the
   bench before trusting.
4. Read outputs on ADC legs (TTL high ~3.4 V reads fine); open-collector
   outputs need a pull-up leg (roving GPIO pull, already in the session kit).
5. Match: all vectors of exactly one candidate pass ⇒ name it, place ITS
   record (the §2.6 picker's commit path, preselected). Several pass ⇒ offer
   the picker filtered to survivors. None ⇒ keep the generic record.
6. Surfaces: (a) the scan's confirm pass — "identify?" escalation after a
   chip finding; (b) Parts > Test on a placed generic `IC*` record — same
   runner, and the natural home for **re-assignment**.

Seed vectors: 7400/7402/7404/7408/7432/7447/7486 + 74HC595 + NE555 (astable
sanity check is its own shape) — authored from datasheets (facts aren't
copyrightable; the GPL AVR-tester source stays unread, `DESIGN` §13).

### 5.3 Order of work — ALL LANDED 2026-08-28

1. ~~Tier-1 fingerprint collector + partdb field + matcher~~ (benched first,
   as prescribed — the bench rewrote the measurement, see §5.1).
2. ~~Power-delivery decision for TTL~~ (§5.2, decided).
3. ~~Vector schema + runner + 8-10 seed records~~ (8 sets).
4. ~~Picker preselection + part-card re-assign action~~ (survivor-filtered
   picker with tapless commit; Parts > Test re-assign).
5. ~~`PartDbI2cIdent` widening~~ (whoami2 + probe_order + sorted candidates;
   REF §6.3 register reads at scan time remain future work).
6. ~~HIL~~ (`test/hil/test_ic_identify.py`, PASS 23 checks — discovers the
   bench 7447 from its placed record instead of hardcoding rows).

### 5.4 Honest ceilings to carry forward

- A chip whose census+membership missed its true END columns still undersizes
  the dipN (union can't see silent columns past a silent edge); Tier-1
  fingerprints on the assigned record catch the mismatch loudly.
- Phantom pairings (top-half SIP module over an unrelated bottom cluster)
  survive to the confirm prompt; the row ranges in the prompt are the defense.
- E9 pad leak (`DESIGN` §15) still applies to every parked mid-rail node:
  input buffers OFF on session pins.
- The scan cannot and should not distinguish parts the physics can't: report
  "74HC00-class" when vectors genuinely tie, never guess.
