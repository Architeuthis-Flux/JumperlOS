// SPDX-License-Identifier: MIT
// Display driver registry: the i2cscrn table-driven driver lifted to C++.
// One interpreter (the mono-OLED ops below), N rodata descriptors; model
// differences are init bytes + two SH1106 quirks (charge pump 0xAD/0x8B and
// per-page addressing from column 2). Sequences are the bench-proven ones
// from the i2cscrn project (src/snakes/projectFiles.h:1393-1426).

#include "DisplayDrivers.h"

#include <ctype.h>
#include <string.h>

#include "DisplayBus.h"
#include "States.h"        // PartDefinition (partId)

// ---------------------------------------------------------------------------
// Mono-OLED family (SSD1306/1309/1315 + SH1106)
// ---------------------------------------------------------------------------
// Both run in PAGE addressing mode so the chunked flush can position every
// chunk explicitly (page + column) - the address pointer never has to
// survive between transactions, which also makes a mid-frame bus glitch
// self-correcting on the next chunk.

// 128x64 SSD1306-class init (0x20 0x02 = page addressing).
static const uint8_t ssd1306_64_init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 63, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x02,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40,
    0xA4, 0xA6, 0xAF,
};
// 128x32 variant (mux 31, COM pins 0x02).
static const uint8_t ssd1306_32_init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 31, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x02,
    0xA1, 0xC8, 0xDA, 0x02, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40,
    0xA4, 0xA6, 0xAF,
};
// SH1106 128x64: pump is 0xAD/0x8B and there is no 0x20 command (the part
// is page-addressed by nature).
static const uint8_t sh1106_64_init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 63, 0xD3, 0x00, 0x40,
    0xAD, 0x8B,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40,
    0xA4, 0xA6, 0xAF,
};

static bool monoOledInit(DisplayInstance& d) {
    for (uint16_t i = 0; i < d.desc->initLen; i++) {
        uint8_t c = d.desc->initSeq[i];
        if (!displayI2cWrite(d, 0x00, &c, 1)) return false;
    }
    d.flushCursor = 0;
    d.midFrame = false;
    return true;
}

// Chunked page-mode flush: position (page + column) then stream bytes, at
// most d.chunkBytes per call. The frame is fb in MONO_VLSB page order.
static int monoOledFlushChunk(DisplayInstance& d) {
    if (d.fb == nullptr || d.desc == nullptr) return 0;
    const uint16_t w = d.desc->w;
    const uint16_t frameBytes = d.desc->fbBytes;
    if (!d.midFrame) {
        d.flushCursor = 0;
        d.midFrame = true;
    }
    if (d.flushCursor >= frameBytes) {
        d.midFrame = false;
        return 0;
    }

    uint16_t page = d.flushCursor / w;
    uint16_t col = d.flushCursor % w;
    uint16_t left = w - col;                  // never cross a page per chunk
    uint16_t n = d.chunkBytes;
    if (n > left) n = left;
    if (n > frameBytes - d.flushCursor) n = frameBytes - d.flushCursor;

    // ONE merged transaction: three 0x80-continuation position commands,
    // then a 0x40 data run - so a soft-bus chunk pays a single
    // start/addr/stop overhead instead of four.
    uint8_t hwCol = (uint8_t)(col + d.desc->quirks.colOffset);
    uint8_t tx[7 + 64];
    tx[0] = 0x80; tx[1] = (uint8_t)(0xB0 | page);
    tx[2] = 0x80; tx[3] = (uint8_t)(0x00 | (hwCol & 0x0F));
    tx[4] = 0x80; tx[5] = (uint8_t)(0x10 | (hwCol >> 4));
    tx[6] = 0x40;
    if (n > 64) n = 64;
    memcpy(tx + 7, d.fb + d.flushCursor, n);
    if (!displayI2cWriteRaw(d, tx, (uint16_t)(7 + n))) return -1;

    d.flushCursor += n;
    if (d.flushCursor >= frameBytes) {
        d.midFrame = false;
        return 0;
    }
    return 1;
}

