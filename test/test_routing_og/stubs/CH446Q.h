#pragma once
#include <stdint.h>
struct chipXYBitfield { uint16_t connected[8]; };
extern chipXYBitfield lastChipXY[12];
extern int ch446q_timeout_count;
void updateLiveCrossbarDisplay(void);
void sendXYrawUnchecked(int chip, int x, int y, int setOrClear, unsigned long timeoutUs = 5000);
