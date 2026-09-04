# "b prints two different bridge lists" (2026-09-02) — a stale terminal scroll region

Kevin, 17:08: "it's still printing 2 different bridge lists when I send b."
The paste had a full bridge/path/chip-status dump, then a backtick line, then
the tail of a second dump with different net numbers, then his `c` crossbar.

## What is proven

- The first list cannot be current output. Its header has three duplicates
  lines (`pathDuplicates`, `dacDuplicates`, `railsDuplicates`); the build on
  the board prints five (`gpioDuplicates`, `adcDuplicates` were added in
  `12e6629`). The board's flash was fingerprinted over port 5 against the dev
  `firmware.uf2` at four addresses: all match dev, none match main. So that
  list is older screen content, from this morning's build and this morning's
  netlist (rows 2+3 on ADC_1/ADC_2, the failing trio).
- The firmware prints one dump per `b`. Captured over port 7 (same printer,
  `cmd_showBridgeArray`, target = the requesting stream): one clean dump of the
  live 13-bridge netlist, 3048 bytes, no escape bytes. The second list's tail
  and the `c` crossbar in the paste match that live state (12 bridges when he
  sent `b`, `7-GP_2` added just before `c`).
- Nothing on the firmware side positions the cursor on port 1 right now:
  `ledDumpEnabled` and `liveCrossbarEnabled` read 0 from RAM (addresses from
  the ELF, read with `uctypes.bytes_at` over port 5), `top_oled.show_in_terminal
  = off` in `/config.txt`, and the `jumperless` app on port 1 (pipx 1.1.2.1,
  `bridge.py`) is a byte pass-through with no cursor or region sequences.

## What is inferred (not observable from here)

His terminal (emulator unconfirmed - the client's tty owner was not visible
from the sandbox, and the CSI s/u behaviour found later is not xterm.js's,
so probably not VS Code) is still holding a DECSTBM scrolling region from an
earlier LED-mirror (`R`, `\033[28;999r`) or live-crossbar (`c!`,
`\033[19;999r`) session. Generic VT behaviour when the region's top is not
row 1: lines scrolled out of the window are discarded, not saved to
scrollback, and the rows above the window never move.

Demonstrated in emulation (pyte, DECSTBM patched to home the cursor as real
terminals do): 27 rows of text, then `ESC[28;999r`, then the raw port-1 `b`
capture. With the reset stripped (old firmware) the header is absent and all
27 rows stay frozen - his morning symptom. With the new firmware's bytes the
region drops, the whole screen scrolls and the header is on screen. A 60-line dump therefore loses its
head, and whatever was on screen when the region was set (this morning's
dump) stays above it. The firmware-side gap that makes this sticky:

- `setLedDumpEnabled(false)` / `setLiveCrossbarEnabled(false)` send the
  `\033[r` reset, but a reboot or a reflash while the mirror is on never runs
  them (the flag just comes back false).
- `clearNonScrollingRegion()` sends the reset from `setup()`
  (`main.cpp:601`), before any host is attached, so it never reaches the
  terminal that needs it.

Discriminator for Kevin: type `reset` in that terminal (or `R` twice), then
`b` — expect one list whose header has five duplicates lines.

## The fix (uncommitted until Kevin's terminal confirms)

`dropStaleScrollRegion(Stream*)` in `Graphics.cpp`, declared in
`Graphics.h`: prints `\0337\033[r\0338` (DECSC, reset margins, DECRC — a
no-op on a clean terminal) only when neither mirror is enabled and only to
`&Jerial` / `&Serial`; port 7 and the UART never see it.

**First attempt, 17:37, was wrong.** It used the CSI forms `\033[s` /
`\033[u`. Kevin's terminal does not honour them (Terminal.app is known not
to; xterm.js does), so the margin reset homed the cursor and nothing brought
it back: the menu printed from the top of the screen over the app's connect
lines, and `b` printed over the menu (his 20:41 paste). Reproduced offline
by feeding the raw port-1 capture into pyte with a VT-faithful DECSTBM (homes
on a bare `ESC[r`; pyte has no CSI s/u handler either): identical overlay.
The same capture with the DECSC/DECRC build renders clean. DECSC/DECRC is
what ReadingDisplay already pins with on port 1, so it is proven in his
terminal. Called first
thing in `cmd_showBridgeArray` (`b`), `cmd_showNetlist` (`n`, before
`couldntFindPath(1)`, which prints ahead of the header), `cmd_showCrossbar`'s
compact path (`c`) and `cmd_showCrossbarFull` (`C`), and, best-effort, in
`SingleCharCommands::printMenu` (the menu is what a reconnected terminal
usually sees first; whether it fires before a host attaches was not traced,
so the four dump hooks are the guarantee). The HIL `_ANSI` stripper
(`\x1b\[[0-9;]*[A-Za-z]|\x1b[78]`) removes all three.

Verification done (20:50): both targets build; the V5 was touch-flashed
twice (`nodes_save(-1)` first each time; the netlist came back from the slot,
bridges re-ordered by the reload); `b` over port 7 carries no escape bytes;
Kevin freed port 1 and a raw capture there shows, for `m` and `b`, exactly
five non-colour sequences — Jerial's echo (`ESC[K`, `ESC[2G`) and
`ESC7 ESC[r ESC8` — nothing else moves the cursor. Rendered clean in pyte.
What only Kevin can confirm: one list, five-line header, on his screen.

## Known limitation, Kevin's call

While `R` or `c!` is on, a long dump still scrolls through the window and
loses its head; that is what the region does. Two ways out, both design
changes: put the image at the bottom (region top = row 1, so xterm.js and
iTerm2 keep scrollback) using a DSR size query (`\033[9999;9999H\033[6n`,
parsed like `Tui.cpp:150`), or lift the region for the dump and re-pin the
image afterwards (loses the top image-height rows of the dump on screen
unless the screen height is known).

## Separate finding on the same bench

Path 11 in the live `b` — `7 → GP_2` — has all coordinates -1: it is not
routed. Chip A's L-row (`L.y0`) is GP_1's, and the hop that would reach
`L.y7` via chip H is excluded by the `bb < 8 - saveUncommittedHops1` bound in
`resolveAltPaths` (BBtoSF) — the item noted in `K_ROW_BUDGET_2026-09-02.md`.
Not touched here.
