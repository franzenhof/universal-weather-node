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
// sensor's own ID (8-24 bit); WH31: channel in bits 8-15, the 8-bit ID below.
inline uint32_t rfFineOffsetId(const FoFrame& f)
{
    uint32_t id = f.id & 0xFFFFFFUL;
    if (f.channel) id = (uint32_t)f.channel << 8 | (f.id & 0xFF);
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
