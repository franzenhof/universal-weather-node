// LaCrosse / TFA IT+ frame decoder (src/itplus.cpp).
#include "ItPlusDecoder.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main()
{
    // Published TX29-IT example (rtl_433 tests): aa 2d d4 | 92 84 48 6a ec
    const uint8_t tx29[] = {0x92, 0x84, 0x48, 0x6A, 0xEC};
    assert(itPlusCrc8(tx29, 4) == 0xEC);
    ItPlusFrame f;
    assert(itPlusDecode(tx29, f));
    assert(f.sensorId == 10 && !f.newBattery && !f.weakBattery);
    assert(near(f.tempC, 4.8f) && !f.hasHumidity);

    // Temperature + humidity, weak battery, new battery, negative temperature:
    // id 0x2B, new battery, -5.3 C (34.7 -> BCD 3 4 7), 0x80 | 87 %
    uint8_t th[5] = {0x9A, (uint8_t)(0xC0 | 0x20 | 0x03), 0x47, (uint8_t)(0x80 | 87), 0};
    th[4] = itPlusCrc8(th, 4);
    assert(itPlusDecode(th, f));
    assert(f.sensorId == 0x2B && f.newBattery && f.weakBattery);
    assert(near(f.tempC, -5.3f) && f.hasHumidity && f.humidity == 87);

    // Second probe channel: ID + 64, no humidity
    uint8_t pr[5] = {0x92, 0x84, 0x48, ITPLUS_PROBE_FLAG, 0};
    pr[4] = itPlusCrc8(pr, 4);
    assert(itPlusDecode(pr, f) && f.sensorId == 10 + 64 && !f.hasHumidity);

    // Rejections: CRC, model nibble, BCD, humidity range
    uint8_t bad[5] = {0x92, 0x84, 0x48, 0x6A, 0xED};
    assert(!itPlusDecode(bad, f));
    uint8_t model[5] = {0x82, 0x84, 0x48, 0x6A, 0}; model[4] = itPlusCrc8(model, 4);
    assert(!itPlusDecode(model, f));
    uint8_t bcd[5] = {0x92, 0x8A, 0x48, 0x6A, 0}; bcd[4] = itPlusCrc8(bcd, 4);
    assert(!itPlusDecode(bcd, f));
    uint8_t hum[5] = {0x92, 0x84, 0x48, 101, 0}; hum[4] = itPlusCrc8(hum, 4);
    assert(!itPlusDecode(hum, f));
    return 0;
}
