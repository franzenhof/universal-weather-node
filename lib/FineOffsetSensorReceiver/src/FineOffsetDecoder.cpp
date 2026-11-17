#include "FineOffsetDecoder.h"

uint8_t foCrc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint8_t foSum8(const uint8_t* data, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) s = (uint8_t)(s + data[i]);
    return s;
}

namespace {

struct Family { uint8_t code; uint8_t len; const char* model; };
const Family FAMILIES[] = {
    { FO_FAMILY_WH65,  17, "WH65" },
    { FO_FAMILY_WN20,  10, "WN20" },
    { FO_FAMILY_WH31,   7, "WH31" },
    { FO_FAMILY_WH31B,  7, "WH31B" },
    { FO_FAMILY_WN34,   9, "WN34" },
    { FO_FAMILY_WN38,   9, "WN38" },
    { FO_FAMILY_WH40,   9, "WH40" },
    { FO_FAMILY_WH51,  14, "WH51" },
    { FO_FAMILY_WH57,   9, "WH57" },
    { FO_FAMILY_WS68,  16, "WS68" },
    { FO_FAMILY_WS80,  18, "WS80" },
    { FO_FAMILY_WS85,  28, "WS85" },
    { FO_FAMILY_WS90,  32, "WS90" },
};

const Family* findFamily(uint8_t code)
{
    for (const Family& f : FAMILIES)
        if (f.code == code) return &f;
    return nullptr;
}

// CRC over the bytes before CC, sum over the bytes before SS (CC SS = the last two)
bool checkFrame(const uint8_t* b, size_t n)
{
    return foCrc8(b, n - 2) == b[n - 2] && foSum8(b, n - 1) == b[n - 1];
}

uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

void setHumidity(FoFrame& o, uint8_t h)
{
    if (h > 100) return;          // 0xFF = no reading
    o.hasHumidity = true;
    o.humidity    = h;
}

// Temperature as (degC + 40) * 10
void setTemp400(FoFrame& o, int raw)
{
    o.hasTemp = true;
    o.tempC   = (raw - 400) * 0.1f;
}

// UV sensor value (0..20000) of the 0x24 family -> UV index, the console's steps
uint8_t uvIndexFromRaw(uint32_t raw)
{
    static const uint16_t upper[] = { 432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029 };
    uint8_t i = 0;
    while (i < sizeof(upper) / sizeof(upper[0]) && raw > upper[i]) i++;
    return i;
}

// WS80 / WS90 / WS68 / WS85: 9-bit wind values with their MSBs in a flags byte
void setWind9(FoFrame& o, uint8_t flags, uint8_t avg, uint8_t dir, uint8_t gust)
{
    int a = (flags & 0x10) << 4 | avg;
    int d = (flags & 0x20) << 3 | dir;
    int g = (flags & 0x40) << 2 | gust;
    if (a == 0x1FF || d == 0x1FF || d >= 360) return;
    o.hasWind    = true;
    o.windAvgMs  = a * 0.1f;
    o.windGustMs = g == 0x1FF ? o.windAvgMs : g * 0.1f;
    o.windDirDeg = (uint16_t)d;
}

void decodeWh65(const uint8_t* b, size_t len, FoFrame& o, bool wh24Units)
{
    o.model = wh24Units ? "WH24" : "WH65";
    o.id = b[1];
    o.batteryOk = !(b[3] & 0x08);
    int temp = (b[3] & 0x07) << 8 | b[4];
    if (temp != 0x7FF) setTemp400(o, temp);
    setHumidity(o, b[5]);
    int dir  = (b[3] & 0x80) << 1 | b[2];
    int wind = (b[3] & 0x10) << 4 | b[6];
    int gust = b[7];
    const float step = wh24Units ? 1.12f : 0.51f;   // m/s
    if (wind != 0x1FF && dir != 0x1FF && dir < 360) {
        o.hasWind    = true;
        o.windAvgMs  = wind * 0.125f * step;       // wind speed is sent * 8
        o.windGustMs = gust == 0xFF ? o.windAvgMs : gust * step;
        o.windDirDeg = (uint16_t)dir;
    }
    uint32_t rain = be16(b + 8);
    o.hasRain = true;
    o.rainMm  = rain * (wh24Units ? 0.3f : 0.254f);
    uint32_t uv = be16(b + 10);
    if (uv != 0xFFFF) { o.hasUv = true; o.uvIndex = uvIndexFromRaw(uv); }
    uint32_t light = be24(b + 12);
    if (light != 0xFFFFFF) { o.hasLight = true; o.lightLux = light * 0.1f; }
    // WS69: 8 more bytes with a 17-bit pressure value and its own CRC + sum
    if (len >= 25 && checkFrame(b, 25)) {
        if (!wh24Units) o.model = "WS69";
        uint32_t p = ((uint32_t)(b[17] & 0x01) << 16) | be16(b + 18);
        if (p != 0x1FFFF) { o.hasPressure = true; o.pressureHpa = p * 0.01f; }
    }
}

void decodeWs80(const uint8_t* b, FoFrame& o)   // WS80 and WS90 (same first 14 bytes)
{
    o.id = be24(b + 1);
    uint32_t light = be16(b + 4);
    if (light != 0xFFFF) { o.hasLight = true; o.lightLux = light * 10.0f; }
    o.batteryMv = (uint16_t)(b[6] * 20);
    o.batteryOk = o.batteryMv > 2400;    // 2 x AA lithium (WS85 threshold of rtl_433)
    int temp = (b[7] & 0x03) << 8 | b[8];
    if (temp != 0x3FF) setTemp400(o, temp);
    setHumidity(o, b[9]);
    setWind9(o, b[7], b[10], b[11], b[12]);
    if (b[13] != 0xFF) { o.hasUv = true; o.uvIndex = b[13] * 0.1f; }
}

} // namespace

