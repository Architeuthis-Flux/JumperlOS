#!/usr/bin/env python3
# Round 5 (Kevin, 2026-08-30 13:04): dimmer still, and a liveness shimmer -
# the rows under the meter pulse a slow rainbow whenever a session is
# waiting on hardware (partScanActivityHook), so the board never looks
# frozen for more than a blink.
import sys

path = '/Users/kevinsanto/Documents/GitHub/JumperlOS/src/PartsApp.cpp'
src = open(path).read()
orig = src
fails = []

def sub(tag, old, new, count=1):
    global src
    n = src.count(old)
    if n != count:
        fails.append(f'{tag}: found {n}, wanted {count}')
        return
    src = src.replace(old, new)

# ---- brightness: third ask, take it well down ------------------------------
sub('hit-v',
"""static const uint8_t PARTS_HIT_V = 70;   // held-hit brightness (was 110 -
                                         // "a bit more dim", 2026-08-30)""",
"""static const uint8_t PARTS_HIT_V = 40;   // held-hit brightness (110 -> 70
                                         // -> 40, "still too bright")""")
sub('cursor-v',
"""        case 0: b.printRawRow(0b00011111, pr, partsScanVizHue(row, 48), 0xffffff); break;""",
"""        case 0: b.printRawRow(0b00011111, pr, partsScanVizHue(row, 30), 0xffffff); break;""")
sub('pair-cursor-v',
"""            b.printRawRow(0b00011111, pr, partsScanVizHue(row, 36), 0xffffff);
            int pr2 = nodeToPrintRow(row + 1);
            if (pr2 >= 0) b.printRawRow(0b00011111, pr2, partsScanVizHue(row + 1, 36), 0xffffff);""",
"""            b.printRawRow(0b00011111, pr, partsScanVizHue(row, 22), 0xffffff);
            int pr2 = nodeToPrintRow(row + 1);
            if (pr2 >= 0) b.printRawRow(0b00011111, pr2, partsScanVizHue(row + 1, 22), 0xffffff);""")

# ---- meter rows are remembered, and shimmer while the hardware waits --------
sub('meter-track',
"""static const uint32_t PARTS_METER_COLOR = 0x121216;
static void partsMeterViz2(int a, int b2) {
    partsPaintRow(a, PARTS_METER_COLOR);
    partsPaintRow(b2, PARTS_METER_COLOR);
    requestLedShow(2);
}
static void partsMeterViz3(int a, int b2, int c) {
    partsPaintRow(a, PARTS_METER_COLOR);
    partsPaintRow(b2, PARTS_METER_COLOR);
    partsPaintRow(c, PARTS_METER_COLOR);
    requestLedShow(2);
}""",
"""static const uint32_t PARTS_METER_COLOR = 0x0C0C0E;
static int16_t s_meterRows[3] = {-1, -1, -1};
static void partsMeterViz2(int a, int b2) {
    s_meterRows[0] = (int16_t)a;
    s_meterRows[1] = (int16_t)b2;
    s_meterRows[2] = -1;
    partsPaintRow(a, PARTS_METER_COLOR);
    partsPaintRow(b2, PARTS_METER_COLOR);
    requestLedShow(2);
}
static void partsMeterViz3(int a, int b2, int c) {
    s_meterRows[0] = (int16_t)a;
    s_meterRows[1] = (int16_t)b2;
    s_meterRows[2] = (int16_t)c;
    partsPaintRow(a, PARTS_METER_COLOR);
    partsPaintRow(b2, PARTS_METER_COLOR);
    partsPaintRow(c, PARTS_METER_COLOR);
    requestLedShow(2);
}
// The liveness shimmer (partScanActivityHook): whenever a measurement is
// waiting on hardware - an INA conversion, a decay watch, a drain - the
// rows under the meter cycle a slow dim rainbow. Self-rate-limited to
// ~90ms a frame; a session that never waits never shimmers, and a board
// that IS waiting visibly breathes instead of looking hung (Kevin,
// 2026-08-30: "never seem like it's frozen for more than a quarter of a
// second").
static void partsMeterPulse(void) {
    static unsigned long lastMs = 0;
    unsigned long now = millis();
    if (now - lastMs < 90) return;
    lastMs = now;
    bool any = false;
    for (int k = 0; k < 3; k++) {
        int r = s_meterRows[k];
        if (r < 1 || r > 60) continue;
        hsvColor h;
        h.h = (uint8_t)((now / 12 + k * 40) & 0xFF);
        h.s = 200;
        h.v = 20;
        partsPaintRow(r, HsvToRaw(h));
        any = true;
    }
    if (any) requestLedShow(2);
}""")

