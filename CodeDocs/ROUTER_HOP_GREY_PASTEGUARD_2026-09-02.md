# 2026-09-02 evening — 7→GP_2 unroutable, black net in the terminal, paste flood

Three asks from Kevin at 22:23, after the scroll-region fix landed (`fa3fb1b`).

## 1. "Can't find a path" for 7 → GP_2

**Netlist (live on his bench):** rows 2-5 on ADC_0-3, 6-GP_1, 7-GP_2, 8-GP_3,
rails, GND, ADC_3-D0, and the probe feed BUF_IN-GP_8. Path 11 (`7 → GP_2`)
came back with every coordinate -1.

**Fabric facts** (KiCad netlist, `jumperless-v5r7-hardware` memory): row 7 is
A.Y7; GP_2 is L.X5; chip A's only lane into L is A.X13 → L.Y0, and L.Y0 was
GP_1's (row 6, net 10). A hop through another breadboard chip X uses X's
NC Y0 row as a bounce: A.X(lane to X) → X.X(lane from A) —X.Y0— X.X(lane to
L) → L.Y(X). Free L rows were Y3 (D), Y4 (E), Y7 (H); D's and E's bounce
rows were already carrying nets 7 and 8 (the ADC hops), B's and G's L rows
were GP_3's and GND's. H was the only chip that worked: A.X14 (/AH0) → H.X0
→ H.Y0 → H.X3 (HL) → L.Y7 → L.X5.

**Root cause:** `resolveAltPaths` BBtoSF walked the bounce chips with
`for (bb = 0; bb < 8 - chipStates[chip[1]].uncommittedHops; bb++)`.
`uncommittedHops` is incremented by `ijklPaths` for every SF-to-SF path that
borrows a Y row on that chip (the probe feed BUF_IN→GP_8 borrows L.Y2), so
with the feed present the loop stopped at G and never tried H. The row the
SF-to-SF path actually took is already refused by `freeOrSameNetY(sfChip,
bb, …)` inside the loop; the bound bought nothing. The OG router has always
looped to 8. Fix: `bb < 8`, the two unused `saveUncommittedHops*` locals
removed. Expected result on the bench: path 11 routed with chip2 = H, y1 = 7
(L.Y7); the `c` crossbar's H column gains crosspoints at (X0,Y0) and
(X3,Y0), A gains (X14,Y7), L gains (X5,Y7).

## 2. Unassigned net colours print black

`b`'s chip status printed net 13 (BUF_IN→GP_8) as `\033[38;5;0m` — black.
`assignTermColor` maps `colorToVT100(packRgb(netColors[i]))`, and a net whose
LED colour was never assigned (packed RGB 0) lands on index 0. New
`netTermColorIndex(i)` in `NetManager.cpp` returns 244 (xterm grey50) for a
zero colour and the mapped index otherwise; used by `assignTermColor` and the
three direct sites in `listNets` (`n`). Kevin's words: "let's just make
unassigned colors show as grey in the terminal." Expected on the bench:
`\033[38;5;244m13` in `b`.

## 3. A pasted block ran every byte as a command, then the board hung

Kevin pasted a KiCad symbol block into the app in char mode. Each `(` printed
the whole undo log, `)` the PSRAM status, the letters ran whatever they
spelled (`i` persists config), and after "(hi" the USB device vanished
("Device not configured" on every later write). At 22:40 the board was not
enumerated in any mode (no JLV5 ports, not BOOTSEL) — a core-0 hang with
USB dead, not a HardFault (a fault reboots through the watchdog and prints a
`[crashlog]`; there is no hardware watchdog to catch a hang). It needs a
power cycle. The hang itself is a separate item; the guard reduces exposure
but does not explain it.

**Guard (main.cpp, char mode only — `useLineBuffering == false`):**

