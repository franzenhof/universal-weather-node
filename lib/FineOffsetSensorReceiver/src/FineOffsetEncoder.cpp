#include "FineOffsetEncoder.h"
#include <cmath>

namespace {

// (°C + 40) * 10, clamped to the sensors' realistic range (-40..60 °C = 0..1000);
// the WH25 decoder rejects a raw value above 1000.
int tempRaw(float c)
{
    long r = lroundf(c * 10.0f) + 400;
    return r < 0 ? 0 : r > 1000 ? 1000 : (int)r;
}

// Append CRC-8 over the payload and the 8-bit sum over payload + CRC (the last
// two bytes of every family-byte frame); returns the full frame length.
size_t finishCrcSum(uint8_t* b, size_t payloadLen)
{
    b[payloadLen]     = foCrc8(b, payloadLen);
    b[payloadLen + 1] = foSum8(b, payloadLen + 1);
    return payloadLen + 2;
}

} // namespace

size_t foEncodeWh31(uint8_t* out, uint8_t id, uint8_t channel, float tempC,
                    bool hasHum, uint8_t humidity, bool batteryOk)
{
    if (!out || channel < 1 || channel > 8) return 0;
    const int tr = tempRaw(tempC);
    out[0] = FO_FAMILY_WH31;
    out[1] = id;
    out[2] = (uint8_t)((((channel - 1) & 0x07) << 4) | (batteryOk ? 0 : 0x04) | ((tr >> 8) & 0x03));
    out[3] = (uint8_t)(tr & 0xFF);
    // 0 = temperature only (WN30); a present humidity is clamped to the WH31's
    // 1..99 %, so a saturated sensor (fog) does not turn into a WN30
    out[4] = hasHum ? (humidity < 1 ? 1 : humidity > 99 ? 99 : humidity) : 0;
    return finishCrcSum(out, 5);
}

size_t foEncodeWh40(uint8_t* out, uint32_t id, float rainTotalMm, bool batteryOk)
{
    if (!out) return 0;
    long rr = lroundf(rainTotalMm * 10.0f);
    if (rr < 0) rr = 0; else if (rr > 0xFFFF) rr = 0xFFFF;
    out[0] = FO_FAMILY_WH40;
    out[1] = (uint8_t)((id >> 16) & 0x0F);
    out[2] = (uint8_t)((id >> 8) & 0xFF);
    out[3] = (uint8_t)(id & 0xFF);
    out[4] = batteryOk ? 15 : 5;   // (b[4] & 0x1F) * 100 mV: 1500 = ok, 500 = low
    out[5] = (uint8_t)((rr >> 8) & 0xFF);
    out[6] = (uint8_t)(rr & 0xFF);
    return finishCrcSum(out, 7);
}

size_t foEncodeWn32p(uint8_t* out, uint8_t id, float tempC, uint8_t humidity,
                     float pressureHpa, bool batteryOk)
{
    if (!out) return 0;
    const int tr = tempRaw(tempC);
    out[0] = (uint8_t)(0xE0 | ((id >> 4) & 0x0F));   // type nibble 0xE (thermo-hygro + pressure)
    out[1] = (uint8_t)(((id & 0x0F) << 4) | (batteryOk ? 0 : 0x08) | ((tr >> 8) & 0x03));   // invalid bit 0x04 = 0
    out[2] = (uint8_t)(tr & 0xFF);
    out[3] = (humidity >= 1 && humidity <= 100) ? humidity : 0;
    long pr = 0xFFFF;
    if (pressureHpa >= 0.0f) {
        pr = lroundf(pressureHpa * 10.0f);
        if (pr < 3000 || pr > 11000) pr = 0xFFFF;   // out of 300..1100 hPa: the decoder rejects it, so send "none"
    }
    out[4] = (uint8_t)((pr >> 8) & 0xFF);
    out[5] = (uint8_t)(pr & 0xFF);
    out[6] = foSum8(out, 6);
    uint8_t x = 0;
    for (int i = 0; i < 6; i++) x ^= out[i];
    out[7] = (uint8_t)((x << 4) | (x >> 4));   // WH25-style nibble-swapped XOR (ignored by WN32B consoles)
    return FO_WH25_FRAME_LEN;
}

namespace {
// WH65 UV field: the raw value the decoder maps to a given UV index (the band
// just above the previous threshold), so decode(encode(i)) == i.
uint32_t uvRawForIndex(uint8_t idx)
{
    static const uint16_t upper[] = { 432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029 };
    const uint8_t n = sizeof(upper) / sizeof(upper[0]);
    if (idx == 0) return 0;
    if (idx > n) idx = (uint8_t)(n + 1);
    return idx <= n ? (uint32_t)upper[idx - 1] + 1 : 5030u;
}
long clampL(long v, long lo, long hi) { return v < lo ? lo : v > hi ? hi : v; }
} // namespace

size_t foEncodeWh65(uint8_t* out, const FoWh65Values& v)
{
    if (!out) return 0;
    // Wind: 9-bit speed (0.125 * 0.51 m/s per step), 8-bit gust (0.51 m/s),
    // 9-bit direction 0..359. 0x1FF speed or direction = "no wind".
    int wraw = 0x1FF, draw = 0x1FF, graw = 0xFF;
    if (v.hasWind && v.windDirDeg < 360) {
        wraw = (int)clampL(lroundf(v.windAvgMs / (0.125f * 0.51f)), 0, 0x1FE);
        graw = (int)clampL(lroundf(v.windGustMs / 0.51f), 0, 0xFE);
        draw = v.windDirDeg;
    }
    // Temperature: 11-bit (°C + 40) * 10, 0x7FF = none.
    int traw = v.hasTemp ? (int)clampL(lroundf(v.tempC * 10.0f) + 400, 0, 0x7FE) : 0x7FF;
    long rraw = v.hasRain ? clampL(lroundf(v.rainTotalMm / 0.254f), 0, 0xFFFF) : 0;   // rain is always present
    uint32_t uraw = v.hasUv ? uvRawForIndex(v.uvIndex) : 0xFFFFu;
    long lraw = v.hasLight ? clampL(lroundf(v.lightLux * 10.0f), 0, 0xFFFFFE) : 0xFFFFFF;

    out[0]  = FO_FAMILY_WH65;
    out[1]  = v.id;
    out[2]  = (uint8_t)(draw & 0xFF);
    out[3]  = (uint8_t)((((draw >> 8) & 1) << 7) | (((wraw >> 8) & 1) << 4) | (v.batteryOk ? 0 : 0x08) | ((traw >> 8) & 0x07));
    out[4]  = (uint8_t)(traw & 0xFF);
    out[5]  = (v.hasHum && v.humidity <= 100) ? v.humidity : 0xFF;
    out[6]  = (uint8_t)(wraw & 0xFF);
    out[7]  = (uint8_t)(graw & 0xFF);
    out[8]  = (uint8_t)((rraw >> 8) & 0xFF);
    out[9]  = (uint8_t)(rraw & 0xFF);
    out[10] = (uint8_t)((uraw >> 8) & 0xFF);
    out[11] = (uint8_t)(uraw & 0xFF);
    out[12] = (uint8_t)((lraw >> 16) & 0xFF);
    out[13] = (uint8_t)((lraw >> 8) & 0xFF);
    out[14] = (uint8_t)(lraw & 0xFF);
    return finishCrcSum(out, 15);
}
