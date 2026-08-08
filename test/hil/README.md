# Jumperless HIL regression suite

Hardware-in-the-loop tests that drive a real Jumperless V5 over USB. Run
before releases; each file also runs standalone.

```bash
pip install pyserial
python3 test/hil/run_all.py
```

## Requirements

- A Jumperless V5 connected over USB, sitting at the main terminal (not
  inside an app or menu), with **nothing plugged into the breadboard**.
- The [jumperless-v5 skill](https://github.com/Architeuthis-Flux) installed at
  `~/.cursor/skills/jumperless-v5/` (or `~/.claude/skills/...`), or point
  `JUMPERLESS_PY` at its `scripts/jumperless.py`. It provides the MicroPython
  raw-REPL transport on CDC port 5; the suite talks to CDC port 1 directly
  for single-char commands (`i!`, `i?`).
- `test_encoder_ui.py` additionally needs a Raspberry Pi Debug Probe with
  OpenOCD on `localhost:4444` and the jumperless-swd-input skill
  (`JL_INPUT` overrides the `jl_input.py` path). It auto-skips otherwise.

If the board is not connected the suite fails fast with one message — it
never retries or troubleshoots connections.

## What each file covers

| File | Covers |
|---|---|
| `test_micropython_fs.py` | REPL exec, gc headroom, `jfs` write/read/delete, OLED calls |
| `test_routing.py` | DAC→row→ADC routing, disconnect, **RouteSafety driven-pair regression** (GPIO+GND and GPIO loopback nets must route) |
| `test_net_currents.py` | Net voltage scan estimate vs INA0 on a real DAC→ISENSE→row→GND loop, zero-load deadband, `i?` self-check + audit |
| `test_config.py` | New config keys present, runtime toggle persists through the idle flush |
| `test_stress.py` | 40 connect/disconnect cycles with the scanner running: REPL stays responsive, no PIO timeouts, no live short |
| `test_encoder_ui.py` | SWD-injected click menu open/scroll/close (skips without a debug probe) |

Manual pre-release checks the suite cannot do (need a human hand):
probe tip taps near row-band edges (variance gate + duplicate-walk tuning),
switch position detection in both positions, and probe connect/clear flows.
