#pragma once
// OLED screensaver (Settings -> Display, docs/settings.md#display): takes over the
// 128x64 display every few minutes for a few seconds, so the always-lit pixels
// of the normal screens rest (burn-in). Pure code, no Arduino / display library:
// frames are drawn straight into a 1024-byte buffer in SSD1306 order (byte
// x + 128 * page, bit = row in the page) - the same layout as oledPreviewCustom(),
// so the main display (memcpy into the driver's buffer), the settings preview
// and the host test (test/firmware_screensaver.cpp) share one renderer.
// All sprites are drawn procedurally (lines, circles): no bitmaps, no RAM.
#include <stdint.h>
#include <math.h>

enum SaverMode : uint8_t { SV_OFF = 0, SV_BLANK, SV_DOT, SV_FIGURE, SV_ICONS, SV_MODE_COUNT };

// Weather conditions of the walking figure (one animation pool each, screensaver.cpp)
enum SaverCond : uint8_t { SC_FROST = 0, SC_THAW, SC_MILD, SC_HOT, SC_UV, SC_RAIN, SC_WIND,
                           SC_LIGHTNING, SC_FOG, SC_AIR, SC_SOIL, SC_NIGHT, SC_COUNT };

// What the figure reacts to; a missing value is NAN (soilPct: -1).
struct SaverInput {
    float tempC = NAN, humidity = NAN, uvIndex = NAN;
    float rainRateMmH = NAN, rain1hMm = NAN, windMs = NAN, gustMs = NAN;
    float pm25 = NAN, co2Ppm = NAN;
    int   soilPct = -1;          // driest WH51 sensor
    bool  lightning = false;     // WH57 reports a storm front
    bool  thawed = false;        // it froze within the last hours and is above 0 degC now
    bool  night = false;
};

// Bit mask (1 << SaverCond) of the conditions that hold; SC_MILD only when
// nothing else applies to the temperature (0..27 degC or unknown).
uint16_t saverConditions(const SaverInput& in);
// The condition to show: lightning always; else one of the active
// non-trivial ones at random (rnd); else night, else mild.
uint8_t saverPickCondition(const SaverInput& in, uint32_t rnd);

// Number of animations in the pool of a condition / of the rare surprises, and a name.
uint8_t     saverAnimCount(uint8_t cond);
uint8_t     saverSurpriseCount();
const char* saverAnimName(uint8_t cond, uint8_t anim, bool surprise);

struct SaverScene {
    uint8_t  mode = SV_OFF, cond = SC_MILD, anim = 0;
    bool     surprise = false;   // a rare animation from the surprise pool instead
    uint32_t seed = 0;           // positions of the snow / rain, ...
};
// Chooses condition and animation (the surprise: about 1 in 10) for mode.
SaverScene saverStart(uint8_t mode, const SaverInput& in, uint32_t rnd);
// Frame at elapsedMs of a takeover lasting durationMs (clamped), into out[1024].
void saverRender(const SaverScene& sc, uint32_t elapsedMs, uint32_t durationMs, uint8_t out[1024]);
