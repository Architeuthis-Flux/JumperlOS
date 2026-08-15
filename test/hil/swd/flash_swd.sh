#!/usr/bin/env bash
# Flash the Jumperless V5 over the CMSIS-DAP debug probe - the recovery path
# when the board's own USB is gone - WITHOUT the two traps that bit us:
#
#  1. OpenOCD's reset on RP2350 does not stop peripherals. If the USB mic was
#     streaming, its ADC DMA keeps writing samples into RAM while OpenOCD
#     streams the image through its RAM work area, and you get "Verify Failed"
#     with ADC-sample-looking bytes in flash. So: abort every DMA channel and
#     stop the ADC after `reset halt`, before programming.
#  2. Killing OpenOCD without `shutdown` can leave the core's reset vector-catch
#     armed, after which even a picotool/BOOTSEL reboot halts silently in ROM.
#     This script always ends with a clean `reset run; shutdown`.
#
# Usage: test/hil/swd/flash_swd.sh [path/to/firmware.elf]   (default: debug env ELF)
set -euo pipefail
ELF="${1:-.pio/build/jumperless_v5_debug/firmware.elf}"
OOCD="${OOCD:-$HOME/.platformio/packages/tool-openocd-rp2040-earlephilhower}"
exec "$OOCD/bin/openocd" -s "$OOCD/share/openocd/scripts" \
  -f interface/cmsis-dap.cfg -c "adapter speed 2000" -f target/rp2350.cfg \
  -c "init; reset halt; mww 0x50000444 0xffff; mww 0x400a0000 0; sleep 20; program $ELF verify; reset run; shutdown"
