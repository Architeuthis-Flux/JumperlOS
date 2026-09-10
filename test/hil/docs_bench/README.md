# docs_bench - drive the V5 without hands so agents can verify the docs

Used 2026-09-07/08 to walk every Getting Started, GPIO and Examples section
on a real V5 (5.7.11.0) with fresh agents and screenshots. Ledger of what
they found: CodeDocs/DOCS_BENCH_LEDGER_2026-09-08.md.

- `bench.py --help` - the whole tool: terminal (CR only), MicroPython exec,
  simulated probe taps (`probe_tap`), switch position, OLED as text/PNG,
  board PNG from the port-7 `:leds` dump, logo/rails from the terminal's
  `R!` dump, `run <path>` + `stop` (bare Ctrl+Q then bare Ctrl+C), `reset`.
- `snapshot.py` / `restore.py` - the bench invariant: live state YAML plus
  device-side `.hilbak` copies of slots, config, undo history and project
  run files; restore reboots and fingerprint-diffs.
- `compile_ledger.py` - turns the agents' JSON reports (+ the workflow's
  recheck verdicts) into one markdown ledger.

Limits on 5.7.11.0: no wheel or probe-button simulation survives the Python
hand-back, so menus and probe modes need a hand (or SWD on the V5); rails,
logo and pad LEDs are not in `:leds`; a script that gets any byte parked in
front of its Ctrl+Q wedges the board (see the handoff), hence the CR-only
terminal and the run/stop discipline.

## Restoring the bench after the 2026-09-07/08 run

`snapshot_latest.json` is the bench snapshot taken 2026-09-07 23:03 (file
bytes and hashes of config.txt, the slot files, undo history and the 555 run
file, plus the live-state fingerprint in `../kevin_state_2026-09-07_2303.yaml`);
`device_files/` is the same content as plain files for reading. With the
board answering and ports 1 and 5 free:

    python3 test/hil/docs_bench/restore.py

Read the last lines: `VERIFIED: live state matches the snapshot`, the
`:json:power` line (top 4.4 V, bottom 7.4 V), `post-reboot checks: CLEAN`
with the list of stray files it removed, and `JL Audio In interfaces after
reboot: 0`. Anything else, report it rather than forcing.

Two things learned on 2026-09-09 that the scripts now handle, but read them
before trusting a second restore: `jl.run_file_restore()` works from the
`.hilbak` copies the snapshot left on the device and deletes them when it
succeeds, so only the FIRST restore has them; later runs fall back to the
host copies in `device_files/` (the capture appended one newline to each,
the scripts strip it and check the FNV-1a hash). And leaving a slot (`<3`,
`<0`) schedules a save of the slot you left that lands a few seconds later,
so the scripts switch first, wait, and only then write that slot's file.