size_t foFrameLength(uint8_t family)
{
    const Family* f = findFamily(family);
    return f ? f->len : 0;
}

bool foDecode(const uint8_t* b, size_t len, FoFrame& out, bool wh24Units)
{
    if (!b || len < 1) return false;
    const Family* f = findFamily(b[0]);
    if (!f || len < f->len || !checkFrame(b, f->len)) return false;
    FoFrame o;
    o.family = f->code;
    o.model  = f->model;
    switch (f->code) {
        case FO_FAMILY_WH65:
            decodeWh65(b, len, o, wh24Units);
            break;
        case FO_FAMILY_WH31: case FO_FAMILY_WH31B:
            o.id        = b[1];
            o.channel   = (uint8_t)(((b[2] >> 4) & 0x07) + 1);
            o.batteryOk = !(b[2] & 0x04);
            setTemp400(o, (b[2] & 0x03) << 8 | b[3]);
            setHumidity(o, b[4]);
            break;
        case FO_FAMILY_WN34: case FO_FAMILY_WN38: {
            o.id = be24(b + 1);
            // 12-bit two's complement, tenths; sub-type 4 (WN34D, WN38) without the +40 offset
            int raw = (b[4] & 0x0F) << 8 | b[5];
            if (raw & 0x800) raw -= 0x1000;
            o.hasTemp   = true;
            o.tempC     = raw * 0.1f - ((b[4] >> 4) == 4 ? 0.0f : 40.0f);
            o.batteryMv = (uint16_t)((b[6] & 0x7F) * 20);
            o.batteryOk = o.batteryMv > 1200;
            break;
        }
        case FO_FAMILY_WH40:
            o.id        = (uint32_t)(b[1] & 0x0F) << 16 | be16(b + 2);
            o.batteryMv = (uint16_t)((b[4] & 0x1F) * 100);
            o.batteryOk = o.batteryMv == 0 || o.batteryMv >= 1000;
            o.hasRain   = true;
            o.rainMm    = be16(b + 5) * 0.1f;
            break;
        case FO_FAMILY_WN20:
            o.id        = be16(b + 2);
            o.batteryMv = (uint16_t)(b[4] * 20);
            o.batteryOk = b[4] > 90;               // 1.8 V (2 x AAA)
            o.hasRain   = true;
            o.rainMm    = be16(b + 5) * 0.1f;
            break;
        case FO_FAMILY_WH51:
            o.id          = be24(b + 1);
            o.batteryMv   = (uint16_t)((b[4] & 0x1F) * 100);
            o.batteryOk   = o.batteryMv >= 1300;   // 1 x AA; erratic readings from 1.2 V
            if (b[6] <= 100) { o.hasMoisture = true; o.moisture = b[6]; }
            break;
        case FO_FAMILY_WH57: {
            o.id             = (uint32_t)(b[1] & 0x0F) << 16 | be16(b + 2);
            o.batteryOk      = ((b[4] >> 1) & 0x03) >= 1;   // 0 = about 2.3 V, 1 = 2.6 V, 2 = 3.2 V
            o.hasLightning   = true;
            o.lightningState = b[1] >> 4;
            o.lightningCount = b[6];
            uint8_t km = b[5] & 0x3F;
            o.stormDistanceKm = km == 0x3F ? FO_LIGHTNING_NO_DISTANCE : km;
            break;
        }
        case FO_FAMILY_WS68: {
            o.id = be16(b + 2);
            uint32_t light = be16(b + 4);
            if (light != 0xFFFF) { o.hasLight = true; o.lightLux = light * 10.0f; }
            o.batteryOk = b[6] > 0x20;
            setWind9(o, b[7], b[10], b[11], b[12]);
            if (b[13] != 0xFF) { o.hasUv = true; o.uvIndex = b[13] * 0.1f; }
            break;
        }
        case FO_FAMILY_WS80:
            decodeWs80(b, o);
            break;
        case FO_FAMILY_WS90:
            decodeWs80(b, o);
            o.hasRain = true;
            o.rainMm  = be16(b + 19) * 0.1f;
            break;
        case FO_FAMILY_WS85:
            o.id        = be24(b + 1);
            o.batteryMv = (uint16_t)(b[4] * 20);
            o.batteryOk = o.batteryMv > 2400;
            setWind9(o, b[5], b[7], b[8], b[9]);
            o.hasRain = true;
            o.rainMm  = be16(b + 15) * 0.1f;
            break;
        default:
            return false;
    }
    out = o;
    return true;
}
