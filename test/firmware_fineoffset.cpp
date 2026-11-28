// Fine Offset (Ecowitt / Froggit) frame decoder (lib/FineOffsetSensorReceiver),
// the shared IT+ / Fine Offset 17.241 kbit/s dispatch and the table mapping
// (src/rf_fsk17.h, src/rf_sensors_data.cpp). Frames: the published examples of
// the rtl_433 decoder documentation (bytes after the 2D D4 sync); others are
// built from them with seal().
#include "FineOffsetDecoder.h"
#include "ItPlusDecoder.h"
#include "rf_fsk17.h"
#include "config.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "config.h"
uint32_t lsRandom() { static uint32_t n = 0x100; return n += 0x111; }   // lsensors.h (new ★ entries)
void lsLock() {}     // the table's lock (lsensors.h): single-threaded here
void lsUnlock() {}
Config gCfg;   // rf_sensors_data.cpp reads the sensor lists (runtime.cpp is not linked here)
unsigned long hostNow = 0;   // millis() of rf_stats.cpp

static bool near(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// CRC-8 at n-2 and sum at n-1 for a frame of n bytes
static void seal(uint8_t* b, size_t n)
{
    b[n - 2] = foCrc8(b, n - 2);
    b[n - 1] = foSum8(b, n - 1);
}

// Hex string ("42cc40 5a..", spaces ignored) -> bytes; returns the count
static size_t hex(const char* h, uint8_t* out)
{
    size_t n = 0;   // nibbles
    for (; *h; h++) {
        if (*h == ' ') continue;
        int v = *h <= '9' ? *h - '0' : (*h | 0x20) - 'a' + 10;
        if (n % 2 == 0) out[n / 2] = (uint8_t)(v << 4); else out[n / 2] |= (uint8_t)v;
        n++;
    }
    return n / 2;
}

// Decodes a published frame (hex) as the receiver delivers it (32 bytes,
// noise after the frame)
static bool dec(const char* h, FoFrame& f)
{
    uint8_t fr[FO_MAX_FRAME_LEN], buf[FO_MAX_FRAME_LEN];
    size_t n = hex(h, fr);
    for (size_t i = 0; i < FO_MAX_FRAME_LEN; i++) buf[i] = (uint8_t)(0x5A ^ (i * 37));
    memcpy(buf, fr, n);
    return foDecode(buf, FO_MAX_FRAME_LEN, f);
}

// A received frame: always FO_MAX_FRAME_LEN bytes, noise after the real frame
static size_t rx(uint8_t* buf, const uint8_t* frame, size_t n)
{
    for (size_t i = 0; i < FO_MAX_FRAME_LEN; i++) buf[i] = (uint8_t)(0x5A ^ (i * 37));
    memcpy(buf, frame, n);
    return FO_MAX_FRAME_LEN;
}

int main()
{
    FoFrame f;
    uint8_t buf[FO_MAX_FRAME_LEN];

    // ---- 0x24: WH24 / WH65 / WS69 ----
    const uint8_t wh24[] = {0x24, 0xbf, 0x0a, 0xe2, 0x06, 0x4e, 0x08, 0x02, 0x00, 0x4a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8f, 0x07};
    assert(foFrameLength(0x24) == sizeof(wh24));
    assert(foCrc8(wh24, 15) == 0x8f && foSum8(wh24, 16) == 0x07);
    // rtl_433 reading of this frame (WH24 units): 11.8 C, 78 %, 266 deg, 1.12 / 2.24 m/s, 22.2 mm
    assert(foDecode(buf, rx(buf, wh24, sizeof(wh24)), f, true));
    assert(!strcmp(f.model, "WH24") && f.family == 0x24 && f.id == 0xbf && f.batteryOk);
    assert(f.hasTemp && near(f.tempC, 11.8f) && f.hasHumidity && f.humidity == 78);
    assert(f.hasWind && f.windDirDeg == 266 && near(f.windAvgMs, 1.12f) && near(f.windGustMs, 2.24f));
    assert(f.hasRain && near(f.rainMm, 22.2f, 0.01f));
    assert(f.hasUv && f.uvIndex == 0.0f && f.hasLight && f.lightLux == 0.0f && !f.hasPressure);
    // default: WH65 / WS69 units (0.51 m/s, 0.254 mm)
    assert(foDecode(wh24, sizeof(wh24), f) && !strcmp(f.model, "WH65"));
    assert(near(f.windAvgMs, 0.51f) && near(f.windGustMs, 1.02f) && near(f.rainMm, 74 * 0.254f, 0.01f));
    assert(rfFineOffsetId(f) == 0x240000BFUL);

    const uint8_t wh65[] = {0x24, 0x50, 0x67, 0xe2, 0x87, 0x41, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8c, 0x14};
    assert(foDecode(buf, rx(buf, wh65, sizeof(wh65)), f));
    assert(f.id == 0x50 && near(f.tempC, 24.7f) && f.humidity == 65 && f.windDirDeg == 359);
    assert(near(f.windAvgMs, 0.0f) && near(f.rainMm, 0.254f) && f.uvIndex == 0.0f);

    // WS69: 8 more bytes with pressure (here 0x1FFFF = no pressure module)
    uint8_t ws69[] = {0x24, 0x1c, 0x88, 0x01, 0xca, 0x63, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0xd5,
                      0x01, 0xff, 0xff, 0xff, 0x00, 0x2f, 0x83, 0x5a};
    assert(foDecode(buf, rx(buf, ws69, sizeof(ws69)), f));
    assert(!strcmp(f.model, "WS69") && f.id == 0x1c && near(f.tempC, 5.8f) && f.humidity == 99);
    assert(f.windDirDeg == 136 && near(f.rainMm, 51 * 0.254f, 0.01f) && !f.hasPressure);
    ws69[17] = 0x01; ws69[18] = 0x8B; ws69[19] = 0xCD;   // 101325 Pa
    seal(ws69, sizeof(ws69));
    assert(foDecode(ws69, sizeof(ws69), f) && f.hasPressure && near(f.pressureHpa, 1013.25f, 0.01f));
    ws69[24] ^= 1;   // bad extension: still a valid WH65 frame, no pressure
    assert(foDecode(ws69, sizeof(ws69), f) && !strcmp(f.model, "WH65") && !f.hasPressure);
    // extension whose checks pass by chance (noise after a WH65 frame in the
    // fixed-length read) but whose pressure is no station pressure: not a WS69
    for (uint32_t pa : { 0u, 29999u, 110001u, 0x1FFFEu }) {
        ws69[17] = (uint8_t)(pa >> 16); ws69[18] = (uint8_t)(pa >> 8); ws69[19] = (uint8_t)pa;
        seal(ws69, sizeof(ws69));
        assert(foDecode(ws69, sizeof(ws69), f) && !strcmp(f.model, "WH65") && !f.hasPressure);
    }
    for (uint32_t pa : { 30000u, 110000u }) {   // 300 / 1100 hPa: the limits
        ws69[17] = (uint8_t)(pa >> 16); ws69[18] = (uint8_t)(pa >> 8); ws69[19] = (uint8_t)pa;
        seal(ws69, sizeof(ws69));
        assert(foDecode(ws69, sizeof(ws69), f) && !strcmp(f.model, "WS69") && f.hasPressure && near(f.pressureHpa, pa * 0.01f, 0.01f));
    }

    // invalid markers, negative temperature, low battery, 9-bit wind direction
    uint8_t inv[17];
    memcpy(inv, wh65, sizeof(inv));
    inv[3] = 0x80 | 0x10 | 0x08 | 0x07; inv[4] = 0xff;   // dir MSB, wind MSB, low battery, temp 0x7FF
    inv[2] = 0xff; inv[6] = 0xff; inv[5] = 0xff;         // dir 0x1FF, wind 0x1FF, humidity 0xFF
    inv[10] = inv[11] = 0xff; inv[12] = inv[13] = inv[14] = 0xff;
    seal(inv, sizeof(inv));
    assert(foDecode(inv, sizeof(inv), f) && !f.batteryOk);
    assert(!f.hasTemp && !f.hasHumidity && !f.hasWind && !f.hasUv && !f.hasLight && f.hasRain);
    RfValues v = rfFineOffsetValues(f);
    assert(v.ok == RF_VAL_RAIN);
    memcpy(inv, wh65, sizeof(inv));
    inv[3] = 0x01; inv[4] = 0x2c;                        // 300 -> -10.0 C
    inv[10] = 0x0a; inv[11] = 0x00;                      // UV 2560 -> index 6
    inv[12] = 0x01; inv[13] = 0x86; inv[14] = 0xa0;      // 100000 -> 10000 lux
    seal(inv, sizeof(inv));
    assert(foDecode(inv, sizeof(inv), f) && near(f.tempC, -10.0f) && f.uvIndex == 6.0f && near(f.lightLux, 10000.0f, 0.1f));

    // ---- WH31 (DP50 / WN31) ----
    const uint8_t wh31[] = {0x30, 0xc3, 0x81, 0xd5, 0x5c, 0x2a, 0xcf};
    assert(foDecode(buf, rx(buf, wh31, sizeof(wh31)), f));
    assert(!strcmp(f.model, "WH31") && f.id == 0xc3 && f.channel == 1 && f.batteryOk);
    assert(near(f.tempC, 6.9f) && f.humidity == 92 && !f.hasWind && !f.hasRain);
    assert(rfFineOffsetId(f) == 0x300001C3UL);
    const uint8_t wh31b[] = {0x30, 0x35, 0xc2, 0x2f, 0x3c, 0x0f, 0xa1};
    assert(foDecode(wh31b, sizeof(wh31b), f) && f.channel == 5 && near(f.tempC, 15.9f) && f.humidity == 60);
    uint8_t wh31n[] = {0x37, 0x12, (uint8_t)(0x70 | 0x04 | 0x00), 0xfa, 40, 0, 0};   // WH31B, ch 8, low battery, 250 -> -15.0 C
    seal(wh31n, sizeof(wh31n));
    assert(foDecode(wh31n, sizeof(wh31n), f) && !strcmp(f.model, "WH31B") && f.channel == 8 && !f.batteryOk);
    assert(near(f.tempC, -15.0f) && f.humidity == 40 && rfFineOffsetId(f) == 0x37000812UL);
    v = rfFineOffsetValues(f);
    assert(v.ok == (RF_VAL_TEMP | RF_VAL_HUM) && near(v.tempC, -15.0f) && v.humidity == 40);

    // ---- WH40 rain gauge ----
    const uint8_t wh40[] = {0x40, 0x00, 0xcd, 0x6f, 0x10, 0x00, 0x0a, 0xbf, 0x55};
    assert(foDecode(buf, rx(buf, wh40, sizeof(wh40)), f));
    assert(f.id == 0x0cd6f && f.batteryMv == 1600 && f.batteryOk && f.hasRain && near(f.rainMm, 1.0f) && !f.hasTemp);
    assert(rfFineOffsetId(f) == 0x4000CD6FUL && rfFineOffsetValues(f).ok == RF_VAL_RAIN);

    // ---- WN20 rain gauge ----
    const uint8_t wn20[] = {0x20, 0x00, 0x2c, 0x87, 0x98, 0x00, 0x13, 0x04, 0x9e, 0x20};
    assert(foDecode(buf, rx(buf, wn20, sizeof(wn20)), f));
    assert(f.id == 0x2c87 && f.batteryMv == 3040 && f.batteryOk && near(f.rainMm, 1.9f));

    // ---- WH57 lightning ----
    const uint8_t wh57s[] = {0x57, 0x81, 0x05, 0xc8, 0x05, 0x81, 0x01, 0xdf, 0x0b};
    assert(foDecode(buf, rx(buf, wh57s, sizeof(wh57s)), f));
    assert(f.id == 0x105c8 && f.hasLightning && f.lightningState == 8 && f.lightningCount == 1);
    assert(f.stormDistanceKm == 1 && f.batteryOk && rfFineOffsetValues(f).ok == 0);
    const uint8_t wh57r[] = {0x57, 0x01, 0x05, 0xc8, 0x05, 0xbf, 0x00, 0xdd, 0xc6};
    assert(foDecode(wh57r, sizeof(wh57r), f) && f.lightningState == 0 && f.stormDistanceKm == FO_LIGHTNING_NO_DISTANCE);

    // ---- WH51 soil moisture ----
    const uint8_t wh51[] = {0x51, 0x00, 0x6b, 0x58, 0x6e, 0x7f, 0x24, 0xf8, 0xd2, 0xff, 0xff, 0xff, 0x3c, 0x28};
    assert(foDecode(buf, rx(buf, wh51, sizeof(wh51)), f));
    assert(f.id == 0x006b58 && f.hasMoisture && f.moisture == 36 && f.batteryMv == 1400 && f.batteryOk);
    assert(rfFineOffsetValues(f).ok == 0 && rfFineOffsetId(f) == 0x51006B58UL);

    // ---- WN34 / WN38 temperature probes ----
    const uint8_t wn34[] = {0x34, 0x00, 0x29, 0x65, 0x02, 0x85, 0x44, 0x66, 0xf3};
    assert(foDecode(buf, rx(buf, wn34, sizeof(wn34)), f));
    assert(f.id == 0x002965 && near(f.tempC, 24.5f) && f.batteryMv == 1360 && f.batteryOk && !f.hasHumidity);
    const uint8_t wn38[] = {0x38, 0x00, 0x28, 0x85, 0x40, 0xf5, 0x41, 0x4c, 0xa7};
    assert(foDecode(wn38, sizeof(wn38), f) && !strcmp(f.model, "WN38") && near(f.tempC, 24.5f));
    uint8_t wn34d[] = {0x34, 0x01, 0x02, 0x03, 0x4D, 0xDA, 0x3c, 0, 0};   // WN34D (sub-type 4): -55.0 C, 1200 mV
    seal(wn34d, sizeof(wn34d));
    assert(foDecode(wn34d, sizeof(wn34d), f) && near(f.tempC, -55.0f) && !f.batteryOk);

    // ---- WS80 ----
    const uint8_t ws80[] = {0x80, 0x0a, 0x00, 0x3b, 0x00, 0x00, 0x88, 0x8a, 0x59, 0x38, 0x18, 0x6d, 0x1c, 0x00, 0xff, 0xff, 0xd8, 0xdf};
    assert(foDecode(buf, rx(buf, ws80, sizeof(ws80)), f));
    assert(f.id == 0x0a003b && f.batteryMv == 2720 && f.batteryOk && near(f.tempC, 20.1f) && f.humidity == 56);
    assert(f.hasWind && near(f.windAvgMs, 2.4f) && near(f.windGustMs, 2.8f) && f.windDirDeg == 109);
    assert(f.hasUv && f.uvIndex == 0.0f && f.hasLight && f.lightLux == 0.0f && !f.hasRain);
    v = rfFineOffsetValues(f);
    assert(v.ok == (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_UV | RF_VAL_LIGHT) && near(v.windDirDeg, 109.0f));

    // ---- WS90 (family 0x90: starts with the IT+ model nibble 9) ----
    const uint8_t ws90[] = {0x90, 0x00, 0x34, 0x2b, 0x00, 0x77, 0xa4, 0x82, 0x62, 0x39, 0x00, 0x3e, 0x00, 0x00, 0x3f, 0xff,
                            0x20, 0x00, 0xba, 0x00, 0x00, 0x26, 0x02, 0x00, 0xff, 0x9f, 0xf8, 0x00, 0x00, 0x82, 0x92, 0x4f};
    assert(sizeof(ws90) == FO_MAX_FRAME_LEN);
    assert(foDecode(ws90, sizeof(ws90), f));
    assert(f.id == 0x00342b && near(f.lightLux, 1190.0f) && f.batteryMv == 3280 && near(f.tempC, 21.0f));
    assert(f.humidity == 57 && f.windDirDeg == 62 && f.windAvgMs == 0.0f && f.hasRain && f.rainMm == 0.0f);
    assert(f.hasSupercap && near(f.supercapV, 3.8f));   // b[21] = 0x26
    assert(rfFineOffsetValues(f).ok == (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_RAIN | RF_VAL_UV | RF_VAL_LIGHT));
    assert(!foDecode(ws90, 31, f));                     // truncated
    uint8_t ws90r[32];
    memcpy(ws90r, ws90, sizeof(ws90r));
    ws90r[19] = 0x04; ws90r[20] = 0xd2;                 // 1234 -> 123.4 mm
    ws90r[7] |= 0x10 | 0x20 | 0x40; ws90r[10] = 0x2c; ws90r[11] = 0x0b; ws90r[12] = 0x90;   // 30.0 m/s, 267 deg, 40.0 m/s
    ws90r[8] = 0x00; ws90r[7] &= (uint8_t)~0x03;        // 0 -> -40.0 C
    seal(ws90r, sizeof(ws90r));
    assert(foDecode(ws90r, sizeof(ws90r), f) && near(f.rainMm, 123.4f, 0.01f) && near(f.tempC, -40.0f));
    assert(near(f.windAvgMs, 30.0f) && f.windDirDeg == 267 && near(f.windGustMs, 40.0f));
    assert(!f.extPower && f.batteryOk);
    // WS90 firmware >= 1.6.0 on an external supply: battery 0 V = external power, not a flat battery
    ws90r[6] = 0x00; seal(ws90r, sizeof(ws90r));
    assert(foDecode(ws90r, sizeof(ws90r), f) && f.batteryMv == 0 && f.extPower && f.batteryOk);
    {
        RfSensorReading x;
        rfFineOffsetExtras(x, f);
        assert(x.extPower);
    }

    // ---- WS85 ----
    const uint8_t ws85[] = {0x85, 0x00, 0x28, 0xEB, 0x87, 0x82, 0x6F, 0x00, 0x83, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x0B, 0x00, 0x00, 0xFF, 0xEF, 0xFD, 0x00, 0x00, 0x6B, 0xDD, 0x0F};
    assert(foFrameLength(0x85) == sizeof(ws85));
    assert(foDecode(buf, rx(buf, ws85, sizeof(ws85)), f));
    assert(f.id == 0x0028eb && f.batteryMv == 2700 && f.batteryOk && f.windDirDeg == 131 && f.hasRain && !f.hasTemp);
    assert(f.hasSupercap && near(f.supercapV, 1.1f));   // b[17] = 0x0B

    // ---- WS68 ----
    const uint8_t ws68[] = {0x68, 0x00, 0x00, 0xc5, 0x01, 0x07, 0x4b, 0x0f, 0xff, 0xff, 0x00, 0x2e, 0x00, 0x02, 0xa6, 0x63};
    assert(foDecode(buf, rx(buf, ws68, sizeof(ws68)), f));
    assert(f.id == 0xc5 && near(f.lightLux, 2630.0f) && f.windDirDeg == 46 && near(f.uvIndex, 0.2f) && f.batteryOk);
    const uint8_t ws68w[] = {0x68, 0x00, 0x00, 0xc5, 0x00, 0x00, 0x4b, 0x0f, 0xff, 0xff, 0x7e, 0xe0, 0x94, 0x00, 0x75, 0xec};
    assert(foDecode(ws68w, sizeof(ws68w), f) && near(f.windAvgMs, 12.6f) && f.windDirDeg == 224 && near(f.windGustMs, 14.8f));

    // ==== Families added from the published frames (hex vectors: rtl_433
    // decoder documentation, rtl_433_tests readme, rtl_433 issues #2756 / #3614
    // / #3665 and PR #3679; every one re-checked: CRC-8 + sum) ====

    // ---- 0x42 WH0290 / Ambient PM25, 0x41 WH41: PM2.5 (+ PM10 estimate) ----
    struct { const char* h; float pm25, pm10; } wh0290[] = {
        { "42cc405a40641561",  9.0f, 10.0f }, { "42cc4064406ee444", 10.0f, 11.0f }, { "42cc406e4078df53", 11.0f, 12.0f },
        { "42cc40784082870f", 12.0f, 13.0f }, { "42cc408c4096d383", 14.0f, 15.0f }, { "42cc419a41aec199", 41.0f, 43.0f } };
    for (auto& t : wh0290) {
        assert(dec(t.h, f) && f.family == FO_FAMILY_WH0290 && !strcmp(f.model, "WH0290") && f.id == 0xcc);
        assert(f.hasPm25 && near(f.pm25, t.pm25) && f.hasPm10 && near(f.pm10, t.pm10) && f.pm10Estimated);
        assert(f.batteryOk && !f.extPower && !f.hasTemp && !f.hasCo2 && !f.hasPm1);   // 5 bars
        assert(rfFineOffsetId(f) == 0x420000CCUL);
        assert(rfFineOffsetValues(f).ok == 0);   // no value-source group
    }
    assert(dec("41c741ae01c2f9b3", f) && !strcmp(f.model, "WH41") && f.id == 0xc7);
    assert(near(f.pm25, 43.0f) && near(f.pm10, 45.0f) && f.batteryOk);                         // 4 bars
    {
        uint8_t a[8];
        hex("41c741ae01c2f9b3", a);
        a[2] = 0x01; a[4] = 0x41; seal(a, 8);            // level 1 (low): bit 2 clear, bits 1-0 = 01
        assert(foDecode(a, 8, f) && !f.batteryOk && !f.extPower && near(f.pm25, 0.1f * 0x01ae));
        a[2] = 0x41; a[4] = 0x81; seal(a, 8);            // level 6 = USB power
        assert(foDecode(a, 8, f) && f.batteryOk && f.extPower);
        a[3] ^= 0x01;                                    // CRC
        assert(!foDecode(a, 8, f));
    }
    assert(!dec("42cc405a40641562", f) && !dec("42cc405a40641461", f));                          // sum, CRC

    // ---- 0x43 WH43 (no real frame published: built from the documented layout) ----
    {
        uint8_t a[10] = {0x43, 0x00, 0x12, 0x34, 0x40, 0x5a, 0x40, 0x64, 0, 0};
        seal(a, sizeof(a));
        assert(foDecode(a, sizeof(a), f) && !strcmp(f.model, "WH43") && f.id == 0x1234);
        assert(near(f.pm25, 9.0f) && near(f.pm10, 10.0f) && f.pm10Estimated && f.batteryOk);
        assert(foFrameLength(FO_FAMILY_WH43) == 10);
    }

    // ---- 0x45 WH45 / DP250: T, H, PM2.5, PM10, CO2 ----
    assert(dec("45003660027e3640230029022907 4f", f) && !strcmp(f.model, "WH45") && f.id == 0x003660);
    assert(f.hasTemp && near(f.tempC, 23.8f) && f.hasHumidity && f.humidity == 54);
    assert(near(f.pm25, 3.5f) && near(f.pm10, 4.1f) && !f.pm10Estimated && f.hasCo2 && f.co2Ppm == 553);
    assert(f.batteryOk && !f.extPower && !f.hasPm1 && !f.hasPm4 && rfFineOffsetId(f) == 0x45003660UL);
    assert(rfFineOffsetValues(f).ok == (RF_VAL_TEMP | RF_VAL_HUM));
    assert(!dec("45003660027e3640230029022907 4e", f) && !dec("45003660027e3640230029022906 4f", f));

    // ---- 0x46 WH46: as WH45 plus PM1 and PM4 ----
    assert(dec("460027f102b533403240390 30b002a00360190e416", f) && !strcmp(f.model, "WH46") && f.id == 0x0027f1);
    assert(near(f.tempC, 29.3f) && f.humidity == 51 && near(f.pm25, 5.0f) && near(f.pm10, 5.7f) && f.co2Ppm == 779);
    assert(f.hasPm1 && near(f.pm1, 4.2f) && f.hasPm4 && near(f.pm4, 5.4f) && f.batteryOk);
    assert(!dec("460027f102b533403240390 30b002a00360191e416", f));

    // ---- 0x55 WH55 leak: channel, 20-bit ID (with the channel bits, as Ecowitt shows it), alarm ----
    struct { const char* h; uint8_t ch; uint32_t id; uint8_t bat; uint16_t raw; bool high, leak; } wh55[] = {
        { "5530cf55040289beaea42010", 4, 0x30cf55, 4, 0x0289, true,  false },   // + 2 trailing bytes (ignored)
        { "5510cf55030292be674590",   2, 0x10cf55, 3, 0x0292, true,  false },
        { "5510cf5503010bfe1eb418",   2, 0x10cf55, 3, 0x010b, true,  true  },
        { "5510cf5503029abe503610",   2, 0x10cf55, 3, 0x029a, true,  false },
        { "5510cf550302933ee94810",   2, 0x10cf55, 3, 0x0293, false, false },
        { "5510cf5503010b7e647a10",   2, 0x10cf55, 3, 0x010b, false, true  },
        { "5500cf55030299bef2c7",     1, 0x00cf55, 3, 0x0299, true,  false },
        { "5520cf55030295be695a",     3, 0x20cf55, 3, 0x0295, true,  false },
        { "5530cf55030299be5257",     4, 0x30cf55, 3, 0x0299, true,  false },
        { "5500cf550302983e7cd0",     1, 0x00cf55, 3, 0x0298, false, false },
        { "5500cf5503017bfeaaa0",     1, 0x00cf55, 3, 0x017b, true,  true  } };
    for (auto& t : wh55) {
        assert(dec(t.h, f) && f.family == FO_FAMILY_WH55 && !strcmp(f.model, "WH55"));
        assert(f.channel == t.ch && f.id == t.id && f.batteryOk && f.hasLeak && f.leak == t.leak);
        assert(f.leakHighSensitivity == t.high && f.leakRaw == t.raw);
        assert(rfFineOffsetId(f) == (0x55000000UL | t.id));   // not the WH31 channel form
    }
    {
        uint8_t a[12];
        hex("5530cf55040289beaea42010", a);
        a[10] = 0x00; a[11] = 0xff;                      // trailing bytes: not part of the check
        assert(foDecode(a, sizeof(a), f) && f.leakRaw == 0x0289);
        a[9] ^= 0x01;                                    // sum
        assert(!foDecode(a, sizeof(a), f));
        hex("5530cf55040289beaea42010", a);
        a[4] = 0x01; seal(a, 10);                        // 1 bar = low
        assert(foDecode(a, sizeof(a), f) && !f.batteryOk);
        a[4] = 0x07; seal(a, 10);                        // no battery level: rejected
        assert(!foDecode(a, sizeof(a), f));
    }

    // ---- 0x35 WN35 leaf wetness ----
    struct { const char* h; uint32_t id; uint8_t pct; uint16_t mv; } wn35[] = {
        { "3500305e010655075570eb", 0x00305e, 7, 1700 },  { "3500305e01061c00540943", 0x00305e, 0, 1680 },
        { "3500305e01061d00544f8a", 0x00305e, 0, 1680 },  { "35004139010588005661f4", 0x004139, 0, 1720 },
        { "3500413901074c0e56056c", 0x004139, 14, 1720 }, { "350041390109682356dc76", 0x004139, 35, 1720 },
        { "350041390109d62756a0ac", 0x004139, 39, 1720 } };
    for (auto& t : wn35) {
        assert(dec(t.h, f) && !strcmp(f.model, "WN35") && f.id == t.id);
        assert(f.hasLeafWetness && f.leafWetnessPct == t.pct && f.batteryMv == t.mv && f.batteryOk && !f.hasTemp);
    }
    assert(!dec("3500305e010655075570ea", f) && !dec("3500305e010655075571eb", f));

    // ---- 0xA2 WH52 soil moisture / temperature / EC (battery not decoded) ----
    assert(dec("a2005d8d026f5b166040562668459358805e7b2941088471", f) && !strcmp(f.model, "WH52") && f.id == 0x005d8d);
    assert(near(f.tempC, 22.3f) && f.hasMoisture && f.moisture == 91 && f.hasConductivity && near(f.conductivityUsCm, 643.4f, 0.1f));
    assert(f.batteryOk && f.batteryMv == 0 && !f.hasHumidity && rfFineOffsetValues(f).ok == RF_VAL_TEMP);
    assert(dec("a2005d8d026e4a625344989668459358805e7b2941088757", f));
    assert(near(f.tempC, 22.2f) && f.moisture == 74 && near(f.conductivityUsCm, 8365.9f, 0.1f));
    assert(!dec("a2005d8d026e4a625344989668459358805e7b2941088758", f));
    assert(!dec("a2005d8d028000462000821668459358805e7b2942080f70", f));   // a published frame with a typo

    // ---- WH25 / WH32 family: type nibble, sum + plausibility (no CRC) ----
    assert(dec("e50272282721c9bb", f) && f.family == FO_FAMILY_WH25 && !strcmp(f.model, "WN32P") && f.id == 0x50);
    assert(f.batteryOk && near(f.tempC, 22.6f) && f.humidity == 40 && f.hasPressure && near(f.pressureHpa, 1001.7f, 0.01f));
    assert(rfFineOffsetId(f) == 0x25000050UL && rfFineOffsetValues(f).ok == (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_PRESS));   // pressure: value source group
    assert(dec("e50272282721c900", f));                    // XOR missing (WH32B / WN32B): accepted
    assert(!dec("e50272282721c8bb", f));                   // sum
    {
        // outdoor WH32 / WN32: type 0xD, no pressure (FFFF) - the values of rtl_433_tests fineoffset_wh32
        uint8_t a[8] = {0xdf, 0x82, 0x68, 54, 0xff, 0xff, 0, 0};   // ID 0xF8, 21.6 C
        a[6] = foSum8(a, 6);
        assert(foDecode(a, 8, f) && f.family == FO_FAMILY_WH32 && !strcmp(f.model, "WN32") && f.id == 0xf8);
        assert(near(f.tempC, 21.6f) && f.humidity == 54 && !f.hasPressure && f.batteryOk);
        assert(rfFineOffsetId(f) == 0x320000F8UL);
        a[1] |= 0x08; a[6] = foSum8(a, 6);                 // low battery
        assert(foDecode(a, 8, f) && !f.batteryOk);
        uint8_t c[8];
        memcpy(c, a, 8); c[0] = 0xcf; c[6] = foSum8(c, 6); // type nibble C: not this family
        assert(!foDecode(c, 8, f));
        memcpy(c, a, 8); c[3] = 150; c[6] = foSum8(c, 6);  // humidity 150 %
        assert(!foDecode(c, 8, f));
        memcpy(c, a, 8); c[1] = 0x83; c[2] = 0xf2; c[6] = foSum8(c, 6);   // 1010 = 61 degC
        assert(!foDecode(c, 8, f));
        memcpy(c, a, 8); c[4] = 0x07; c[5] = 0xd0; c[6] = foSum8(c, 6);   // 200 hPa
        assert(!foDecode(c, 8, f));
        memcpy(c, a, 8); c[4] = 0x27; c[5] = 0x21; c[6] = foSum8(c, 6);   // 1001.7 hPa: plausible
        assert(foDecode(c, 8, f) && f.hasPressure);
        memcpy(c, a, 8); c[1] |= 0x04; c[6] = foSum8(c, 6);               // invalid flag, no pressure: nothing left
        assert(!foDecode(c, 8, f));
        memcpy(c, a, 8); c[3] = 0; c[6] = foSum8(c, 6);                   // humidity 0: none
        assert(foDecode(c, 8, f) && f.hasTemp && !f.hasHumidity);
        assert(!foDecode(a, 7, f));                                       // too short
    }
    // the D/E type nibble is no IT+ frame (model nibble 9) and no family code
    {
        ItPlusFrame it2;
        uint8_t b2[FO_MAX_FRAME_LEN];
        hex("e50272282721c9bb", b2);
        assert(rfFsk17Decode(b2, 8, true, true, f, it2) == RF_FSK_FINEOFFSET && f.hasPressure);
        assert(foFrameLength(0xe5) == 0 && foFrameLength(FO_FAMILY_WH25) == 0);
    }

    // ---- WH31 frame with 0 % humidity (temperature-only WN30): no humidity ----
    {
        uint8_t a[7];
        memcpy(a, wh31, sizeof(a)); a[4] = 0; seal(a, sizeof(a));
        assert(foDecode(a, sizeof(a), f) && f.hasTemp && !f.hasHumidity);
        assert(rfFineOffsetValues(f).ok == RF_VAL_TEMP);
    }

    // ---- table entry: the per-sensor values ----
    {
        RfSensorReading e;
        assert(dec("460027f102b533403240390 30b002a00360190e416", f));
        rfFineOffsetExtras(e, f);
        assert(e.hasPm25 && near(e.pm25, 5.0f) && e.hasPm1 && e.hasPm4 && e.hasCo2 && e.co2Ppm == 779 && !e.pm10Estimated);
        assert(!e.hasLeak && !e.hasPressure && !e.extPower);
        assert(dec("5510cf5503010bfe1eb418", f));
        rfFineOffsetExtras(e, f);
        assert(e.hasLeak && e.leak && e.leakHighSensitivity && e.leakRaw == 0x010b);
        assert(dec("e50272282721c9bb", f));
        rfFineOffsetExtras(e, f);
        assert(!e.hasPressure);                            // pressure is a value group now: via rfStoreValues()
        unsigned long gap;
        rfStoreValues(e, rfFineOffsetValues(f), 1000, gap);
        assert(e.hasPressure && near(e.pressureHpa, 1001.7f, 0.01f) && e.pressMs == 1000);
    }

    // ---- rejections ----
    uint8_t bad[sizeof(ws80)];
    memcpy(bad, ws80, sizeof(bad)); bad[16] ^= 0x01;    // CRC
    assert(!foDecode(bad, sizeof(bad), f));
    memcpy(bad, ws80, sizeof(bad)); bad[17] ^= 0x01;    // sum
    assert(!foDecode(bad, sizeof(bad), f));
    memcpy(bad, ws80, sizeof(bad)); bad[9] ^= 0x01;     // payload
    assert(!foDecode(bad, sizeof(bad), f));
    uint8_t unk[9] = {0x4F, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0, 0};   // unknown family code
    seal(unk, sizeof(unk));
    assert(!foDecode(unk, sizeof(unk), f) && foFrameLength(0x4F) == 0);
    assert(!foDecode(wh31, 6, f) && !foDecode(wh31, 0, f));

    // ---- shared 17.241 kbit/s phase: IT+ and Fine Offset from the same frame buffer ----
    ItPlusFrame it;
    const uint8_t tx29[] = {0x92, 0x84, 0x48, 0x6A, 0xEC};   // published TX29-IT frame
    size_t n = rx(buf, tx29, sizeof(tx29));
    assert(rfFsk17Decode(buf, n, true, true, f, it) == RF_FSK_ITPLUS && it.sensorId == 10);
    assert(rfFsk17Decode(buf, n, true, false, f, it) == RF_FSK_NONE);       // Fine Offset only
    assert(rfFsk17Decode(buf, ITPLUS_FRAME_LEN, false, true, f, it) == RF_FSK_ITPLUS);   // IT+ only, 5-byte mode
    n = rx(buf, wh31, sizeof(wh31));
    assert(rfFsk17Decode(buf, n, true, true, f, it) == RF_FSK_FINEOFFSET && f.channel == 1);
    assert(rfFsk17Decode(buf, n, false, true, f, it) == RF_FSK_NONE);       // IT+ only: family 0x30 is no IT+ frame
    assert(rfFsk17Decode(ws90, sizeof(ws90), true, true, f, it) == RF_FSK_FINEOFFSET && f.family == FO_FAMILY_WS90);
    assert(rfFsk17Decode(ws90, sizeof(ws90), false, true, f, it) != RF_FSK_FINEOFFSET);

    // ---- receive phases and names ----
    assert(rfPhaseSet(RF_PROTO_FINEOFFSET) == RF_PROTO_ITPLUS17);
    assert(rfPhaseSet(RF_PROTO_ITPLUS17 | RF_PROTO_FINEOFFSET) == RF_PROTO_ITPLUS17);      // no alternating
    assert(rfPhaseSet(RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9 | RF_PROTO_FINEOFFSET) == (RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9));
    assert(rfPhaseSet(RF_PROTO_BRESSER | RF_PROTO_FINEOFFSET) == (RF_PROTO_BRESSER | RF_PROTO_ITPLUS17));
    assert(rfPhaseSet(RF_PROTO_BRESSER) == RF_PROTO_BRESSER && rfPhaseSet(0) == RF_PROTO_BRESSER);
    assert(!strcmp(rfProtoName(RF_PROTO_FINEOFFSET), "Fine Offset") && RF_PROTO_ALL == 15);

    // ---- table: WH65 rain counter (0.254 mm per count) as running total ----
    rfSensorsClear();
    int idx = rfSlotFor(0x24000050UL, 1000);
    assert(idx == 0);
    RfSensorReading& r = gRfSensors[idx];
    r.used = true; r.id = 0x24000050UL;
    unsigned long gap;
    assert(foDecode(wh65, sizeof(wh65), f));
    v = rfFineOffsetValues(f);
    assert(rfStoreValues(r, v, 1000, gap) == 0.0f && r.hasRain && r.hasTemp && r.hasWind);   // first packet: no delta
    uint8_t wh65b[17];
    memcpy(wh65b, wh65, sizeof(wh65b)); wh65b[9] = 0x03;                                     // +2 counts
    seal(wh65b, sizeof(wh65b));
    assert(foDecode(wh65b, sizeof(wh65b), f));
    v = rfFineOffsetValues(f);
    assert(near(rfStoreValues(r, v, 17000, gap), 0.508f) && gap == 0);

    // ---- Real frames from a forum radio trace (rc.8 on a Heltec V4, 21 Nov 2026) ----
    // Each frame as received, with the values the node showed - a regression
    // check of every decoder against real sensors, not only published examples.
    {
        struct Real { const char* hex; uint32_t id; const char* model; };
        auto ok = [&](const char* h, uint32_t id, const char* model) {
            const bool d = dec(h, f);
            if (!(d && rfFineOffsetId(f) == id && !strcmp(f.model, model) && f.batteryKnown))
                printf("REAL FRAME %s: decoded %d id %08lX model %s\n", h, d, (unsigned long)rfFineOffsetId(f), f.model);
            assert(d && rfFineOffsetId(f) == id && !strcmp(f.model, model) && f.batteryKnown);
        };
        ok("900181A20000A089C25908F00C003FFFB4E50F01965D0A4F943737F3FDA315D9", 0x900181A2, "WS90");
        assert(near(f.tempC, 5.0f, 0.05f) && f.humidity == 89 && near(f.windAvgMs, 0.8f, 0.05f) && near(f.windGustMs, 1.2f, 0.05f));
        assert(f.windDirDeg == 240 && near(f.rainMm, 40.6f, 0.05f) && f.uvIndex == 0.0f && f.lightLux == 0.0f);
        assert(near(f.supercapV, 2.9f, 0.01f) && f.batteryMv == 3200 && f.batteryOk);
        ok("4500394902662B4014801B0201B0FC", 0x45003949, "WH45");
        assert(near(f.tempC, 21.4f, 0.05f) && f.humidity == 43 && near(f.pm25, 2.0f, 0.51f) && near(f.pm10, 3.0f, 0.51f) && f.co2Ppm == 513);
        ok("35002C1D0003D05D55ADB0", 0x35002C1D, "WN35");
        assert(f.leafWetnessPct == 93 && f.batteryMv == 1700);
        ok("34007A6F40B54A57B3", 0x34007A6F, "WN34");          // sub-type 4: no +40 offset
        assert(near(f.tempC, 18.1f, 0.05f) && f.batteryMv == 1480);
        ok("3400A3C901BC4829CE", 0x3400A3C9, "WN34");          // sub-type 0: +40 offset
        assert(near(f.tempC, 4.4f, 0.05f) && f.batteryMv == 1440);
        ok("380029174020556592", 0x38002917, "WN38");
        assert(near(f.tempC, 3.2f, 0.05f) && f.batteryMv == 1700);
        ok("30DBB2812970D7", 0x300004DB, "WH31");
        assert(f.channel == 4 && near(f.tempC, 24.1f, 0.05f) && f.humidity == 41 && f.batteryOk);
        ok("40018EB8911C9AA674", 0x40018EB8, "WH40");
        assert(near(f.rainMm, 732.2f, 0.05f) && f.batteryMv == 1700);
        ok("40018EB8901C9AE0AD", 0x40018EB8, "WH40");
        assert(near(f.rainMm, 732.2f, 0.05f) && f.batteryMv == 1600);
        ok("20002AA492159D047FB5", 0x20002AA4, "WN20");
        assert(near(f.rainMm, 553.3f, 0.05f) && f.batteryMv == 2920);
        ok("5500ED880302BB3EE4AC", 0x5500ED88, "WH55");
        assert(f.channel == 1 && f.hasLeak && !f.leak);
        ok("510F651B0F7F11F87FFFFFFF9C8F", 0x510F651B, "WH51");
        assert(f.moisture == 17 && f.batteryMv == 1500);
        ok("57413CFF033F5968D6", 0x57013CFF, "WH57");
        assert(f.hasLightning && f.lightningCount == 89 && f.stormDistanceKm == FO_LIGHTNING_NO_DISTANCE);
        ok("A20042FF01CA1C3C300CAE1604FC5352854A7AD55304B2D2", 0xA20042FF, "WH52");
        assert(near(f.tempC, 5.8f, 0.05f) && f.moisture == 28 && near(f.conductivityUsCm, 127.0f, 0.5f));
        ok("A200825202661761302E2126764A938674D67BD84A0930F4", 0xA2008252, "WH52");
        assert(near(f.tempC, 21.4f, 0.05f) && f.moisture == 23 && near(f.conductivityUsCm, 461.0f, 0.5f));
        ok("ECE267242761E105", 0x250000CE, "WN32P");           // WN32P: WH25 frame (type 0xE)
        assert(f.family == FO_FAMILY_WH25 && near(f.tempC, 21.5f, 0.05f) && f.humidity == 36 && near(f.pressureHpa, 1008.1f, 0.05f));
        ok("D6C1BF5AFFFFAE6C", 0x3200006C, "WN32");           // WN32: WH32 frame (type 0xD)
        assert(f.family == FO_FAMILY_WH32 && near(f.tempC, 4.7f, 0.05f) && f.humidity == 90 && !f.hasPressure);

        // LDS01 (family 0x54): 19-byte frames every 80 s, CRC-8 at 17 and sum
        // at 18. Distance = low 12 bits of b[6..7] in mm (bit 0x40 of b[6] =
        // fast sending), b[15] = heater-on count (low byte).
        assert(foFrameLength(FO_FAMILY_LDS01) == 19);
        const char* const lds[] = { "548028018C64052940000007093FEC1A66FF15", "548028018C64052A400000070946B61A66866E",
                                    "548028018C64052A4000000A094CCA1A66292E", "548028018C6405294000000B0948FD1A6690C4",
                                    "548028018C64052A4000000E0947BD1A6608FF" };
        for (const char* h : lds) {
            assert(dec(h, f) && !strcmp(f.model, "LDS01") && f.family == FO_FAMILY_LDS01 && f.id == 0x802801);
            assert(rfFineOffsetId(f) == 0x54802801 && !f.batteryKnown && !f.hasTemp && !f.hasRain);
            assert(f.hasDistance && f.distanceMm >= 1321 && f.distanceMm <= 1322 && !f.fastTx &&
                   f.hasHeatCount && f.heatCount == 0x1A);
        }
        // Real trace with the console's own readings (Dirk27, 2026-11-25):
        // distance (mm) and the heat-on count (console "HoC", low byte).
        struct { const char* hx; uint16_t mm; bool fast; uint8_t hoc; } real[] = {
            { "548028018C6401F04000001015CA502566220A",  496, false, 0x25 },   // HoC 293
            { "548028018C6443E94000000E0B261927661452", 1001, true,  0x27 },   // HoC 295, 10-s frame
            { "548028018B6401F44000001214C02C2966793B",  500, false, 0x29 },   // HoC 297
        };
        for (auto& t : real) {
            assert(dec(t.hx, f) && f.hasDistance && f.distanceMm == t.mm);
            assert(f.fastTx == t.fast && f.hasHeatCount && f.heatCount == t.hoc);
        }
        uint8_t bad[19];
        hex(lds[0], bad); bad[10] ^= 0x01;   // one bit off: CRC and sum fail
        assert(!foDecode(buf, rx(buf, bad, sizeof(bad)), f));
        RfSensorReading nr; nr.proto = RF_PROTO_FINEOFFSET; nr.id = 0x54802801;
        assert(rfNominalIntervalMs(nr) == 80000);
    }
    // WN30: the WH31 frame with humidity 0 = temperature only, named WN30
    {
        uint8_t fr[7] = { 0x30, 0xA3, 0x02, 0x72, 0x00, 0, 0 }, buf[FO_MAX_FRAME_LEN];
        seal(fr, 7);
        FoFrame w;
        assert(foDecode(buf, rx(buf, fr, 7), w));
        assert(!strcmp(w.model, "WN30") && w.hasTemp && !w.hasHumidity && near(w.tempC, 22.6f));
        fr[4] = 41; seal(fr, 7);
        assert(foDecode(buf, rx(buf, fr, 7), w) && !strcmp(w.model, "WH31") && w.hasHumidity);
        fr[4] = 0xFF; seal(fr, 7);   // 0xFF = no reading: a WN30 that sends 0xFF, not 0x00, is still temperature-only
        assert(foDecode(buf, rx(buf, fr, 7), w) && !strcmp(w.model, "WN30") && w.hasTemp && !w.hasHumidity);
    }
    // Radio-clock time (family 0x52, rtl_433 "RCC"): BCD date / time, no sensor values
    {
        uint8_t fr[11] = { 0x52, 0xA3, 0x4A, 0x26, 0x11, 0x22, 0x18, 0x05, 0x09, 0, 0 }, buf[FO_MAX_FRAME_LEN];
        seal(fr, 11);
        FoFrame c;
        assert(foFrameLength(FO_FAMILY_RCC) == 11);
        assert(foDecode(buf, rx(buf, fr, 11), c));
        assert(c.hasClock && c.id == 0xA3 && c.clockYear == 2026 && c.clockMonth == 11 && c.clockDay == 22 &&
               c.clockHour == 18 && c.clockMinute == 5 && c.clockSecond == 9 && !c.hasTemp);
        fr[4] = 0x13; seal(fr, 11);   // month 13: rejected
        assert(!foDecode(buf, rx(buf, fr, 11), c));
        fr[4] = 0x11; fr[6] = 0x24; seal(fr, 11);   // hour 24: rejected
        assert(!foDecode(buf, rx(buf, fr, 11), c));
    }
    // A real DCF77 WH31 after a battery change (forum radio trace, 2026-11-23)
    {
        FoFrame c;
        assert(dec("52801226112313432103B8", c));
        assert(c.hasClock && c.id == 0x80 && c.clockYear == 2026 && c.clockMonth == 11 && c.clockDay == 23 &&
               c.clockHour == 13 && c.clockMinute == 43 && c.clockSecond == 21 && !c.batteryKnown);
    }
    return 0;
}