// "See this? Click to confirm": a border with corner ticks 2 px in - the
// pattern that makes the classic misdetection identify the right answer
// (an SH1106 driven as SSD1306 wraps the border by the 2-px column offset,
// and the ticks make the wrap unmistakable).
static void monoOledTestPattern(DisplayInstance& d, const char* label) {
    (void)label;   // no C++ framebuf font in slice 1 - the border is the message
    if (d.fb == nullptr) return;
    memset(d.fb, 0, d.desc->fbBytes);
    const int w = d.desc->w, h = d.desc->h;
    for (int x = 0; x < w; x++) {
        d.fb[x] |= 0x01;                              // top row
        d.fb[(h / 8 - 1) * w + x] |= 0x80;            // bottom row
    }
    for (int p = 0; p < h / 8; p++) {
        d.fb[p * w] = 0xFF;                           // left column
        d.fb[p * w + w - 1] = 0xFF;                   // right column
    }
    for (int t = 0; t < 8; t++) {                     // corner ticks, 2 px in
        d.fb[2 * w + 2] |= (1 << (t & 3));
        d.fb[2 * w + w - 3] |= (1 << (t & 3));
    }
    d.dirty = true;
}

static void monoOledSleep(DisplayInstance& d) {
    uint8_t off = 0xAE;
    displayI2cWrite(d, 0x00, &off, 1);
}

static const DisplayOps monoOledOps = {
    monoOledInit, monoOledFlushChunk, monoOledTestPattern, monoOledSleep,
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static const DisplayDriverDesc displayDrivers[] = {
    { "ssd1306", DispBus::I2C_SOFT, 128, 64, ssd1306_64_init,
      sizeof(ssd1306_64_init), &monoOledOps, { 0, true }, { 0x3C, 0x3D, 0 },
      1024, true },
    { "ssd1306_32", DispBus::I2C_SOFT, 128, 32, ssd1306_32_init,
      sizeof(ssd1306_32_init), &monoOledOps, { 0, true }, { 0x3C, 0x3D, 0 },
      512, true },
    { "sh1106", DispBus::I2C_SOFT, 128, 64, sh1106_64_init,
      sizeof(sh1106_64_init), &monoOledOps, { 2, true }, { 0x3C, 0x3D, 0 },
      1024, true },
};
static const int numDisplayDrivers = sizeof(displayDrivers) / sizeof(displayDrivers[0]);

static bool keyEq(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

const DisplayDriverDesc* findDisplayDriver(const char* key) {
    if (key == nullptr || key[0] == '\0') return nullptr;
    for (int i = 0; i < numDisplayDrivers; i++) {
        if (keyEq(displayDrivers[i].id, key)) return &displayDrivers[i];
    }
    return nullptr;
}

const DisplayDriverDesc* displayResolveForPart(const PartDefinition& p) {
    if (p.partId[0] == '\0') return nullptr;
    // Exact driverKey match first, then the model-name prefixes the seed DB
    // uses ("SSD1306_I2C", "ssd1315_i2c", "SH1106..." all reach a family).
    const DisplayDriverDesc* d = findDisplayDriver(p.partId);
    if (d != nullptr) return d;
    char low[17];
    int i = 0;
    for (; p.partId[i] != '\0' && i < 16; i++) low[i] = (char)tolower((unsigned char)p.partId[i]);
    low[i] = '\0';
    if (strncmp(low, "ssd130", 6) == 0 || strncmp(low, "ssd131", 6) == 0) {
        return findDisplayDriver(strstr(low, "32") != nullptr ? "ssd1306_32" : "ssd1306");
    }
    if (strncmp(low, "sh1106", 6) == 0) return findDisplayDriver("sh1106");
    return nullptr;
}
