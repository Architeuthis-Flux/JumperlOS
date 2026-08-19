#!/bin/zsh
# Regenerate jl_input.py's per-build ADDR table from the freshly built ELF.
set -e
cd /Users/kevinsanto/Documents/GitHub/JumperlOS
NM=$(ls ~/.platformio/packages/toolchain-rp2040-earlephilhower/bin/arm-none-eabi-nm | head -1)
ELF=.pio/build/jumperless_v5/firmware.elf
JLI=~/.cursor/skills/jumperless-swd-input/scripts/jl_input.py
EBS=$($NM -C $ELF | awk '$3=="encoderButtonState"{print $1}')
EDS=$($NM -C $ELF | awk '$3=="encoderDirectionState"{print $1}')
LBS=$($NM -C $ELF | awk '$3=="lastButtonEncoderState"{print $1}')
EDC=$($NM -C $ELF | awk '$3=="encoderDirectionConsumed"{print $1}')
BET=$($NM -C $ELF | awk '$3=="buttonEventTimestamp"{print $1}')
ICM=$($NM -C $ELF | awk '$3=="inClickMenu"{print $1}')
for v in EBS EDS LBS EDC BET ICM; do [ -n "${(P)v}" ] || { echo "FAIL: symbol for $v not found"; exit 1; }; done
perl -pi -e "
s{(\"encoderButtonState\":\s+)0x[0-9a-f]+}{\${1}0x$EBS};
s{(\"encoderDirectionState\":\s+)0x[0-9a-f]+}{\${1}0x$EDS};
s{(\"lastButtonEncoderState\":\s+)0x[0-9a-f]+}{\${1}0x$LBS};
s{(\"encoderDirectionConsumed\":\s+)0x[0-9a-f]+}{\${1}0x$EDC};
s{(\"buttonEventTimestamp\":\s+)0x[0-9a-f]+}{\${1}0x$BET};
s{(\"inClickMenuPtr\":\s+)0x[0-9a-f]+}{\${1}0x$ICM};
" $JLI
grep -A7 "^ADDR = {" $JLI
