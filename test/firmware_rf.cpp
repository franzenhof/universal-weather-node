// Wireless sensor table (src/rf_sensors_data.cpp) and the rain/freshness
// handling of src/sources.cpp: per-message values, slot reuse, expiry,
// rain after an outage.
#include "rf_sensors.h"
#include "sources.h"
#include "globals.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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
    // ---- 32 sensors: slot mapping with and without an ID list; only
    // slots 1-4 are value sources (SRC_RF1..SRC_RF4 keep their meaning) ----
    static_assert(RF_MAX_SENSORS == 32 && RF_SOURCE_SLOTS == 4, "32 received, 4 source slots");
    static_assert(SRC_AUTO == 0 && SRC_BOARD == 1 && SRC_RF1 == 2 && SRC_RF4 == 5 && SRC_OFF == 6,
                  "stored settings / parameters 45-49 must keep their values");
    rfSensorsClear();
    gCfg.rfIds[0] = '\0';
    for (int k = 0; k < SG_COUNT; k++) gCfg.srcSel[k] = SRC_AUTO;
    for (uint32_t id = 1; id <= RF_MAX_SENSORS; id++) {
        int i = rfSlotFor(0x30000000UL + id, millis());
        assert(i == (int)id - 1);
        RfSensorReading& r = gRfSensors[i];
        r.used = true; r.id = 0x30000000UL + id; r.lastMs = millis();
    }
    assert(rfSlotFor(0x30000099UL, millis()) == -1);            // 33rd sensor: table full
    for (int k = 0; k < RF_MAX_SENSORS; k++) {                   // no list: slot = reception order
        assert(rfSensorIndexForSlot(k) == k && rfSlotOfIndex(k) == k);
    }
    assert(rfSensorIndexForSlot(RF_MAX_SENSORS) == -1 && rfSensorIndexForSlot(-1) == -1);
    // light only from sensor 6: Auto never looks beyond wireless 4
    RfSensorReading& six = gRfSensors[5];
    six.hasLight = true; six.lightLux = 1000.0f; six.lightMs = millis();
    assert(srcResolve(SG_LIGHT) == SRC_NONE);
    gRfSensors[3].hasLight = true; gRfSensors[3].lightMs = millis();
    assert(srcResolve(SG_LIGHT) == 3);                           // wireless 4 = table index 3
    gCfg.srcSel[SG_LIGHT] = SRC_RF4;
    assert(srcResolve(SG_LIGHT) == 3);
    gCfg.srcSel[SG_LIGHT] = SRC_RF1;
    assert(srcResolve(SG_LIGHT) == SRC_NONE);                    // wireless 1 has no light

    // ID list (reversed order): slot n = n-th ID, wherever the sensor sits in the table
    std::string list;
    for (int k = RF_MAX_SENSORS; k >= 1; k--) {
        char e[16];
        snprintf(e, sizeof(e), "%s%08lX", list.empty() ? "" : ", ", (unsigned long)(0x30000000UL + k));
        list += e;
    }
    // the longest list (32 x 8 hex digits, ", " separators) fits the setting
    assert(list.size() == 32 * 8 + 31 * 2 && list.size() < sizeof(gCfg.rfIds));
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "%s", list.c_str());
    assert(rfParseIds(gCfg.rfIds).size() == RF_MAX_SENSORS);
    for (int k = 0; k < RF_MAX_SENSORS; k++) {
        assert(rfSensorIndexForSlot(k) == RF_MAX_SENSORS - 1 - k);
        assert(rfSlotOfIndex(RF_MAX_SENSORS - 1 - k) == k);
    }
    // value sources follow the list: wireless 1 = the first ID (table index 31)
    gRfSensors[31].hasLight = true; gRfSensors[31].lightMs = millis();
    gCfg.srcSel[SG_LIGHT] = SRC_RF1;
    assert(srcResolve(SG_LIGHT) == 31);
    gCfg.srcSel[SG_LIGHT] = SRC_AUTO;
    assert(srcResolve(SG_LIGHT) == 31);
    gRfSensors[31].hasLight = false;
    assert(srcResolve(SG_LIGHT) == SRC_NONE);                    // sensors 4 and 6 (list slots 29, 27) never a source
    // a 33rd ID is ignored; a listed sensor not received has no table entry
    assert(rfParseIds((list + ", 3000AAAA").c_str()).size() == RF_MAX_SENSORS);
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "3000BBBB, 30000002");
    assert(rfSensorIndexForSlot(0) == -1 && rfSensorIndexForSlot(1) == 1 && rfSensorIndexForSlot(2) == -1);
    assert(rfSlotOfIndex(1) == 1 && rfSlotOfIndex(0) == -1);     // not listed: no slot
    // a list stored by an older firmware (buffer of 48 bytes, max 4 IDs) reads the same
    const char* old = "39582376, 30000301, 9000342B, 00002A5B";
    assert(strlen(old) < 48 && rfParseIds(old).size() == 4 && rfParseIds(old)[3] == 0x2A5B);
    gCfg.rfIds[0] = '\0';
    rfSensorsClear();

    // ---- indoor sensors (WH45 / WH46 / WN32P-WH25) never feed temperature /
    // humidity through Auto; "Wireless n" may still pick them ----
    bme280Present = false;
    for (int k = 0; k < SG_COUNT; k++) gCfg.srcSel[k] = SRC_AUTO;
    auto th = [](int i, uint32_t id, uint8_t proto) -> RfSensorReading& {
        RfSensorReading& r = gRfSensors[i];
        r.used = true; r.id = id; r.proto = proto;
        r.hasTemp = r.hasHumidity = true; r.tempC = 20.0f + i; r.humidity = 40;
        r.lastMs = r.tempMs = r.humMs = millis();
        return r;
    };
    th(0, 0x45001234, RF_PROTO_FINEOFFSET);   // WH45 CO2 / PM sensor (indoor), wireless 1
    th(1, 0x250000A7, RF_PROTO_FINEOFFSET);   // WN32P indoor T/H/pressure, wireless 2
    assert(rfIsIndoor(gRfSensors[0]) && rfIsIndoor(gRfSensors[1]));
    assert(srcResolve(SG_AIR) == SRC_NONE);                      // no outdoor sensor: nothing, not the indoor ones
    th(2, 0x46000777, RF_PROTO_FINEOFFSET);   // WH46, wireless 3
    th(3, 0x300001C3, RF_PROTO_FINEOFFSET);   // WH31 outdoor, wireless 4
    assert(rfIsIndoor(gRfSensors[2]) && !rfIsIndoor(gRfSensors[3]));
    assert(srcResolve(SG_AIR) == 3);                             // Auto skips wireless 1-3
    gCfg.srcSel[SG_AIR] = SRC_RF1;
    assert(srcResolve(SG_AIR) == 0);                             // explicit choice: allowed
    gCfg.srcSel[SG_AIR] = SRC_RF1 + 1;
    assert(srcResolve(SG_AIR) == 1);
    gCfg.srcSel[SG_AIR] = SRC_AUTO;
    // the WN32 outdoor variant (pseudo family 0x32) and other protocols are not indoor
    RfSensorReading out = gRfSensors[3]; out.id = 0x32000011;
    assert(!rfIsIndoor(out));
    out.id = 0x45001234; out.proto = RF_PROTO_BRESSER;          // Bresser ID that happens to start with 45
    assert(!rfIsIndoor(out));
    rfSensorsClear();
    return 0;
}
