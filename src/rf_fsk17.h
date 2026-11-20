#pragma once
// The shared 17.241 kbit/s receive phase (rf_sensors.cpp): LaCrosse / TFA IT+
// and Fine Offset use the same bit rate, preamble and sync word 2D D4, so one
// receive configuration catches both. A frame (the bytes after the sync word,
// read in fixed-length mode with the longest Fine Offset frame) is tried as
// Fine Offset first - family code, CRC-8 and sum over up to 32 bytes; also
// needed because the WS90 family code 0x90 starts with the IT+ model nibble
// 9 - and then as IT+ (model nibble 9, CRC-8 over the first 4 bytes).
// Header-only and RadioLib-free: host-tested (test/firmware_fineoffset.cpp).
#include <FineOffsetDecoder.h>
#include <ItPlusDecoder.h>
#include "rf_sensors.h"

enum RfFskKind : uint8_t { RF_FSK_NONE = 0, RF_FSK_FINEOFFSET, RF_FSK_ITPLUS };

inline RfFskKind rfFsk17Decode(const uint8_t* b, size_t len, bool fineOffset, bool itPlus,
                               FoFrame& fo, ItPlusFrame& it)
{
    if (fineOffset && foDecode(b, len, fo)) return RF_FSK_FINEOFFSET;
    if (itPlus && len >= ITPLUS_FRAME_LEN && itPlusDecode(b, it)) return RF_FSK_ITPLUS;
    return RF_FSK_NONE;
}

// Table ID of a Fine Offset sensor: family code in the top byte, then the
// sensor's own ID (8-24 bit); WH31: channel in bits 8-15, the 8-bit ID below
// (the WH55's ID already contains its channel bits, as the Ecowitt app shows it).
// WH25 / WH32 family (no family byte on the air): 25 / 32.
inline uint32_t rfFineOffsetId(const FoFrame& f)
{
    uint32_t id = f.id & 0xFFFFFFUL;
    if (f.channel && (f.family == FO_FAMILY_WH31 || f.family == FO_FAMILY_WH31B))
        id = (uint32_t)f.channel << 8 | (f.id & 0xFF);
    return (uint32_t)f.family << 24 | id;
}

// The value groups of one Fine Offset frame (only those it carries and that
// are valid), for rfStoreValues().
inline RfValues rfFineOffsetValues(const FoFrame& f)
{
    RfValues v;
    if (f.hasTemp)     { v.ok |= RF_VAL_TEMP;  v.tempC = f.tempC; }
    if (f.hasHumidity) { v.ok |= RF_VAL_HUM;   v.humidity = f.humidity; }
    if (f.hasWind) {
        v.ok |= RF_VAL_WIND;
        v.windAvgMs = f.windAvgMs;  v.windGustMs = f.windGustMs;  v.windDirDeg = f.windDirDeg;
    }
    if (f.hasRain)     { v.ok |= RF_VAL_RAIN;  v.rainTotalMm = f.rainMm; }
    if (f.hasUv)       { v.ok |= RF_VAL_UV;    v.uvIndex = f.uvIndex; }
    if (f.hasLight)    { v.ok |= RF_VAL_LIGHT; v.lightLux = f.lightLux; }
    return v;
}

// The per-sensor values of a Fine Offset frame outside the value-source groups
// (air quality, leak, leaf wetness, EC, pressure) into the table entry.
inline void rfFineOffsetExtras(RfSensorReading& r, const FoFrame& f)
{
    if (f.hasPressure)  { r.hasPressure = true;  r.pressureHpa = f.pressureHpa; }
    if (f.hasPm25)      { r.hasPm25 = true;      r.pm25 = f.pm25; }
    if (f.hasPm10)      { r.hasPm10 = true;      r.pm10 = f.pm10; r.pm10Estimated = f.pm10Estimated; }
    if (f.hasPm1)       { r.hasPm1 = true;       r.pm1 = f.pm1; }
    if (f.hasPm4)       { r.hasPm4 = true;       r.pm4 = f.pm4; }
    if (f.hasCo2)       { r.hasCo2 = true;       r.co2Ppm = f.co2Ppm; }
    if (f.hasPm25)      r.extPower = f.extPower;
    if (f.hasLeak) {
        r.hasLeak = true;  r.leak = f.leak;  r.leakHighSensitivity = f.leakHighSensitivity;  r.leakRaw = f.leakRaw;
    }
    if (f.hasLeafWetness)  { r.hasLeafWetness = true;  r.leafWetnessPct = f.leafWetnessPct; }
    if (f.hasConductivity) { r.hasConductivity = true; r.conductivityUsCm = f.conductivityUsCm; }
}