- *Interrupt:* a bare Esc, or three Enters in a row, discards whatever is
  queued and prints one line (`[Esc: N queued bytes cleared]`). Esc is armed
  for 30 ms first so an arrow key's `ESC [ A` (three separate 1-byte USB
  writes from the app) is swallowed as a sequence rather than treated as the
  interrupt. Enter events count `\r\n` as one. `m` already drained a >20-byte
  backlog in `printMenu`.
- *Flood limiter:* a backlog of ≥ 24 bytes behind a char that does not read
  a body from the stream (`W Q f + - > { [` do), or 8 command chars inside
  500 ms, enters a drain state: the busy loop keeps servicing USB (the CDC
  FIFO refills between passes), everything queued is eaten until the port has
  been quiet for 300 ms, then one line reports the total
  (`[input flood: dropped N bytes …]`).
- *Honest limit:* the limiter bounds the damage, it cannot block it. Any rule
  that lets a short run of commands through ("bncRV") lets the first few
  bytes of a paste through too, and those are live commands. Line mode,
  relayed input and the body-taking handlers (Wokwi push, `f`, the S/L
  prompts, which block on their own prompt) are untouched.
- *Exact solution, not done:* bracketed paste (`ESC[?2004h`) gives hard
  start/end markers so the size is known before acting. It is another
  terminal-mode toggle (today's scroll-region lesson) and the markers would
  reach the body readers unless stripped at the read layer.

## Verification (bench, 22:46-23:05)

Kevin put the board in BOOTSEL (it had hung with USB dead; no `[crashlog]`
on the first menu afterwards, so it was a hang, not a fault) and the build
went on with `picotool load -x`; the netlist came back from the slot. Later
builds went on with the 1200-baud touch after `nodes_save(-1)`.

- **Route:** `b` over port 7 shows path 11 as `7 A x14 y7 -> GP_2 L x5 y7,
  via H x0 y0 / x3 y0` - the hand-traced route. The `c` crossbar gained
  (H.X0,Y0), (H.X3,Y0), (A.X14,Y7), (L.X5,Y7). `test_routing.py`: see below.
- **Grey:** on port 1 `b` prints net 13 as `38;5;244m`; no `38;5;0m`
  anywhere. A port-7 `b` shows no colour escapes at all, before or after
  this change: `changeTerminalColor` writes through Jerial's default stream,
  not the response target, so the backchannel's chip status is uncoloured
  regardless (measurement artifact, not a failure; pre-existing quirk).
- **Paste guard** (port 1 raw, `\x0f` first so the firmware is in char mode
  like the app, `\x0e` after): a 936-byte KiCad block sent byte-by-byte ran
  ONE command (the first `(` printed the undo log once) and then
  `[input flood: dropped 935 bytes …]`; the same block as one write ran
  nothing and dropped 936. `b` afterwards works. Junk followed by a bare Esc:
  the limiter had already caught the junk (34 dropped). `ESC [ A` produced
  no output. Enter x3 with nothing queued is silent; with 12 `(` queued
  behind it: `[Enter x3: 12 queued bytes cleared]`, no undo prints. `n` and
  `b` sent in one write both ran.
- **Config the paste changed** (captured at the first boot after the hang):
  `[measurement] net_currents = 1` - the `i` in "(hi" toggled the net current
  scan and persisted it; its earlier value is not known here. `[terminal]
  colors = true` and `line_buffering = true` were untouched.
- Board stayed on USB for the 90 s watch after the first flash and through
  everything above.

## Bench incident during verification

`test_routing.py` run standalone (PASS, 5 checks) has no state restore - only
`run_all.py` wraps the suites with capture/restore. It left the live netlist
at just the probe feed and saved that over `/slots/slot0.yaml` (`nets:`
empty; parts 4051@31 and 74393@41, rails 3.80/2.70 V, DACs 2.50/0.60 V and
the GPIO config survived). Restored from the `b` captures by writing the
twelve bridges back into the slot file over port 5 (numeric ids, no `dup:`
so the per-class defaults apply) and `machine.reset()`; the undo log was not
touched. After the reset: 13 bridges, nothing unrouted, `7 → GP_2` via H.
