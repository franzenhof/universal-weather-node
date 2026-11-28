// Fine Offset sensor-frame encoder (lib/.../FineOffsetEncoder.cpp): every frame
// it builds must decode back to the same values through foDecode - proving the
// byte layout, CRC-8 and checksum are correct (the decoder is validated against
// real frames). The transmit path is separate and V4-only; this is bytes only.
#include "FineOffsetEncoder.h"
#include "FineOffsetDecoder.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <initializer_list>

static float clampT(float c) { return c < -40 ? -40 : c > 60 ? 60 : c; }

int main()
{
    uint8_t b[FO_MAX_FRAME_LEN];

    // ---- WH31 thermo-hygrometer ----
    for (int ch = 1; ch <= 8; ch++)
        for (float temp : { -39.5f, -12.3f, 0.0f, 23.5f, 59.9f })
            for (int batt = 0; batt <= 1; batt++)
                for (int hasHum = 0; hasHum <= 1; hasHum++) {
                    size_t n = foEncodeWh31(b, 0x2A, (uint8_t)ch, temp, hasHum != 0, 55, batt != 0);
                    assert(n == 7);
                    assert(foCrc8(b, 5) == b[5] && foSum8(b, 6) == b[6]);   // the check bytes are right
                    FoFrame f;
                    assert(foDecode(b, n, f));
                    assert(f.family == FO_FAMILY_WH31 && f.id == 0x2A && f.channel == (uint8_t)ch);
                    assert(f.batteryOk == (batt != 0));
                    assert(f.hasTemp && std::fabs(f.tempC - clampT(temp)) < 0.05f);
                    if (hasHum) { assert(f.hasHumidity && f.humidity == 55); }
                    else        { assert(!f.hasHumidity); }
                }
    // a present humidity outside the WH31's 1..99 is clamped, not sent as "none" (WN30)
    for (int h : { 0, 1, 99, 100, 255 }) {
        FoFrame f;
        assert(foDecode(b, foEncodeWh31(b, 0x2A, 1, 20.0f, true, (uint8_t)h, true), f));
        assert(f.hasHumidity && f.humidity == (h < 1 ? 1 : h > 99 ? 99 : h));
    }
    // channel out of range is rejected
    assert(foEncodeWh31(b, 1, 0, 20.0f, true, 50, true) == 0);
    assert(foEncodeWh31(b, 1, 9, 20.0f, true, 50, true) == 0);

    // ---- WH40 rain gauge (20-bit id) ----
    for (uint32_t id : { 0x00001u, 0x5ABCDu, 0xFFFFFu })
        for (float rain : { 0.0f, 15.2f, 123.4f, 6553.5f })
            for (int batt = 0; batt <= 1; batt++) {
                size_t n = foEncodeWh40(b, id, rain, batt != 0);
                assert(n == 9);
                FoFrame f;
                assert(foDecode(b, n, f));
                assert(f.family == FO_FAMILY_WH40 && f.id == id);
                assert(f.hasRain && std::fabs(f.rainMm - rain) < 0.05f);
                assert(f.batteryOk == (batt != 0));
            }

    // ---- WN32P thermo-hygro-barometer (BME280), WH25 family ----
    for (uint32_t id : { 0x00u, 0x4Fu, 0xFFu })
        for (float temp : { -5.0f, 21.3f, 42.7f })
            for (int hum : { 0, 55, 100 })
                for (float press : { -1.0f, 950.1f, 1013.2f, 1084.0f }) {
                    size_t n = foEncodeWn32p(b, (uint8_t)id, temp, (uint8_t)hum, press, true);
                    assert(n == FO_WH25_FRAME_LEN);
                    FoFrame f;
                    assert(foDecode(b, n, f));
                    assert(f.id == id);
                    assert(f.hasTemp && std::fabs(f.tempC - clampT(temp)) < 0.05f);
                    if (hum >= 1) { assert(f.hasHumidity && f.humidity == hum); }
                    else          { assert(!f.hasHumidity); }
                    if (press >= 0.0f) { assert(f.hasPressure && std::fabs(f.pressureHpa - press) < 0.05f); }
                    else               { assert(!f.hasPressure); }
                }

    // ---- WH65 / WS69 outdoor array: wind + rain + temp/hum + UV + light ----
    {
        FoWh65Values v;
        v.id = 0x7C;
        v.hasTemp = true;  v.tempC = 18.5f;
        v.hasHum = true;   v.humidity = 62;
        v.hasWind = true;  v.windAvgMs = 5.2f; v.windGustMs = 9.8f; v.windDirDeg = 270;
        v.hasRain = true;  v.rainTotalMm = 123.4f;
        v.hasUv = true;    v.uvIndex = 5;
        v.hasLight = true; v.lightLux = 54000.0f;
        size_t n = foEncodeWh65(b, v);
        assert(n == 17);
        FoFrame f;
        assert(foDecode(b, n, f));
        assert(f.family == FO_FAMILY_WH65 && f.id == 0x7C && f.batteryOk);
        assert(f.hasTemp && std::fabs(f.tempC - 18.5f) < 0.05f);
        assert(f.hasHumidity && f.humidity == 62);
        assert(f.hasWind && std::fabs(f.windAvgMs - 5.2f) < 0.05f && std::fabs(f.windGustMs - 9.8f) < 0.3f && f.windDirDeg == 270);
        assert(f.hasRain && std::fabs(f.rainMm - 123.4f) < 0.15f);
        assert(f.hasUv && (int)f.uvIndex == 5);
        assert(f.hasLight && std::fabs(f.lightLux - 54000.0f) < 0.2f);
    }
    // wind direction round-trips across the full circle incl. the 9-bit MSB boundary
    for (int dir : { 0, 1, 90, 180, 255, 256, 270, 359 }) {
        FoWh65Values v; v.hasWind = true; v.windAvgMs = 3.0f; v.windGustMs = 4.0f; v.windDirDeg = (uint16_t)dir;
        size_t n = foEncodeWh65(b, v); FoFrame f;
        assert(foDecode(b, n, f) && f.hasWind && f.windDirDeg == (uint16_t)dir);
    }
    // everything off: the frame still decodes; only rain is always present (0)
    {
        FoWh65Values v; v.id = 0x10;
        size_t n = foEncodeWh65(b, v); FoFrame f;
        assert(foDecode(b, n, f));
        assert(!f.hasTemp && !f.hasHumidity && !f.hasWind && !f.hasUv && !f.hasLight);
        assert(f.hasRain && f.rainMm == 0.0f);
    }
    // UV index round-trips across all bands
    for (int uv = 0; uv <= 13; uv++) {
        FoWh65Values v; v.hasUv = true; v.uvIndex = (uint8_t)uv;
        size_t n = foEncodeWh65(b, v); FoFrame f;
        assert(foDecode(b, n, f) && f.hasUv && (int)f.uvIndex == uv);
    }

    printf("Firmware foencode regressions passed.\n");
    return 0;
}
