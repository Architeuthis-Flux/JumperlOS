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
#include "PartDb.h"        // partdbResolveDriver - the binding authority
#include "States.h"        // PartDefinition

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

// Zero the controller's WHOLE RAM (all 8 pages of a 64-row part - a 128x32
// panel's hidden half included). Power-on GDDRAM is noise; the old code let
// the first full frame paint over it, which flashed garbage on attach and
// would show the hidden half's noise on the first pageFlip. ~1 KB at the
// wire's ~37 us/byte is a one-time ~40 ms inside the beacon's init.
static bool monoOledClearRam(DisplayInstance& d) {
    const uint16_t w = d.desc->w;
    uint8_t tx[7 + 64];
    memset(tx + 7, 0, 64);
    for (uint8_t page = 0; page < 8; page++) {
        for (uint16_t col = 0; col < w; col += 64) {
            uint8_t hwCol = (uint8_t)(col + d.desc->quirks.colOffset);
            tx[0] = 0x80; tx[1] = (uint8_t)(0xB0 | page);
            tx[2] = 0x80; tx[3] = (uint8_t)(0x00 | (hwCol & 0x0F));
            tx[4] = 0x80; tx[5] = (uint8_t)(0x10 | (hwCol >> 4));
            tx[6] = 0x40;
            uint16_t n = (uint16_t)((w - col) < 64 ? (w - col) : 64);
            if (!displayI2cWriteRaw(d, tx, (uint16_t)(7 + n))) return false;
        }
    }
    return true;
}

static bool monoOledInit(DisplayInstance& d) {
    // Hold the final display-on (0xAF, every family sequence's last byte)
    // until AFTER the RAM clear - otherwise the panel lights up showing
    // power-on noise (or, on a re-init, the stale previous frame) and blanks
    // progressively as the clear sweeps (verify-workflow finding).
    for (uint16_t i = 0; i + 1 < d.desc->initLen; i++) {
        uint8_t c = d.desc->initSeq[i];
        if (!displayI2cWrite(d, 0x00, &c, 1)) return false;
    }
    if (!monoOledClearRam(d)) return false;
    uint8_t lastCmd = d.desc->initSeq[d.desc->initLen - 1];
    if (!displayI2cWrite(d, 0x00, &lastCmd, 1)) return false;
    d.flushCursor = 0;
    d.midFrame = false;
    // RAM is all-zero now: the shadow can say so, and the first frame is a
    // pure delta (only lit pixels transmit).
    if (d.shadow != nullptr) {
        uint16_t shadowBytes = d.desc->quirks.pageFlip ? (uint16_t)(d.desc->fbBytes * 2)
                                                       : d.desc->fbBytes;
        memset(d.shadow, 0, shadowBytes);
        d.shadowValidMask = d.desc->quirks.pageFlip ? 0b11 : 0b01;
    } else {
        d.shadowValidMask = 0;
    }
    // initSeq's 0x40 shows RAM line 0 (half 0) - so writing starts on the
    // hidden half 1 and the first completed frame flips to it.
    d.backHalf = d.desc->quirks.pageFlip ? 1 : 0;
    return true;
}

// One chunk's span at `cursor`: at most chunkBytes, never crossing a page.
static inline uint16_t chunkSpan(const DisplayInstance& d, uint16_t cursor) {
    const uint16_t w = d.desc->w;
    uint16_t n = d.chunkBytes;
    uint16_t left = w - (cursor % w);
    if (n > left) n = left;
    if (n > d.desc->fbBytes - cursor) n = (uint16_t)(d.desc->fbBytes - cursor);
    return n;
}

