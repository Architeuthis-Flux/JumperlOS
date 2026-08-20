// SPDX-License-Identifier: MIT
#include "PioRegistry.h"
#include <string.h>

struct PioRegEntry {
    int8_t  block;   // 0..2, -1 = empty slot
    int8_t  sm;      // -1 = not tied to / unknown
    uint8_t offset;
    uint8_t length;
    const char* name;
};

#define PIO_REG_MAX 14
static PioRegEntry s_reg[PIO_REG_MAX];
static uint8_t s_regCount = 0;
static uint8_t s_regDropped = 0; // table full - shouldn't happen, but say so

void pioRegistryLog(PIO pio, int sm, uint offset, uint length, const char* name) {
    if (s_regCount >= PIO_REG_MAX) { s_regDropped++; return; }
    s_reg[s_regCount++] = { (int8_t)pio_get_index(pio), (int8_t)sm,
                            (uint8_t)offset, (uint8_t)length, name };
}

void pioRegistryUnlog(PIO pio, uint offset, const char* name) {
    int8_t b = (int8_t)pio_get_index(pio);
    for (int i = 0; i < s_regCount; i++) {
        if (s_reg[i].block == b && s_reg[i].offset == (uint8_t)offset &&
            strcmp(s_reg[i].name, name) == 0) {
            s_reg[i] = s_reg[--s_regCount];
            return;
        }
    }
}

void pioRegistryPrint(Stream& target) {
    for (int b = 0; b < (int)NUM_PIOS; b++) {
        PIO p = PIO_INSTANCE(b);
        uint used = 0;
        for (int i = 0; i < s_regCount; i++) {
            if (s_reg[i].block == b) used += s_reg[i].length;
        }
        target.printf("PIO%d@%-2u %2u/32:", b, pio_get_gpio_base(p), used);
        bool any = false;
        for (int i = 0; i < s_regCount; i++) {
            if (s_reg[i].block != b) continue;
            any = true;
            if (s_reg[i].sm >= 0) {
                target.printf(" %s[%u..%u]SM%d", s_reg[i].name, s_reg[i].offset,
                              s_reg[i].offset + s_reg[i].length - 1, s_reg[i].sm);
            } else {
                target.printf(" %s[%u..%u]", s_reg[i].name, s_reg[i].offset,
                              s_reg[i].offset + s_reg[i].length - 1);
            }
        }
        if (!any) target.print(" (no registered programs)");
        target.print("\n\r");
    }
    if (s_regDropped) target.printf("pio registry: %u entries DROPPED (table full)\n\r", s_regDropped);
}