# meter cleanup also forgets the rows (the shimmer must not resurrect them)
sub('meter-done-forget',
"""static void partsMeterDone(int a, int b2, int c) {
    int rr[3] = {a, b2, c};""",
"""static void partsMeterDone(int a, int b2, int c) {
    s_meterRows[0] = s_meterRows[1] = s_meterRows[2] = -1;
    int rr[3] = {a, b2, c};""")

# ---- rails + fingerprint sessions feed the shimmer their rows ---------------
sub('rails-meter-rows',
"""        // the candidate, on the board: green = tried as ground, warm = as
        // VDD, white = the two witness pins under the meter
        partsPaintRow(cand[i][0], 0x003008);""",
"""        // the candidate, on the board: green = tried as ground, warm = as
        // VDD, white = the two witness pins under the meter
        s_meterRows[0] = (int16_t)cand[i][0];
        s_meterRows[1] = (int16_t)cand[i][1];
        s_meterRows[2] = (int16_t)probeRows[0];
        partsPaintRow(cand[i][0], 0x003008);""")
sub('fp-meter-rows',
"""static void partsFpViz(int row, int state) {""",
"""static void partsFpViz(int row, int state) {
    if (state == 0) {   // this pin is the one under the meter now
        s_meterRows[0] = (int16_t)row;
        s_meterRows[1] = -1;
        s_meterRows[2] = -1;
    }""")

# ---- hook registration: both launchers own the pulse for their lifetime -----
sub('auto-hook-set',
"""void partsAutoLauncher(void) {
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;
    partsAutoAborted = false;
    partsAutoAbortCause = nullptr;""",
"""void partsAutoLauncher(void) {
    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;
    partsAutoAborted = false;
    partsAutoAbortCause = nullptr;
    partScanActivityHook = partsMeterPulse;   // sessions shimmer their rows""")
sub('auto-hook-clear',
"""    s_scanVizFlags = nullptr;   // it points at this frame's flags[] - never
                                // let partsMeterDone read it after we return""",
"""    s_scanVizFlags = nullptr;   // it points at this frame's flags[] - never
                                // let partsMeterDone read it after we return
    partScanActivityHook = nullptr;
    s_meterRows[0] = s_meterRows[1] = s_meterRows[2] = -1;""")
sub('test-hook-set',
"""    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    int pick = 0;
    while (true) {""",
"""    inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;
    partScanActivityHook = partsMeterPulse;   // sessions shimmer their rows

    int pick = 0;
    while (true) {""")
sub('test-hook-clear',
"""        // the reading stays up until the user says they've read it
        if (partsWaitForPress() == -2) break;
    }

    inClickMenu = 0;""",
"""        // the reading stays up until the user says they've read it
        if (partsWaitForPress() == -2) break;
    }

    partScanActivityHook = nullptr;
    s_meterRows[0] = s_meterRows[1] = s_meterRows[2] = -1;
    inClickMenu = 0;""")

if fails:
    print('PATCH INCOMPLETE - nothing written:')
    for f in fails:
        print(' ', f)
    sys.exit(1)

open(path, 'w').write(src)
print('all patches applied; delta lines:', src.count('\n') - orig.count('\n'))