// Chunked page-mode flush: position (page + column) then stream bytes, at
// most d.chunkBytes per call. The frame is fb in MONO_VLSB page order.
// DELTA FLUSH: chunks matching the shadow (what the panel already holds)
// are skipped, so a frame costs only its changed bytes. On a ~250 kHz soft
// bus that is the anti-tearing lever - a full-frame sweep takes ~250 ms and
// shears moving content, while the animation's real per-frame delta is a
// handful of chunks that land in tens of ms.
static int monoOledFlushChunk(DisplayInstance& d) {
    if (d.fb == nullptr || d.desc == nullptr) return 0;
    const uint16_t w = d.desc->w;
    const uint16_t frameBytes = d.desc->fbBytes;
    const bool flip = d.desc->quirks.pageFlip;
    // pageFlip: everything writes into the HIDDEN half (backHalf) - its page
    // window in RAM and its own image in the shadow. The shown half is never
    // touched mid-frame, which is what makes the flip tear-free.
    const uint8_t pageBase = flip ? (uint8_t)(d.backHalf * (d.desc->h / 8)) : 0;
    const uint16_t shadowOff = (flip && d.backHalf) ? frameBytes : 0;
    if (!d.midFrame) {
        d.flushCursor = 0;
        d.midFrame = true;
    }
    if (d.shadow != nullptr && (d.shadowValidMask & (1u << d.backHalf))) {
        while (d.flushCursor < frameBytes) {
            uint16_t span = chunkSpan(d, d.flushCursor);
            if (memcmp(d.fb + d.flushCursor, d.shadow + shadowOff + d.flushCursor, span) != 0)
                break;
            d.flushCursor += span;
        }
    }
    if (d.flushCursor >= frameBytes) {
        if (flip) {
            // The atomic reveal: one display-start-line command maps the
            // just-written half onto the panel. On NACK, return -1 WITHOUT
            // toggling - the retry skips every (already-matching) chunk and
            // lands back here to resend the flip.
            uint8_t flipCmd = (uint8_t)(0x40 | (d.backHalf ? 32 : 0));
            if (!displayI2cWrite(d, 0x00, &flipCmd, 1)) return -1;
            d.shadowValidMask |= (uint8_t)(1u << d.backHalf);
            d.backHalf ^= 1;
        } else if (d.shadow != nullptr) {
            d.shadowValidMask |= 0b01;
        }
        d.midFrame = false;
        return 0;
    }

    uint16_t page = d.flushCursor / w;
    uint16_t col = d.flushCursor % w;
    uint16_t n = chunkSpan(d, d.flushCursor);

    // ONE merged transaction: three 0x80-continuation position commands,
    // then a 0x40 data run - so a soft-bus chunk pays a single
    // start/addr/stop overhead instead of four.
    uint8_t hwCol = (uint8_t)(col + d.desc->quirks.colOffset);
    uint8_t tx[7 + 64];
    tx[0] = 0x80; tx[1] = (uint8_t)(0xB0 | (page + pageBase));
    tx[2] = 0x80; tx[3] = (uint8_t)(0x00 | (hwCol & 0x0F));
    tx[4] = 0x80; tx[5] = (uint8_t)(0x10 | (hwCol >> 4));
    tx[6] = 0x40;
    if (n > 64) n = 64;
    memcpy(tx + 7, d.fb + d.flushCursor, n);
    if (!displayI2cWriteRaw(d, tx, (uint16_t)(7 + n))) return -1;

    if (d.shadow != nullptr)
        memcpy(d.shadow + shadowOff + d.flushCursor, d.fb + d.flushCursor, n);
    d.flushCursor += n;
    // Frame-end bookkeeping (including the pageFlip reveal) lives at the top
    // of the next call - one completion path, not two. The burst loop keeps
    // calling while we return 1, so the flip still lands in the same visit.
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
    // quirks = { colOffset, pagedAddressing, pageFlip }. pageFlip only where
    // the panel shows HALF the controller's RAM (128x32 on a 64-row part) -
    // the 64-row panels use all of it and flush progressively instead.
    { "ssd1306", DispBus::I2C_SOFT, 128, 64, ssd1306_64_init,
      sizeof(ssd1306_64_init), &monoOledOps, { 0, true, false }, { 0x3C, 0x3D, 0 },
      1024, true },
    { "ssd1306_32", DispBus::I2C_SOFT, 128, 32, ssd1306_32_init,
      sizeof(ssd1306_32_init), &monoOledOps, { 0, true, true }, { 0x3C, 0x3D, 0 },
      512, true },
    { "sh1106", DispBus::I2C_SOFT, 128, 64, sh1106_64_init,
      sizeof(sh1106_64_init), &monoOledOps, { 2, true, false }, { 0x3C, 0x3D, 0 },
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
    // B-M5: partdbResolveDriver is the ONE binding authority (part.driverKey
    // override, else partId -> record -> record driverKey - id or alias,
    // case-fold). This replaced the temporary partId-prefix sniffing; a key
    // the C registry doesn't implement yet (st7789, 40rgbx160...) resolves
    // here and simply finds no descriptor - the part still places and
    // labels, it just doesn't animate until its driver lands.
    const char* key = partdbResolveDriver(p);
    if (key == nullptr) return nullptr;
    return findDisplayDriver(key);
}
