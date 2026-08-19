# SWD live-state probes (used to catch the config-save storm, 2026-08-13)

Non-intrusive debugging helpers that read the cross-core concurrency flags
over the Raspberry Pi Debug Probe while the firmware runs. Used with the
`jumperless_v5_debug` env and an OpenOCD server:

```
OOCD=~/.platformio/packages/tool-openocd-rp2040-earlephilhower
$OOCD/bin/openocd -s $OOCD/share/openocd/scripts \
  -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg
```

- `sample_state.py [--pcs]` - one-shot dump of core1busy/core2busy/
  core1FrameHoldDepth (the frame hold that replaced pauseCore2, T3.4;
  printed `core0+core1` when nonzero)/readingADC/chipSelect/timeout counters;
  `--pcs` briefly halts each core for PC/LR (symbolize with
  arm-none-eabi-addr2line against the debug ELF).
- `tap_session.py [secs]` - watch lastReadRaw (raw probe-ladder value) +
  configChanged/filesystemActive/frame-hold transitions at ~20Hz while
  tapping pads. This is the trace that caught taps triggering config-save
  storms (V0 re-anchor -> configChanged -> multiple full config.txt writes
  per second while probing).
- `stress_flash.py [iters]` - interleaves slot saves / jfs writes / config
  toggles with flag sampling between ops.

Addresses: all three scripts resolve their symbols from the ELF with
`arm-none-eabi-nm` at start - `JL_ELF` overrides the path, otherwise the
release `jumperless_v5` ELF is used, then the debug one. Make sure that is
the build actually flashed.

Known nit: the `switchPosition` symbol is a C++ reference, so sampling its
address yields a pointer, not the value - ignore that field.
