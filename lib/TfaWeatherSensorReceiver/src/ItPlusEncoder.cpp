#include "ItPlusEncoder.h"
#include <math.h>

size_t itPlusEncode(uint8_t* out, uint8_t sensorId, bool newBattery, float tempC,
                    bool hasHumidity, uint8_t humidity)
{
    if (!out || sensorId > 63 || !isfinite(tempC)) return 0;
    long t = lroundf(tempC * 10.0f) + 400;   // tenths of (degC + 40)
    t = t < 0 ? 0 : t > 999 ? 999 : t;
    const uint8_t tens = (uint8_t)(t / 100), ones = (uint8_t)(t / 10 % 10), tenths = (uint8_t)(t % 10);
    uint8_t h = ITPLUS_NO_HUMIDITY;
    if (hasHumidity) h = humidity < 1 ? 1 : humidity > 99 ? 99 : humidity;
    out[0] = (uint8_t)(0x90 | (sensorId >> 2));
    out[1] = (uint8_t)((sensorId & 0x03) << 6 | (newBattery ? 0x20 : 0) | tens);
    out[2] = (uint8_t)(ones << 4 | tenths);
    out[3] = h;                               // bit 7 = weak battery: never set
    out[4] = itPlusCrc8(out, 4);
    return ITPLUS_FRAME_LEN;
}
