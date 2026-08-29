Prior art: automatic discovery of an unknown component network across many nodes
Sources were fetched and text-extracted directly (several PDFs needed manual Flate decompression). Where PDF extraction mangled Ω/µ I note the corrected value only when a figure caption or second occurrence confirms it.

1. ICT "learn mode", guarding, and 3/4/6-wire guarded measurement
The core in-circuit metrology is a transimpedance amplifier, not a voltmeter. Per Electronic Design, "Principles of Analog In-Circuit Testing": a voltage source V_S drives one end of the DUT; the other end sits on an op-amp summing junction held at virtual ground, and Z_X is computed from the ratio of V_S to the voltage across the feedback impedance Z_F. With open-loop gain 2×10⁶ V/V and ±10 V output swing, the summing node deviates only ±5 µV from ground — that ±5 µV is the entire justification for calling it "virtual." This is the single most transferable idea for a crossbar breadboard: the sense node must be actively held, not passively read, because a held node is immune to everything else attached to it.

Guarding = collapse the parallel path onto a low-impedance driver. For a delta A–B–C where you want R_AB, node C is driven to the potential of the virtual-ground end. Parallel current through R_AC then returns to the guard driver, not to the current meter. The published 3-wire error expression is:


Rx_calc = R_X + R_S + R_M + (R_G · R_A)/(R_B + R_G)
where R_S = source lead resistance, R_M = measure lead resistance, R_G = guard wire resistance, R_A and R_B the guarded branches on the source and measure sides. The residual term (R_G·R_A)/(R_B+R_G) is the finite-guard-impedance error — it vanishes only as R_G → 0. Adding a 4th wire (remote guard sense at the board) lets the op-amp's non-inverting input servo out R_G; 6-wire adds remote sense on both the feedback and guard legs, leaving only finite loop gain and input offset. Reported error curves in that article, at R_S = R_M = 0.8 Ω, R_G = 0.4 Ω, R_A = R_B = 50 Ω: 3-wire ≈1–2 % at 200 Ω degrading at both extremes, 4-wire ≈0.5–1 %, 6-wire local <0.5 %, 6-wire remote essentially error-free. The wire designations are S (source), I/M (measure), G (guard), L/R (local/remote sense).

How many nodes get guarded: about four. US4774455, "Method for automatic guard selection in automatic test equipment" describes the actual learn-mode algorithm on a known-good board: identify every node touching the component, apply a guard to each in turn, measure, retain the ~4 guards yielding the greatest improvement, then test those in combination — and penalize multi-guard combinations by 1 % per extra guard so the program generator doesn't burn ATE resources for marginal gain. Note the architectural point: guard is a bus (the 3070's G bus, with L as its remote sense), so the count is not a hardware limit — any number of nodes can be muxed onto the guard bus; four is what the search heuristic keeps because improvement saturates.

Concrete instrument numbers, from the Keysight i3070 Series 6 datasheet:

Parameter	Value
Max nodes (E9903G / E9902G)	5184 / 2592
Shorts test	source impedance 100 Ω, 0.1 V DC, threshold 2 Ω–1000 Ω, resolution 1.0 Ω, accuracy ±(0.25 % + 2.2 Ω) as printed
Shorts settling time	default 50 µs, programmable 0 → 3.2768 s in 50 µs steps
R unguarded	0.1–10 Ω 4-wire ±1.5 %; 300 Ω–10 kΩ 4-wire ±0.25 %; 1–10 MΩ 2-wire ±5.0 %; + system residual 3.5 Ω
R guarded ("High Guard Ratio")	10 kΩ 6-wire, 1.0 V DC, guard ratio 1000:1 on both legs → ±2.5 %; 100 kΩ 6-wire, 0.1 V DC, guard ratio 1,000,000:1 → ±1.0 % typ
C	10 pF–0.5 µF 2-wire ±2.0 % (+1 pF residual with compensation, 0…+40 pF without); guarded 1000 pF 6-wire @ 1024 Hz, guard ratio 1000:1 → ±6.0 %
L	5 µH–50 mH 4-wire ±2.0 % (+1 µH residual); guarded 10 mH 6-wire @ 8192 Hz → ±5.0 %
Diode / Zener	default 1 mA, programmable to 100 mA; ±(1.0 % rdg + 4 mV), residual 3.5 mV/mA; HV zener 18–60 V ±3.0 %
Beta	emitter bias 100 µA–100 mA, β 10–1000, ±15 %
Two things to steal: guard ratio is the design figure of merit (ratio of guarded shunt to DUT — 1000:1 buys ~2.5 %, 10⁶:1 buys ~1 %), and AC stimulus at 0.1 V is chosen specifically to stay below silicon junction turn-on so unpowered ICs stay off. The ED article adds the model-selection rule: use parallel capacitance model below 10 nF, series above 1–10 µF; and use a single digitizer multiplexed between the DUT voltage and the feedback voltage so gain errors cancel mathematically, with a single-bin DFT to kill DC offset at zero added test time. That last trick is directly implementable on an RP2350 ADC.

