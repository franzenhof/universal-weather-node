// LaCrosse / TFA IT+ frame decoder and encoder (lib/TfaWeatherSensorReceiver).
#include "ItPlusDecoder.h"
#include "ItPlusEncoder.h"
#include <cstring>
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

    // -2.4 C (37.6 -> BCD 3 7 6): exactly the float nearest -2.4, no residue
    uint8_t m24[5] = {0x92, 0x83, 0x76, 0x6A, 0};
    m24[4] = itPlusCrc8(m24, 4);
    assert(itPlusDecode(m24, f) && f.tempC == -2.4f);
    for (int t = 0; t <= 999; t++) {   // every BCD value: the nearest float of (t - 400) / 10
        uint8_t b[5] = {0x92, (uint8_t)(0x80 | (t / 100)), (uint8_t)((((t / 10) % 10) << 4) | (t % 10)), 0x6A, 0};
        b[4] = itPlusCrc8(b, 4);
        assert(itPlusDecode(b, f) && f.tempC == (float)((t - 400) / 10.0));
    }

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

    // ---- encoder (virtual sensors): round trip through itPlusDecode ----
    uint8_t e[ITPLUS_FRAME_LEN];
    assert(itPlusEncode(e, 10, false, 4.8f, false, 0) == ITPLUS_FRAME_LEN);
    assert(!memcmp(e, tx29, sizeof(tx29)));                    // the published example, byte for byte
    for (int t = 0; t <= 999; t++) {                           // every BCD temperature, both flags, all IDs
        const float c = (float)((t - 400) / 10.0);
        const uint8_t id = (uint8_t)(t % 64);
        const bool nb = t & 1;
        assert(itPlusEncode(e, id, nb, c, false, 0) == ITPLUS_FRAME_LEN);
        assert(itPlusDecode(e, f) && f.tempC == c && f.sensorId == id && f.newBattery == nb && !f.weakBattery && !f.hasHumidity);
        assert(e[3] == ITPLUS_NO_HUMIDITY);
    }
    for (int h = 0; h <= 255; h++) {                           // humidity 1..99, never the 0x6A / 0x7D codes
        assert(itPlusEncode(e, 63, true, 21.4f, true, (uint8_t)h) == ITPLUS_FRAME_LEN);
        assert(itPlusDecode(e, f) && f.hasHumidity && f.sensorId == 63 && near(f.tempC, 21.4f));
        assert(f.humidity == (h < 1 ? 1 : h > 99 ? 99 : h));
    }
    // range of the originals, rounding to 0.1
    assert(itPlusEncode(e, 1, false, -55.0f, false, 0) && itPlusDecode(e, f) && f.tempC == -40.0f);
    assert(itPlusEncode(e, 1, false, 80.0f, false, 0) && itPlusDecode(e, f) && f.tempC == 59.9f);
    assert(itPlusEncode(e, 1, false, ITPLUS_TEMP_MIN, false, 0) && itPlusDecode(e, f) && f.tempC == -40.0f);
    assert(itPlusEncode(e, 1, false, ITPLUS_TEMP_MAX, false, 0) && itPlusDecode(e, f) && f.tempC == 59.9f);
    assert(itPlusEncode(e, 1, false, 21.36f, false, 0) && itPlusDecode(e, f) && f.tempC == 21.4f);
    assert(itPlusEncode(e, 1, false, -0.04f, false, 0) && itPlusDecode(e, f) && f.tempC == 0.0f);
    // invalid input: no frame
    assert(itPlusEncode(e, 64, false, 20.0f, false, 0) == 0);
    assert(itPlusEncode(e, 1, false, NAN, false, 0) == 0);
    assert(itPlusEncode(e, 1, false, INFINITY, false, 0) == 0);
    assert(itPlusEncode(nullptr, 1, false, 20.0f, false, 0) == 0);
    return 0;
}
