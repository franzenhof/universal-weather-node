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
    { FO_FAMILY_WN35,  11, "WN35" },
    { FO_FAMILY_WH41,   8, "WH41" },
    { FO_FAMILY_WH0290, 8, "WH0290" },
    { FO_FAMILY_WH43,  10, "WH43" },
    { FO_FAMILY_WH45,  15, "WH45" },
    { FO_FAMILY_WH46,  21, "WH46" },
    { FO_FAMILY_WH55,  10, "WH55" },   // followed by 2 bytes of unknown meaning (not checked)
    { FO_FAMILY_WH52,  24, "WH52" },
    // LDS01: 19-byte frames (CRC-8 at byte 17, sum at 18) every 80 s, seen in
    // a forum radio trace (rc.8, 2026-11-21); the field meanings are not
    // confirmed yet - recognised and listed, no values
    { FO_FAMILY_LDS01, 19, "LDS01" },
    // Radio-clock time of a WH31 with a time receiver (rtl_433 ambientweather_wh31e.c,
    // "RCC", type 0x52; seen there from a WWVB WH31E, the DCF77 version - e.g. DNT -
    // is assumed to send the same): YY II UU YY MM DD HH mm SS CC XX, date and
    // time in BCD. Sent after a battery change and when the clock syncs.
    { FO_FAMILY_RCC,   11, "RCC" },
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

// WS85 / WS90: voltage of the solar-charged super capacitor, 0.1 V in the
// lower 6 bits; 0xFF = not reported (rtl_433 fineoffset_ws85.c / _ws90.c).
void setSupercap(FoFrame& o, uint8_t raw)
{
    if (raw == 0xFF) return;
    o.hasSupercap = true;
    o.supercapV   = (raw & 0x3F) * 0.1f;
}

void decodeWs80(const uint8_t* b, FoFrame& o)   // WS80 and WS90 (same first 14 bytes)
{
    o.id = be24(b + 1);
    uint32_t light = be16(b + 4);
    if (light != 0xFFFF) { o.hasLight = true; o.lightLux = light * 10.0f; }
    o.batteryMv = (uint16_t)(b[6] * 20);
    o.batteryOk = o.batteryMv > 2400;    // 2 x AA lithium (WS85 threshold of rtl_433)
    // WS90 firmware >= 1.6.0 (WS80 likely too) reports 0 V on an external power
    // supply (Ecowitt wiki by Gyvate, section WS90): not a flat battery
    if (b[6] == 0) { o.extPower = true; o.batteryOk = true; }
    int temp = (b[7] & 0x03) << 8 | b[8];
    if (temp != 0x3FF) setTemp400(o, temp);
    setHumidity(o, b[9]);
    setWind9(o, b[7], b[10], b[11], b[12]);
    if (b[13] != 0xFF) { o.hasUv = true; o.uvIndex = b[13] * 0.1f; }
}

// Air quality sensors: 4 bytes "Bp pp bA AA" - PM2.5 and PM10 as 14-bit
// values in 0.1 ug/m3, the battery level 0-5 split over the top bits
// (B: its bit 2, b: bits 1-0); 6 = powered via USB.
void setAirQuality(FoFrame& o, const uint8_t* p, bool pm10Estimated)
{
    int level = (p[0] & 0x40) >> 4 | (p[2] & 0xC0) >> 6;
    o.batteryOk     = level > 1;   // 1 of 5 bars = low
    o.extPower      = level == 6;
    o.hasPm25       = true;
    o.pm25          = ((p[0] & 0x3F) << 8 | p[1]) * 0.1f;
    o.hasPm10       = true;
    o.pm10          = ((p[2] & 0x3F) << 8 | p[3]) * 0.1f;
    o.pm10Estimated = pm10Estimated;
}

// 11-bit temperature (degC + 40) * 10 of WH45 / WH46
void setTemp11(FoFrame& o, const uint8_t* p)
{
    int raw = (p[0] & 0x07) << 8 | p[1];
    if (raw <= 1000) setTemp400(o, raw);   // -40..60 degC; above = no reading
}

