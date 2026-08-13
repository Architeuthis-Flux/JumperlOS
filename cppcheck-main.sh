#!/bin/bash
# cppcheck script for JumperlOS - focused on main source only
# Detects unused functions, variables, and other code quality issues

set -e

cd "$(dirname "$0")"

echo "Running cppcheck on main source code..."
echo "This will take several minutes with exhaustive checking..."
echo ""

cppcheck \
  --enable=warning,style,performance,portability,unusedFunction \
  --check-level=exhaustive \
  --std=c++11 \
  --platform=unix32 \
  --inline-suppr \
  --template='{file}:{line}: {severity}: {message} [{id}]' \
  --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression \
  --suppress=useStlAlgorithm \
  --suppress=constParameter \
  --suppress=constVariable \
  --suppress=preprocessorErrorDirective \
  --suppress=unknownMacro \
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
  -DPROGMEM= \
  -Isrc \
  -Isrc/routing \
  src/*.cpp src/*.c src/routing/*.cpp \
  2>&1 | grep -vE "^Checking |files checked|% done" | tee cppcheck_results.txt

echo ""
echo "========================================"
echo "Summary of issues found:"
echo "========================================"
echo -n "Unused functions: "
grep -c "unusedFunction" cppcheck_results.txt || echo "0"
echo -n "Unused variables: "
grep -c "unreadVariable" cppcheck_results.txt || echo "0"
echo -n "Total issues: "
grep -cE "style:|warning:|performance:" cppcheck_results.txt || echo "0"
echo ""
echo "Full results saved to: cppcheck_results.txt"
echo ""
echo "To see only unused functions:"
echo "  grep unusedFunction cppcheck_results.txt"
echo ""
echo "To see only unused variables:"
echo "  grep 'unreadVariable\|unusedVariable' cppcheck_results.txt"
