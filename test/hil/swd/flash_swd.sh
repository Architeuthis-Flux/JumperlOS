#!/usr/bin/env bash
# Flash the Jumperless V5 over the CMSIS-DAP debug probe - the recovery path
# when the board's own USB is gone - WITHOUT the two traps that bit us:
#
#  1. OpenOCD's reset on RP2350 does not stop peripherals. If the USB mic was
#     streaming, its ADC DMA keeps writing samples into RAM while OpenOCD
#     streams the image through its RAM work area, and you get "Verify Failed"
#     with ADC-sample-looking bytes in flash. So: stop the ADC and abort every
#     DMA channel after `reset halt`, before programming. Order matters - stop
#     the DREQ source first, or an aborted channel can be re-triggered.
#     NOTE the abort register is 0x50000464 on RP2350; 0x50000444 is the RP2040
#     offset and lands on DMA_TIMER1 here, which silently aborts nothing.
#  2. Stopping OpenOCD without `shutdown` can leave the core's reset vector-catch
#     armed, after which even a picotool/BOOTSEL reboot halts silently in ROM.
#     This script always ends with a clean `reset run; shutdown`.
#
# Usage: test/hil/swd/flash_swd.sh [path/to/firmware.elf]   (default: debug env ELF)
set -euo pipefail
ELF="${1:-.pio/build/jumperless_v5_debug/firmware.elf}"
OOCD="${OOCD:-$HOME/.platformio/packages/tool-openocd-rp2040-earlephilhower}"
exec "$OOCD/bin/openocd" -s "$OOCD/share/openocd/scripts" \
  -f interface/cmsis-dap.cfg -c "adapter speed 2000" -f target/rp2350.cfg \
  -c "init; reset halt; mww 0x400a0000 0; mww 0x50000464 0xffff; sleep 20; mdw 0x50000464; program $ELF verify; reset run; shutdown"