// WH25 / WH32 / WH32B / WN32B / WN32P: no family byte, 8-bit sum only
//   TI IT TT HH PP PP SS XX
//   T: type nibble (0xE with pressure, 0xD outdoor), I: 8-bit ID,
//   flags (low battery, invalid), T: 10-bit temperature (degC + 40) * 10,
//   H: humidity, P: pressure 0.1 hPa (0xFFFF = none), S: sum of the 6 bytes
//   before it, X: XOR of those bytes with the nibbles swapped (WH25 only;
//   missing on WH32B / WN32B, so it is not required).
// The 8-bit sum is the only check: implausible values reject the frame.
bool decodeWh25(const uint8_t* b, size_t len, FoFrame& out)
{
    if (len < FO_WH25_FRAME_LEN || foSum8(b, 6) != b[6]) return false;
    uint8_t type = b[0] >> 4;
    if (type != 0xE && type != 0xD) return false;
    bool invalid = b[1] & 0x04;
    int temp     = (b[1] & 0x03) << 8 | b[2];
    uint8_t hum  = b[3];
    uint32_t p   = be16(b + 4);
    if (!invalid && temp > 1000) return false;                   // > 60 degC
    if (hum > 100 && hum != 0xFF) return false;
    if (p != 0xFFFF && (p < 3000 || p > 11000)) return false;    // 300..1100 hPa
    FoFrame o;
    o.family    = type == 0xE ? FO_FAMILY_WH25 : FO_FAMILY_WH32;
    o.model     = type == 0xE ? "WN32P" : "WN32";
    o.id        = (uint32_t)(b[0] & 0x0F) << 4 | b[1] >> 4;
    o.batteryOk = !(b[1] & 0x08);
    if (!invalid) {
        setTemp400(o, temp);
        if (hum) setHumidity(o, hum);
    }
    if (p != 0xFFFF) { o.hasPressure = true; o.pressureHpa = p * 0.1f; }
    if (!o.hasTemp && !o.hasHumidity && !o.hasPressure) return false;
    out = o;
    return true;
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
    if (!f) return decodeWh25(b, len, out);   // type nibble 0xD / 0xE (no family code uses them)
    if (len < f->len || !checkFrame(b, f->len)) return false;
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
            // 0 % is outside the WH31's range (1-99 %): treated as "no humidity",
            // e.g. a temperature-only sensor sending the WH31 frame (WN30)
            if (b[4]) setHumidity(o, b[4]);
            else if (f->code == FO_FAMILY_WH31) o.model = "WN30";   // temperature only: the WN30 sends this frame
            break;
        case FO_FAMILY_RCC: {
            auto bcd = [](uint8_t v, uint8_t mask) { v &= mask; return (int)((v >> 4) * 10 + (v & 0x0F)); };
            const int y = bcd(b[3], 0xFF), mo = bcd(b[4], 0x1F), d = bcd(b[5], 0x3F);
            const int h = bcd(b[6], 0x3F), mi = bcd(b[7], 0x7F), s = bcd(b[8], 0x7F);
            if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return false;
            o.id = b[1];
            o.hasClock = true;
            o.clockYear = (uint16_t)(2000 + y);
            o.clockMonth = (uint8_t)mo; o.clockDay = (uint8_t)d;
            o.clockHour = (uint8_t)h; o.clockMinute = (uint8_t)mi; o.clockSecond = (uint8_t)s;
            o.batteryKnown = false;
            break;
        }
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
            o.moistureAd  = (uint16_t)((b[7] & 0x01) << 8 | b[8]);   // raw AD (9 bit), for a two-point calibration
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
            setSupercap(o, b[21]);
            break;
        case FO_FAMILY_WS85:
            o.id        = be24(b + 1);
            o.batteryMv = (uint16_t)(b[4] * 20);
            o.batteryOk = o.batteryMv > 2400;
            setWind9(o, b[5], b[7], b[8], b[9]);
            o.hasRain = true;
            o.rainMm  = be16(b + 15) * 0.1f;
            setSupercap(o, b[17]);
            break;
        case FO_FAMILY_WH41: case FO_FAMILY_WH0290:   // FF II Bp pp bA AA CC SS
            o.id = b[1];
            setAirQuality(o, b + 2, true);
            break;
        case FO_FAMILY_WH43:                          // FF II II II Bp pp bA AA CC SS
            o.id = be24(b + 1);
            setAirQuality(o, b + 4, true);
            break;
        case FO_FAMILY_WH45: case FO_FAMILY_WH46:     // FF II II II 0T TT HH Bp pp bA AA CC CC [q q Q Q ? ?] CC SS
            o.id = be24(b + 1);
            setTemp11(o, b + 4);
            setHumidity(o, b[6]);
            setAirQuality(o, b + 7, false);
            o.hasCo2 = true;
            o.co2Ppm = (uint16_t)be16(b + 11);
            if (f->code == FO_FAMILY_WH46) {
                o.hasPm1 = true;  o.pm1 = (be16(b + 13) & 0x3FFF) * 0.1f;
                o.hasPm4 = true;  o.pm4 = (be16(b + 15) & 0x3FFF) * 0.1f;
            }
            break;
        case FO_FAMILY_WH55:                          // 55 fC II II BB RR RR SA CC SS
            // f: 2 flag bits (seen 0) and the channel 0-3; the ID as the
            // Ecowitt app shows it includes the channel bits (e.g. 30CF55 = CH4)
            // battery level 1-5: an extra check against other frames that start
            // with 0x55 and carry a CRC-8 at byte 8 (WH1050 / TFA 30.3151 FSK)
            if (b[4] > 6) return false;
            o.id        = (uint32_t)(b[1] & 0x3F) << 16 | be16(b + 2);
            o.channel   = (uint8_t)(((b[1] >> 4) & 0x03) + 1);
            o.batteryOk = b[4] > 1;
            o.hasLeak   = true;
            o.leak      = b[7] & 0x40;
            o.leakHighSensitivity = b[7] & 0x80;
            o.leakRaw   = (uint16_t)be16(b + 5);
            break;
        case FO_FAMILY_WN35:                          // 35 II II II RR RR RR WW VV CC SS
            o.id        = be24(b + 1);
            o.batteryMv = (uint16_t)(b[8] * 20);
            o.batteryOk = o.batteryMv > 1200;
            if (b[7] <= 100) { o.hasLeafWetness = true; o.leafWetnessPct = b[7]; }
            break;
        case FO_FAMILY_WH52: {                        // A2 II II II Bt tt MM .. mE EE EE .. (calibration) .. CC SS
            o.id = be24(b + 1);
            int temp = (b[4] & 0x1F) << 8 | b[5];     // bits 7-5 of b[4]: transmission boost counter
            if (temp <= 1000) setTemp400(o, temp);
            if (b[6] <= 100) { o.hasMoisture = true; o.moisture = b[6]; }
            o.hasConductivity  = true;                // 20 bit / 25.6 (empirical calibration)
            o.conductivityUsCm = (float)((uint32_t)(b[8] & 0x0F) << 16 | be16(b + 9)) / 25.6f;
            // battery: not decoded (the published byte / scaling are not settled)
            break;
        }
        case FO_FAMILY_LDS01:                         // 54 II II II then 13 bytes not decoded yet, CC SS
            o.id = be24(b + 1);
            o.batteryKnown = false;
            break;
        default:
            return false;
    }
    out = o;
    return true;
}
