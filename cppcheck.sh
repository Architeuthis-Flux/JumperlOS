#!/bin/bash
# cppcheck script for JumperlOS
# Detects unused functions, variables, and other code quality issues

set -e

cd "$(dirname "$0")"

echo "Running cppcheck to find unused functions and code issues..."
echo "This may take a minute..."
echo ""

cppcheck \
  --enable=warning,style,performance,portability,unusedFunction \
  --std=c++11 \
  --platform=unix32 \
  --quiet \
  --template='{file}:{line}: {severity}: {message} [{id}]' \
  --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression \
  --suppress=useStlAlgorithm \
  --suppress=constParameter \
  --suppress=constVariable \
  -DARDUINO=10607 \
  -DARDUINO_RASPBERRY_PI_PICO \
  -DBOARD_NAME=\"jumperless_v5\" \
  -DF_CPU=150000000L \
  -DUSE_TINYUSB \
  -DPICO_RP2350A=0 \
  -D__not_in_flash_func\(x\)=x \
  -D__not_in_flash\(x\)=x \
  -D__attribute__\(x\)= \
  -D__packed= \
  -Iinclude \
  -Isrc \
  -Isrc/routing \
  -Ilib \
  -Iboards/jumperless_v5 \
  -I.pio/libdeps/jumperless_v5/*/src \
  src/ \
  include/ \
  lib/ \
  2>&1 | grep -E "(unused|never used|not used)" || echo "No unused functions detected by cppcheck"

echo ""
echo "Scan complete!"
