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

static bool near(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// CRC-8 at n-2 and sum at n-1 for a frame of n bytes
static void seal(uint8_t* b, size_t n)
{
    b[n - 2] = foCrc8(b, n - 2);
    b[n - 1] = foSum8(b, n - 1);
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
    assert(foDecode(wh24, rx(buf, wh24, sizeof(wh24)), f, true));
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
    assert(foDecode(wh65, rx(buf, wh65, sizeof(wh65)), f));
    assert(f.id == 0x50 && near(f.tempC, 24.7f) && f.humidity == 65 && f.windDirDeg == 359);
    assert(near(f.windAvgMs, 0.0f) && near(f.rainMm, 0.254f) && f.uvIndex == 0.0f);

    // WS69: 8 more bytes with pressure (here 0x1FFFF = no pressure module)
    uint8_t ws69[] = {0x24, 0x1c, 0x88, 0x01, 0xca, 0x63, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0xd5,
                      0x01, 0xff, 0xff, 0xff, 0x00, 0x2f, 0x83, 0x5a};
    assert(foDecode(ws69, rx(buf, ws69, sizeof(ws69)), f));
    assert(!strcmp(f.model, "WS69") && f.id == 0x1c && near(f.tempC, 5.8f) && f.humidity == 99);
    assert(f.windDirDeg == 136 && near(f.rainMm, 51 * 0.254f, 0.01f) && !f.hasPressure);
    ws69[17] = 0x01; ws69[18] = 0x8B; ws69[19] = 0xCD;   // 101325 Pa
    seal(ws69, sizeof(ws69));
    assert(foDecode(ws69, sizeof(ws69), f) && f.hasPressure && near(f.pressureHpa, 1013.25f, 0.01f));
    ws69[24] ^= 1;   // bad extension: still a valid WH65 frame, no pressure
    assert(foDecode(ws69, sizeof(ws69), f) && !strcmp(f.model, "WH65") && !f.hasPressure);

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
    assert(foDecode(wh31, rx(buf, wh31, sizeof(wh31)), f));
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
    assert(foDecode(wh40, rx(buf, wh40, sizeof(wh40)), f));
    assert(f.id == 0x0cd6f && f.batteryMv == 1600 && f.batteryOk && f.hasRain && near(f.rainMm, 1.0f) && !f.hasTemp);
    assert(rfFineOffsetId(f) == 0x4000CD6FUL && rfFineOffsetValues(f).ok == RF_VAL_RAIN);

    // ---- WN20 rain gauge ----
    const uint8_t wn20[] = {0x20, 0x00, 0x2c, 0x87, 0x98, 0x00, 0x13, 0x04, 0x9e, 0x20};
    assert(foDecode(wn20, rx(buf, wn20, sizeof(wn20)), f));
    assert(f.id == 0x2c87 && f.batteryMv == 3040 && f.batteryOk && near(f.rainMm, 1.9f));

    // ---- WH57 lightning ----
    const uint8_t wh57s[] = {0x57, 0x81, 0x05, 0xc8, 0x05, 0x81, 0x01, 0xdf, 0x0b};
    assert(foDecode(wh57s, rx(buf, wh57s, sizeof(wh57s)), f));
    assert(f.id == 0x105c8 && f.hasLightning && f.lightningState == 8 && f.lightningCount == 1);
    assert(f.stormDistanceKm == 1 && f.batteryOk && rfFineOffsetValues(f).ok == 0);
    const uint8_t wh57r[] = {0x57, 0x01, 0x05, 0xc8, 0x05, 0xbf, 0x00, 0xdd, 0xc6};
    assert(foDecode(wh57r, sizeof(wh57r), f) && f.lightningState == 0 && f.stormDistanceKm == FO_LIGHTNING_NO_DISTANCE);

    // ---- WH51 soil moisture ----
    const uint8_t wh51[] = {0x51, 0x00, 0x6b, 0x58, 0x6e, 0x7f, 0x24, 0xf8, 0xd2, 0xff, 0xff, 0xff, 0x3c, 0x28};
    assert(foDecode(wh51, rx(buf, wh51, sizeof(wh51)), f));
    assert(f.id == 0x006b58 && f.hasMoisture && f.moisture == 36 && f.batteryMv == 1400 && f.batteryOk);
    assert(rfFineOffsetValues(f).ok == 0 && rfFineOffsetId(f) == 0x51006B58UL);

    // ---- WN34 / WN38 temperature probes ----
    const uint8_t wn34[] = {0x34, 0x00, 0x29, 0x65, 0x02, 0x85, 0x44, 0x66, 0xf3};
    assert(foDecode(wn34, rx(buf, wn34, sizeof(wn34)), f));
    assert(f.id == 0x002965 && near(f.tempC, 24.5f) && f.batteryMv == 1360 && f.batteryOk && !f.hasHumidity);
    const uint8_t wn38[] = {0x38, 0x00, 0x28, 0x85, 0x40, 0xf5, 0x41, 0x4c, 0xa7};
    assert(foDecode(wn38, sizeof(wn38), f) && !strcmp(f.model, "WN38") && near(f.tempC, 24.5f));
    uint8_t wn34d[] = {0x34, 0x01, 0x02, 0x03, 0x4D, 0xDA, 0x3c, 0, 0};   // WN34D (sub-type 4): -55.0 C, 1200 mV
    seal(wn34d, sizeof(wn34d));
    assert(foDecode(wn34d, sizeof(wn34d), f) && near(f.tempC, -55.0f) && !f.batteryOk);

    // ---- WS80 ----
    const uint8_t ws80[] = {0x80, 0x0a, 0x00, 0x3b, 0x00, 0x00, 0x88, 0x8a, 0x59, 0x38, 0x18, 0x6d, 0x1c, 0x00, 0xff, 0xff, 0xd8, 0xdf};
    assert(foDecode(ws80, rx(buf, ws80, sizeof(ws80)), f));
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

    // ---- WS85 ----
    const uint8_t ws85[] = {0x85, 0x00, 0x28, 0xEB, 0x87, 0x82, 0x6F, 0x00, 0x83, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x0B, 0x00, 0x00, 0xFF, 0xEF, 0xFD, 0x00, 0x00, 0x6B, 0xDD, 0x0F};
    assert(foFrameLength(0x85) == sizeof(ws85));
    assert(foDecode(ws85, rx(buf, ws85, sizeof(ws85)), f));
    assert(f.id == 0x0028eb && f.batteryMv == 2700 && f.batteryOk && f.windDirDeg == 131 && f.hasRain && !f.hasTemp);

    // ---- WS68 ----
    const uint8_t ws68[] = {0x68, 0x00, 0x00, 0xc5, 0x01, 0x07, 0x4b, 0x0f, 0xff, 0xff, 0x00, 0x2e, 0x00, 0x02, 0xa6, 0x63};
    assert(foDecode(ws68, rx(buf, ws68, sizeof(ws68)), f));
    assert(f.id == 0xc5 && near(f.lightLux, 2630.0f) && f.windDirDeg == 46 && near(f.uvIndex, 0.2f) && f.batteryOk);
    const uint8_t ws68w[] = {0x68, 0x00, 0x00, 0xc5, 0x00, 0x00, 0x4b, 0x0f, 0xff, 0xff, 0x7e, 0xe0, 0x94, 0x00, 0x75, 0xec};
    assert(foDecode(ws68w, sizeof(ws68w), f) && near(f.windAvgMs, 12.6f) && f.windDirDeg == 224 && near(f.windGustMs, 14.8f));

    // ---- rejections ----
    uint8_t bad[sizeof(ws80)];
    memcpy(bad, ws80, sizeof(bad)); bad[16] ^= 0x01;    // CRC
    assert(!foDecode(bad, sizeof(bad), f));
    memcpy(bad, ws80, sizeof(bad)); bad[17] ^= 0x01;    // sum
    assert(!foDecode(bad, sizeof(bad), f));
    memcpy(bad, ws80, sizeof(bad)); bad[9] ^= 0x01;     // payload
    assert(!foDecode(bad, sizeof(bad), f));
    uint8_t unk[9] = {0x41, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0, 0};   // WH0290 / WH41: not supported
    seal(unk, sizeof(unk));
    assert(!foDecode(unk, sizeof(unk), f) && foFrameLength(0x41) == 0);
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
    return 0;
}
