#include "ItPlusDecoder.h"

uint8_t itPlusCrc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

bool itPlusDecode(const uint8_t* b, ItPlusFrame& out)
{
    if ((b[0] >> 4) != 0x9) return false;
    if (itPlusCrc8(b, 4) != b[4]) return false;
    int tens = b[1] & 0x0F, ones = b[2] >> 4, tenths = b[2] & 0x0F;
    if (tens > 9 || ones > 9 || tenths > 9) return false;   // BCD
    out = ItPlusFrame{};
    out.sensorId    = (uint8_t)(((b[0] & 0x0F) << 2) | (b[1] >> 6));
    out.newBattery  = (b[1] >> 5) & 1;
    out.weakBattery = (b[3] >> 7) & 1;
    // in tenths first: float "37.6 - 40" gave -2.4000015 (printed -2.400002)
    out.tempC       = (tens * 100 + ones * 10 + tenths - 400) / 10.0f;
    uint8_t h = b[3] & 0x7F;
    if (h == ITPLUS_PROBE_FLAG) {
        out.sensorId += 64;                 // second probe channel: own ID
    } else if (h != ITPLUS_NO_HUMIDITY) {
        if (h > 100) return false;
        out.hasHumidity = true;
        out.humidity    = h;
    }
    return true;
}