Flying probe (Takaya class) uses guarding for the same reason at the pin level — to suppress stray capacitive coupling between adjacent leads during capacitive open-pin detection, where an unconnected pin adds a small series air-gap capacitance to the lead-frame-to-plate capacitor (overview).

2. IEEE 1149.4 mixed-signal test bus
Architecture (from the working group's own ITC 2010 slide deck, text-extracted): each analog pin gets an ABM (Analog Boundary Module) in place of a digital boundary cell. The ABM contains switches SB1, SB2 (pin → internal buses AB1, AB2), SD (core disconnect), and SH / SL / SG (pin → V_H, V_L, V_G for digital-style drive and for the comparator threshold V_TH). Its boundary-register cells are C, D, B1, B2 — C is the enable, D supplies the logic level (V_H/V_L), B1/B2 close the AB1/AB2 switches; C = D = 0 disconnects the pin from the core (EDN, "Extensions to the IEEE 1149.1 boundary-scan standard"). The TBIC (Test Bus Interface Circuit) gates the two internal buses AB1/AB2 to the two external pins AT1/AT2 of the ATAP, and carries its own SH/SL/SG plus a comparator; its BSDL statement exposes control pairs D1a/D1b and D2a/D2b.

The measurement: AB1/AT1 is the current-force path, AB2/AT2 the voltage-sense path. To get an interconnect's R or C between chip A's pin and chip B's pin, you close SB1 in chip A's ABM and SB2 in chip B's ABM, force I into AT1 from external ATE, sense V at AT2, and compute Z = V/I. Because forcing and sensing use separate buses that terminate at separate ABM switches, the bus and switch resistances sit outside the sense loop — this is Kelvin sensing built into the standard, and it's the structural reason two buses exist rather than one. In CMOS the two buses can be electrically identical, so you can alternatively monitor two pins simultaneously.

Does it work in practice? Not well. The Lancaster/TRW DATE'05 paper (arXiv:0710.4826) states plainly that "measurement accuracy is limited due to capacitive and resistive characteristics of the Analogue Boundary Modules that also cause degradation of the observed signal," and that limitations in passive R/C measurement via 1149.4 have been identified. The adoption story is in one fact from that same paper: to build a 1149.4 demonstrator on a real automotive ECU they could not buy compliant silicon and instead emulated compliance with National Semiconductor STA400 dual analog multiplexers bolted onto ordinary IC pins.

Status and successors. IEEE 1149.4-2010 was made Inactive-Reserved on 2021-03-25, then revived: IEEE 1149.4-2024 was approved 2024-09-26 and published 2024-12-06. In board test, what actually replaced it was not another analog bus but capacitive vectorless opens testing — i3070's Vectorless Test EP: 200 mV source, 8192 Hz, programmable capacitance thresholds from 0.5 fF to 750 pF with 0.5 fF–2 fF programming resolution — plus IEEE 1149.6 for AC-coupled nets. For embedded analog instruments, the successor lineage is IEEE 1687 (IJTAG) with P1687.2 specifically extending IJTAG to describe analog IP and analog networks (Siemens/Tessent, Corelis).

3. Huntron Tracker / V-I signature analysis
All numbers from the Huntron ASA Training Course Workbook, P/N 21-1376 Rev. A (text-extracted; the workbook renders Ω in Symbol font, which extracts as "W").

Source parameters. A Tracker range is exactly three numbers — V (peak sine amplitude), R (internal source resistance), F (frequency):

R: 10, 50, 100, 1 k, 5 k, 10 k, 50 k, 100 kΩ (8 ranges)
V: 200 mV, 3, 5, 10, 15, 20 V peak (6 ranges)
F: 20 Hz – 2000 Hz (5000 Hz on ProTrack / Tracker 4000 / Model 30)
The STAR interlock table limits V×R combinations so available current stays under 200 mA. Reading the table as a rule: 200 mV is legal on every R; 3–10 V forbid 10 Ω; 15 V forbids 10/50/100 Ω; 20 V permits only 50 kΩ and 100 kΩ. For a firmware stimulus design that's a ready-made safety envelope: V_peak / R_source ≤ 200 mA.

Classification rules, as published. The horizontal axis is applied voltage, the vertical axis is current through the internal R_L; a 0 Ω external path gives no horizontal component (pure vertical = short), an infinite path gives no vertical component (pure horizontal = open). Every real signature is a composite of four primitives:

Resistor — straight line at 0–90°; invariant under changes of F and V, changes only with R_source. That invariance is itself the discriminator.
Capacitor — circle/ellipse; becomes more vertical as F increases (X_C falls).
Inductor — circle/ellipse, possibly with resistive tilt from winding DCR; becomes more horizontal as F increases (X_L rises). Also nearly invariant to V changes.
Semiconductor — "two or more linear line segments that most of the time form an approximate right angle." Conduction in one direction = diode; conduction in both directions with two knees = zener.
Transistor — model it as two junctions: collector–base reads as a plain diode signature, base–emitter reads as a zener signature, and the emitter–collector reverse breakdown equals the base–emitter reverse breakdown.
Quantitative readout from the graticule. The display has 4 divisions per side, so volts/division = V_range/4. Worked example from the workbook: at the 3 V_pk range, 3/4 = 0.75 V/div, and a silicon diode knee "slightly before the first division" reads as 0.6–0.7 V_pk forward drop. Same construction gives zener V_Z directly from the reverse knee position. Resistance follows from the line angle, since the vertical axis is I through the known internal R and the horizontal is V — a 45° line means R_DUT ≈ R_source, which is why the workbook's tuning rule is: start at 100 Ω or 1 kΩ, step up a range if the trace is near-horizontal, down if near-vertical, and aim for 45°. That "aim for 45°" is autoranging for impedance in one sentence.

Comparison workflow. Huntron Workstation stores per-range Max Samples / Tolerance / Delay and a per-sequence Compare Priority of Same (same serial), All, or Merge (merged min/max envelope signatures) — i.e. known-good learning is an envelope built from multiple good boards, not a single golden trace (Workstation tutorial). Multi-channel scanners exist at 64 and 128 pins (Access DH manual).

4. Efficient adjacency discovery on N nodes
For N = 60 breadboard rows, naive pairwise is C(60,2) = 1770 two-terminal measurements. The accelerations, in the order you'd actually apply them:

(a) Presence prune first — O(N). Before any pairwise work, ask each row "is anything attached at all?" Charge-transfer / relaxation measurement of a single node's self-capacitance runs entirely on GPIO with no external analog parts: precharge C_x to a known V, share charge into a filter cap through a passive impedance, repeat until an accumulated threshold is crossed, and count cycles — the count is the capacitance (Microchip/Quantum charge-transfer patents, US7449895 / US7902842). Sigma-delta variants quantize the same thing. Industrially the same idea is the vectorless open test: 200 mV, 8192 Hz, 0.5 fF resolution. Empty breadboard rows differ from occupied ones by picofarads, and pruning k occupied rows out of 60 cuts the pairwise set from 1770 to C(k,2) — for k = 12 that's 66, a 27× reduction before you measure a single resistance. TDR is not the tool here: a breadboard row is ~5 cm, so a round trip is ≈0.5 ns, requiring >10 GHz edge rates to resolve — the capacitance question answers the same "is anything there" for free.

(b) One-drives-all-sense — O(N) configurations. The wire-harness industry's standard scan is exactly your problem. US20130162262A1, "System and Method for Automated Testing of an Electric Cable Harness": resistively pull every pin high, then pull one pin low at a time and read back the state of all pins in parallel. Each of the N steps yields a full N-bit row of the connectivity matrix, so a 16-pin harness produces a 16×16 matrix in 16 steps. Zeros in a row are that pin's net-mates; row weight tells you net size directly (the patent's worked matrix shows rows with one zero = isolated pin, two zeros = one partner, three zeros = a three-way net). This is O(N) in wall time only because there are N parallel receivers. With k parallel measure lines instead of N, the cost is N·⌈(N−1)/k⌉ configurations — for N = 60, k = 4 that's 60×15 = 900, vs 1770 naive; with k = 8, 480.

(c) Pooling / group testing — the crossbar makes this native. A crossbar can short an arbitrary set of rows onto one measure line, and the resulting measurement is a Boolean OR (or a conductance sum) over the set. That is literally a group test. Dorfman pooling (expected tests per item = 1/s + 1 − (1−p)^s, optimal s ≈ 1/√p giving ≈2√p) applies when connectivity is sparse: a breadboard with ~10 components on 60 rows has p ≈ 10/1770 ≈ 0.006, so optimal pool size s ≈ 13 and expected cost ≈ 2√p ≈ 0.15 tests per pair — a ~6.6× reduction over exhaustive pairwise. For the adaptive version, Hwang's Generalized Binary Splitting finds d defectives among n candidates in ≈ d·log₂(n/d) + O(d) tests, degenerating to ⌊log₂ n⌋ + 1 when d = 1 (Du & Hwang, Combinatorial Group Testing and Its Applications; modern treatment).

Concrete algorithm for a 60-row crossbar with k measure lines:


1. Presence scan:  for each row i, charge-transfer C_i          → O(N),  keep occupied set S, |S| = k_occ
2. For each i in S:
     a. drive row i with V through R_source
     b. bisection over S\{i}: short half the set onto measure line,
        read current. If |I| < threshold, discard that half.
        Recurse on positive halves.                              → ~d_i · log2(|S|) pooled measurements
3. For each discovered pair (i,j): guarded 2-terminal characterization
   (drive i, TIA on j, all other rows in S driven to 0 V as guards)
4. Classify (i,j) from the V-I trajectory (Huntron rules, §3)
Step 2 costs O(N · d · log₂ N) rather than O(N²) — for N = 60 with average degree d = 2, that's roughly 60 × 2 × 6 ≈ 720 pooled reads at worst, and far fewer after the presence prune. Crucially, step 3's guarding is what makes step 2's results trustworthy: without holding the non-participating rows, every measurement is a delta-network reading, not a component reading.

(d) Thresholds. Cirris publishes the numbers the harness industry actually ships with (Guidelines for Setting Resistance Test Thresholds): continuity pass = stringent 1.05·R_actual + 0.5 Ω, good = max(2 Ω, 1.10·R_actual + 1 Ω), moderate = max(5 Ω, 1.20·R_actual + 2 Ω); low-voltage isolation = 5 MΩ stringent / 500 kΩ good / 100 kΩ moderate. IPC/WHMA-A-620 Rev A specifies 2 Ω, or 1 Ω plus actual resistance, for Class 3 in the absence of an agreed spec. Typical tester resolution: ±1 % / 0.1 Ω two-wire, ±2 % / 0.001 Ω four-wire. Two thresholds, not one — a low one for paths through components and a high one for pure-isolation checks — is the pattern worth copying.

(e) Theoretical ceiling. What's recoverable at all from boundary measurements is settled: for circular planar resistor networks, Curtis–Ingerman–Morrow and Colin de Verdière–Gitler–Vertigan give a combinatorial criterion for recoverability from the Dirichlet-to-Neumann map, and if the underlying graph is known and critical, the conductances are uniquely determined (arXiv:1203.4045; Borcea et al.). For the harder joint topology+value problem from limited measurements, see arXiv:2412.02315, which relates each Thevenin/effective-resistance measurement to the unknown Laplacian, yielding a multivariate polynomial system solved under triangle-inequality, Kalmanson, and connectedness constraints — but their worked example is only 4 boundary + 2 interior nodes, and they note the maximal planar candidate graph has 3n_b − 6 edges and grows nearly exponentially. Practical conclusion: don't try to invert a Laplacian for a breadboard. Isolate with guarding and measure components one at a time.

5. Guarding math for a three-node delta
(Derived here; cross-checked against the ICT and Keithley formulas below.)

Nodes A, B, C with unknown R_AB, R_BC, R_AC. Drive A with source V through negligible source impedance; hold B at virtual ground with a transimpedance amp measuring I_B; drive C with a guard amplifier commanded to 0 V.

Ideal case (V_C = 0 exactly): both R_BC and R_AC have 0 V across their B-side/return, so


I_B = V / R_AB          →  R_AB = V / I_B   exactly
Current V/R_AC still flows, but it flows into the guard driver, not into the meter. This is the whole trick: the parallel path is not removed, it is redirected.

Finite guard output impedance Z_g: the guard sinks I_C ≈ V/R_AC, so it sits at


V_C ≈ Z_g · V / R_AC
which injects an error current into the summing node:


I_err = V_C / R_BC = Z_g·V / (R_AC · R_BC)
Fractional error:


ε = I_err / I_ideal = [Z_g·V/(R_AC·R_BC)] / (V/R_AB)
  = Z_g · R_AB / (R_AC · R_BC)
Three design consequences, all directly actionable: error scales linearly with guard output impedance, linearly with the DUT resistance you're trying to measure, and inversely with the product of the two shunt legs. Defining the guard ratio as G = √(R_AC·R_BC)/R_AB, ε ≈ Z_g/(G²·R_AB). Reading the i3070 table backwards through this: guard ratio 1000:1 on both legs at 10 kΩ giving ±2.5 % implies an effective Z_g in the tens of ohms — consistent with a real relay-plus-amp guard path.

This has the same shape as the ICT article's published residual (R_G·R_A)/(R_B+R_G) — both say guard series impedance times the source-side shunt, divided by the measure-side shunt. Two independent derivations agreeing is the reason to trust it.

Kelvin/remote-sense fix: add a sense wire from the guard amp's feedback to node C at the board, which servos Z_g down by the amp's loop gain — this is exactly the 4-wire→6-wire progression in §1.

The precision-metrology form of the same theorem, from the Keithley Low Level Measurements Handbook, 7th ed. (§2.2.1, "Shunt Resistance Loading and Guarding"). Unguarded, a shunt leakage R_L simply divides:


V_M = V_S · R_L / (R_S + R_L)
Guarded — driving the shield/guard from a unity-gain buffer with open-loop gain A_GUARD:


V_M = V_S · (A_GUARD · R_L) / (R_S + A_GUARD · R_L),    A_GUARD = 10⁴ … 10⁶
The leakage resistance is multiplied by the guard amplifier's open-loop gain. Keithley's worked example: R_S = 10 GΩ, R_L = 100 GΩ, V_S = 10 V. Unguarded error ≈ 9 %; with A_GUARD = 10⁵, V_M = 9.99999 V, error <0.001 % — a factor of ~10⁵ improvement from one buffer. The handbook's definition is worth memorizing verbatim: a guard is a low-impedance point at nearly the same potential as the high-impedance node being guarded. Guarding also collapses settling time, since the cable/stray capacitance is bootstrapped out: their Figure 2-10 shows a measurement that hadn't settled after 12+ s unguarded settling in ~2 s guarded, and elsewhere an unguarded case still unsettled at 70 s. For a firmware scanner chasing response time, guarding is a latency optimization as much as an accuracy one.

Related three-terminal technique: the electrometer's Guarded Ohms mode surrounds the HI input node with a unity-gain guard, neutralizing input cable capacitance C_S and making >10 GΩ measurements fast rather than glacial (Keithley §1.5, Figure 1-19).

6. Automatic breadboard / circuit discovery projects
CircuitSense (UIST '17, NTU HCI Lab) — paper PDF, DOI — is the closest existing prior art to automatic component discovery on a breadboard, and it splits the problem exactly the way you'd want to:

Pin location sensing (passive): a strain gauge on every spring clip — 5 gauges per clip, 3 front / 2 back, 4.0 × 2.4 mm to match a standard clip. Inserting a lead bends the clip and shifts gauge resistance, producing 0.3–0.5 mV across a Wheatstone bridge; an AD8228 instrumentation amp at gain 100 feeds a 12-bit ADC, scanned through a two-stage cascade of 16:1 ADG1606 muxes driven by a Cortex-M4. This answers "where are the pins and how many" mechanically, without any electrical stimulus — the presence-detection role that §4(a) fills capacitively on a crossbar.
Component recognition (active probing): a 50 Hz square wave (period 0.02 s), amplitude starting at 500 mV and adaptively raised to 2000 mV for components with a turn-on threshold. 50 Hz was chosen as the explicit tradeoff between detection latency and the range of detectable R/C/L. For each component they probe all 2-combinations of its pins, driving one pin, grounding another, and recording at every pin: #waves = #pins · C(#pins, 2) — 2 waves for a 2-pin part, 9 for 3-pin. For 8-pin ICs, C(8,2)=28 is too slow, so they probe only the four opposite-row pairs (1,8), (2,7), (3,6), (4,5) — a hand-designed pooling shortcut worth noting.
Value extraction, in closed form. Resistance from the divider against known mux and generator output resistances: V_out/V_in = (R_x + R_m)/(R_x + R_w + 2R_m). Capacitance from the log-linearized RC decay — log V(t) − log V₀ = −t/RC, fit the slope (they use linear programming for noise rejection) and divide out the known R. Inductance identically from log V(t) − log(V₂−V₁) = −tR/L.
Classifier: 2000 samples at 75 kHz per wave; features = time- and frequency-domain mean/peak/median/variance/std plus the cepstrum; a separate Random Forest per pin count.
Results: 22 component types at 100 % accuracy (wires, R, C, L, 1N4001, beepers, LM317/LM337, TIP31C/32C/120/122, and ten 8-pin ICs including NE555, LM358, MCP3002, 24LC256, MSGEQ7). Measured ranges: R 50–1000 Ω at ≤5 % error; C 1–100 µF at ≤15 %; L 0.01–1 H — all three ceilings set by ADC resolution and sample rate, not by the method. Diode/beeper polarity changes the waveform, so both polarities were added to the training set.
The honest read: CircuitSense proves type classification from a low-voltage transient is tractable with a small feature set and a Random Forest, and that the binding constraints are ADC speed and stimulus frequency — both of which an RP2350 with a real DAC and a crossbar improves on. Its weakness is that it needs a strain gauge per hole, which a crossbar replaces with pure electrical presence detection.

Toastboard (UIST '16, Berkeley) — PDF — connects all 48 rows of a standard breadboard (backing removed, bonded to a PCB with Z-axis conductive tape) through a two-stage mux cascade to one ADC on a CC3200, sampling rows round-robin, with LED bars (red = power, orange = ground, green = other). Important distinction: Toastboard measures voltages only and does not extract topology — the user still draws the virtual circuit, and the software cross-checks it ("if a wire has been drawn in software between two rows but they are measured to not have equal voltages, the user will be alerted to a possible bad connection"). Its stated design goals are nonetheless the right requirements list for your case: sense presence of power/ground, sense when components have been connected, measure many circuit nodes at once, visualize, and lower the expertise barrier with automatic analysis.

Adjacent work in the same line: CurrentViz (UIST '17) senses and visualizes current flow rather than voltage; Bifröst (UIST '17) correlates code execution against bus/electrical activity; Scanalog (UIST '17) exposes FPAA internal state and emits Fritzing schematics; VirtualComponent (CHI '19) uses plug-in component banks plus a breadboard that manages connections and component values in software — the closest architectural cousin to a crossbar breadboard. On the patent side, US4714875 "Printed circuit board fault location system" describes bidirectional switching arrays that subject components to AC at multiple frequencies for "impedance profiling" of inductances and series-parallel networks — a 1987 statement of frequency-swept discrimination on a switch matrix.

What I'd take into firmware, in priority order
Hold the sense node at virtual ground (TIA), don't passively read it. ±5 µV of virtual-ground error buys immunity to everything else on the node.
Guard every non-participating row to the sense potential, not to a floating/high-Z state. Error is ε ≈ Z_g·R_AB/(R_AC·R_BC) — drive impedance is the whole game, and guarding also cuts settling time by 5–30× (Keithley).
Presence-scan capacitively first (O(N), fF-class resolution is industry-standard), then run pairwise only over occupied rows: 1770 → C(k,2).
Pool aggressively — shorting a row set onto one measure line is a native Boolean OR; bisection gives each node's neighbors in ~log₂60 ≈ 6 reads, so O(N·d·log N) not O(N²).
Classify by invariance, not by shape matching. Sweep frequency: a straight line that doesn't move is resistive; an ellipse that verticalizes is capacitive; one that horizontalizes is inductive; two linear segments meeting at a knee is a junction, two knees is a zener, and a transistor is diode-on-one-junction + zener-on-the-other.
Autorange to 45° against the source resistance (Huntron's rule), and cap stimulus at V/R ≤ 200 mA (STAR) with amplitude low enough (0.1 V AC, per ICT practice) to keep unpowered junctions off — then step up adaptively to 0.5→2 V only when the node reads open, as CircuitSense does.
Learn a known-good envelope from multiple passes (merged min/max), not a single golden trace, and use two thresholds (component-path vs isolation), as both Huntron and Cirris do.
Sources
Principles of Analog In-Circuit Testing — Electronic Design
Keysight i3070 Series 6 In-Circuit Test System datasheet
US4774455 — Automatic guard selection in ATE
IEEE 1149.4 Standard for a Mixed Signal Test Bus (ITC 2010 slides)
Extensions to the IEEE 1149.1 boundary-scan standard — EDN
IEEE 1149.4-2010 status (Inactive-Reserved) · IEEE 1149.4-2024 (Active)
Jeffrey et al., 1149.4 in a safety-critical automotive ECU (arXiv:0710.4826)
Huntron ASA Training Course Workbook P/N 21-1376 Rev. A · Huntron Workstation tutorial · Huntron Access DH manual
US20130162262A1 — Automated testing of an electric cable harness
Cirris — Guidelines for Setting Resistance Test Thresholds
Keithley Low Level Measurements Handbook, 7th ed.
Dorfman pooling optimal pool size · Binary splitting group testing · Du & Hwang, Combinatorial Group Testing
Charge-transfer capacitance sensing, US7449895
Circular planar resistor networks (arXiv:1203.4045) · Borcea et al., circular resistor networks for EIT · Topology reconstruction from limited boundary measurements (arXiv:2412.02315)
CircuitSense (UIST '17) · Toastboard (UIST '16) · VirtualComponent
US4714875 — PCB fault location system (impedance profiling)
No files were written. Working extractions live only in the session scratchpad.

https://www.electronicdesign.com/home/article/21202923/principles-of-analog-in-circuit-testing
https://docs.alltest.net/manual/Alltest-Agilent-Keysight-E9902G-Datasheet-29440-.pdf
https://patents.google.com/patent/US4774455A/en
https://www.edn.com/extensions-to-the-ieee-1149-1-boundary-scan-standard/
https://assets.testequity.com/te1/Documents/pdf/keithley/KeithleyLowLevelHandbook_7Ed.pdf
https://www.edn.com/extensions-to-the-ieee-1149-1-boundary-scan-standard/
