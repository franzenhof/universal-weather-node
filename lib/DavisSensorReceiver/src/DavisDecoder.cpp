#include "DavisDecoder.h"

uint8_t davisReverseBits(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

uint16_t davisCrc16(const uint8_t* data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

float davisWindDirDeg(uint8_t raw, DavisModel model)
{
    if (model == DAVIS_VUE) {
        float d = raw * 1.40625f + 0.3f;          // 360 / 256 per step
        return d >= 360.0f ? d - 360.0f : d;
    }
    // Vantage Pro2: raw 1..254 spread over 9..351 degrees (the potentiometer's
    // dead band around north); 0 and 255 are reported with north wind.
    if (raw == 0)   return 0.0f;
    if (raw == 255) return 355.0f;
    return 9.0f + (raw - 1) * 342.0f / 253.0f;
}

uint8_t davisTrailerZeroBits(const uint8_t bytes[DAVIS_PACKET_LEN])
{
    uint8_t zeros = 0;
    for (int i = 8; i < 10; i++)
        for (uint8_t m = 1; m; m = (uint8_t)(m << 1))
            if (!(bytes[i] & m)) zeros++;
    return zeros;
}

uint32_t davisIntervalUs(uint8_t txId)
{
    if (txId < 1 || txId > 8) return 0;
    return (40u + txId) * 62500u;               // (40 + ID) / 16 s
}

float davisRainRateMmH(float tipSec, float mmPerTip)
{
    return tipSec > 0.0f ? 3600.0f / tipSec * mmPerTip : 0.0f;
}

const char* davisTypeName(uint8_t type)
{
    switch (type) {
        case DAVIS_MSG_SUPERCAP:  return "supercap";
        case DAVIS_MSG_UNKNOWN3:  return "type 3";
        case DAVIS_MSG_UV:        return "uv";
        case DAVIS_MSG_RAINRATE:  return "rainrate";
        case DAVIS_MSG_SOLAR:     return "solar";
        case DAVIS_MSG_SOLARCELL: return "solarcell";
        case DAVIS_MSG_TEMP:      return "temp";
        case DAVIS_MSG_GUST:      return "gust";
        case DAVIS_MSG_HUMIDITY:  return "humidity";
        case DAVIS_MSG_UNKNOWNC:  return "type C";
        case DAVIS_MSG_RAIN:      return "rain";
        case DAVIS_MSG_SOILLEAF:  return "soil/leaf";
        default:                  return "unknown";
    }
}

bool davisDecode(const uint8_t air[DAVIS_PACKET_LEN], DavisPacket& p, DavisModel model)
{
    uint8_t b[DAVIS_PACKET_LEN];
    for (size_t i = 0; i < DAVIS_PACKET_LEN; i++) b[i] = davisReverseBits(air[i]);
    return davisDecodeOrdered(b, p, model);
}

bool davisDecodeOrdered(const uint8_t bytes[DAVIS_PACKET_LEN], DavisPacket& p, DavisModel model)
{
    p = DavisPacket();
    for (size_t i = 0; i < DAVIS_PACKET_LEN; i++) p.bytes[i] = bytes[i];
    const uint8_t* b = p.bytes;
    const uint16_t rx = (uint16_t)(b[6] << 8 | b[7]);
    const uint16_t crc = davisCrc16(b, 6);
    if (crc == rx)                          p.crc = DAVIS_CRC_OK;
    else if (davisCrc16(b + 8, 2, crc) == rx) p.crc = DAVIS_CRC_REPEATER;
    else                                    return false;
    // An all-zero packet passes (CRC of zeros is 0): noise on a quiet
    // channel can look like that, so it is rejected.
    bool allZero = true;
    for (int i = 0; i < 8; i++) allZero &= b[i] == 0;
    if (allZero) { p.crc = DAVIS_CRC_FAIL; return false; }

    p.type       = b[0] >> 4;
    p.txId       = (uint8_t)((b[0] & 0x07) + 1);
    p.batteryLow = (b[0] & 0x08) != 0;
    p.windMph    = b[1];
    p.windDirRaw = b[2];
    p.hasWindDir = !(b[1] == 0 && b[2] == 0);
    p.windDirDeg = p.hasWindDir ? davisWindDirDeg(b[2], model) : 0.0f;
    p.raw10      = (uint16_t)((b[3] << 2) | (b[4] >> 6));

    switch (p.type) {
        case DAVIS_MSG_TEMP:
            // 12-bit two's complement in 0.1 F: byte 3 and the upper nibble of
            // byte 4; bit 3 of byte 4 set = digital sensor (SHT), otherwise an
            // old analog thermistor (different scale, not decoded).
            if (b[4] & 0x08) {
                int16_t v = (int16_t)(uint16_t)(b[3] << 8 | b[4]);
                p.tempF   = (float)(v >> 4) / 10.0f;        // arithmetic shift keeps the sign
                p.tempC   = (p.tempF - 32.0f) * 5.0f / 9.0f;
                p.hasTemp = true;
            } else if (b[3] != 0xFF) {
                p.tempAnalog = true;
            }
            break;
        case DAVIS_MSG_HUMIDITY: {
            uint16_t raw = (uint16_t)((b[4] >> 4) << 8 | b[3]);
            if (raw == 0) break;                            // no sensor
            float h = (b[4] & 0x08) ? raw / 10.0f              // digital: 0.1 %
                                    : raw * -0.301f + 710.23f; // analog (meteostick's fit)
            if (h < 0.0f) h = 0.0f;
            if (h > 100.0f) h = 100.0f;
            p.humidity = h;
            p.hasHumidity = true;
            break;
        }
        case DAVIS_MSG_RAIN:
            if (b[3] == 0x80) break;                        // no rain collector
            p.rainCount = b[3] & 0x7F;
            p.hasRainCount = true;
            break;
        case DAVIS_MSG_RAINRATE: {
            uint16_t raw = (uint16_t)((b[4] & 0x30) << 4 | b[3]);
            if (raw == 0x3FF)          p.rainTipSec = 0.0f;           // no rain
            else if (b[4] & 0x40)      p.rainTipSec = (float)raw;     // light rain: seconds
            else                       p.rainTipSec = raw / 16.0f;    // heavy rain: 1/16 s
            p.hasRainRate = raw == 0x3FF || raw > 0;
            break;
        }
        case DAVIS_MSG_UV:
            if (b[3] == 0xFF) break;                        // no sensor
            p.uvIndex = p.raw10 / 50.0f;
            p.hasUv = true;
            break;
        case DAVIS_MSG_SOLAR:
            if (b[3] == 0xFF) break;                        // no sensor
            p.solarWm2 = p.raw10 * 1.757936f;
            p.hasSolar = true;
            break;
        case DAVIS_MSG_GUST:
            p.gustMph   = b[3];
            p.gustIndex = b[5] >> 4;
            p.hasGust   = true;
            break;
        case DAVIS_MSG_SUPERCAP:
            if (p.raw10 == 0x3FF) break;
            p.supercapV = p.raw10 / 300.0f;
            p.hasSupercap = true;
            break;
        case DAVIS_MSG_SOLARCELL:
            if (p.raw10 == 0x3FF) break;
            p.solarCellV = p.raw10 / 300.0f;
            p.hasSolarCell = true;
            break;
        default:
            break;
    }
    return true;
}

uint8_t DavisRainCounter::update(uint8_t count)
{
    count &= 0x7F;
    if (!valid_) { valid_ = true; last_ = count; return 0; }
    uint8_t step = (uint8_t)((count - last_) & 0x7F);
    last_ = count;
    if (step > maxStep_) return 0;                  // counter reset, not rain
    total_ += step;
    return step;
}
