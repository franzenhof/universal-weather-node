// Wireless sensor table (src/rf_sensors_data.cpp) and the rain/freshness
// handling of src/sources.cpp: per-message values, slot reuse, expiry,
// rain after an outage.
#include "rf_sensors.h"
#include "sources.h"
#include "globals.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main()
{
    gCfg.rfEn = 1;
    unsigned long gap;

    // ---- per-message values: a 6-in-1 alternates temp/hum/wind/uv and
    // wind/rain messages; only what a message carries is refreshed ----
    RfSensorReading& s = gRfSensors[rfSlotFor(0x1234, millis())];
    s.used = true; s.id = 0x1234;
    RfValues a; a.ok = RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_UV; a.tempC = 5.5f; a.humidity = 80;
    RfValues b; b.ok = RF_VAL_WIND | RF_VAL_RAIN; b.rainTotalMm = 100.0f;
    assert(rfStoreValues(s, a, millis(), gap) == 0.0f && gap == 0);
    assert(rfStoreValues(s, b, millis(), gap) == 0.0f && gap == 0);   // first rain value after start: no increase
    assert(s.hasTemp && s.hasRain && !s.hasLight && near(s.rainTotalMm, 100.0f));
    // the rain part fails for 6 min, the other message keeps coming
    for (int i = 0; i < 30; i++) { delay(12000); rfStoreValues(s, a, millis(), gap); rfExpireValues(millis()); }
    assert(srcRfValueFresh(0, "temp") && srcRfValueFresh(0, "wind") && !srcRfValueFresh(0, "rain"));
    assert((s.staleMask & ~RF_VAL_LIGHT) == RF_VAL_RAIN);   // (light: never received)
    // rain back after the outage: increase + gap reported, staleness cleared
    b.rainTotalMm = 106.0f;
    assert(near(rfStoreValues(s, b, millis(), gap), 6.0f) && gap == 360000UL);
    assert(srcRfValueFresh(0, "rain") && (s.staleMask & ~RF_VAL_LIGHT) == 0);
    b.rainTotalMm = 106.5f;
    assert(near(rfStoreValues(s, b, millis(), gap), 0.5f) && gap == 0);
    b.rainTotalMm = 0.3f;   // battery change: the sensor's total restarts
    assert(near(rfStoreValues(s, b, millis(), gap), 0.3f));

    // ---- expiry survives the millis() wrap: once stale, a value stays
    // stale even if its timestamp looks recent again ----
    delay(SRC_RF_FRESH_MS + 1000);
    rfExpireValues(millis());
    assert(s.staleMask == (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_UV | RF_VAL_RAIN | RF_VAL_LIGHT | RF_VAL_ANY));
    s.tempMs = millis() - 1000;   // what a 32-bit millis() 49.7 days later would look like
    assert(!srcRfValueFresh(0, "temp"));
    rfStoreValues(s, a, millis(), gap);
    assert(srcRfValueFresh(0, "temp") && !srcRfValueFresh(0, "rain"));

    // ---- slots: full table, stale sensor taken over, filter change ----
    rfSensorsClear();
    for (uint32_t id = 1; id <= RF_MAX_SENSORS; id++) {
        int i = rfSlotFor(id, millis());
        assert(i == (int)id - 1);
        gRfSensors[i].used = true; gRfSensors[i].id = id; gRfSensors[i].lastMs = millis();
        gRfSensors[i].proto = id == 2 ? 2 : 1;
        delay(1000);
    }
    assert(rfSlotFor(3, millis()) == 2);            // known sensor
    assert(rfSlotFor(99, millis()) == -1);          // full, all heard recently
    delay(SRC_RF_FRESH_MS);
    gRfSensors[0].lastMs = millis();                 // sensor 1 still sends; 2, 3, 4 are gone
    assert(rfSlotFor(99, millis(), 60000, 4) == -1);  // only sensors of that protocol
    int i = rfSlotFor(99, millis(), 60000, 2);       // e.g. IT+ new battery: same rate only
    assert(i == 1 && !gRfSensors[1].used && gRfSensors[1].packets == 0);
    gRfSensors[1].used = true; gRfSensors[1].id = 99; gRfSensors[1].lastMs = millis();
    i = rfSlotFor(98, millis());                     // the longest silent stale one: sensor 3
    assert(i == 2 && !gRfSensors[2].used);
    rfSensorsClear();
    for (const RfSensorReading& r : gRfSensors) assert(!r.used);

    // ---- rain after an outage is spread, not counted as one burst ----
    gCfg.srcSel[SG_RAIN] = SRC_RF1;
    RfSensorReading& w = gRfSensors[0];
    w.used = true; w.id = 0x77; w.hasRain = true; w.rainMs = w.lastMs = millis();
    assert(srcResolve(SG_RAIN) == 0);
    float before = rainMMSinceStart;
    srcRfPacket(0, false, 6.0f, false, 2 * 3600000UL);   // 6 mm while the sensor was not heard for 2 h
    assert(near(rainMMSinceStart, before));
    for (int k = 0; k < 150; k++) {                       // 30 min of packets: half released (6 mm over 1 h max)
        delay(12000); w.rainMs = millis();
        srcRfPacket(0, false, 0.0f, false);
    }
    assert(near(rainMMSinceStart - before, 3.0f));
    delay(12000); w.rainMs = millis();
    srcRfPacket(0, false, 0.2f, false);                    // new rain counts at once, plus the next share
    assert(near(rainMMSinceStart - before, 3.0f + 0.2f + 0.02f));
    for (int k = 0; k < 400; k++) { delay(12000); w.rainMs = millis(); srcRfPacket(0, false, 0.0f, false); }
    assert(near(rainMMSinceStart - before, 6.2f));         // nothing lost, nothing extra
    return 0;
}
