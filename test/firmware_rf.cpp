// Wireless sensor table (src/rf_sensors_data.cpp) and the rain/freshness
// handling of src/sources.cpp: per-message values, slot reuse, expiry,
// rain after an outage.
#include "rf_sensors.h"
#include "sources.h"
#include "globals.h"
#include "derived.h"
#include "params.h"
#include "rf_fsk17.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    assert((s.staleMask & ~(RF_VAL_LIGHT | RF_VAL_PRESS)) == RF_VAL_RAIN);   // (light, pressure: never received)
    // rain back after the outage: increase + gap reported, staleness cleared
    b.rainTotalMm = 106.0f;
    assert(near(rfStoreValues(s, b, millis(), gap), 6.0f) && gap == 360000UL);
    assert(srcRfValueFresh(0, "rain") && (s.staleMask & ~(RF_VAL_LIGHT | RF_VAL_PRESS)) == 0);
    b.rainTotalMm = 106.5f;
    assert(near(rfStoreValues(s, b, millis(), gap), 0.5f) && gap == 0);
    b.rainTotalMm = 0.3f;   // battery change: the sensor's total restarts - held until the next packet confirms it
    assert(rfStoreValues(s, b, millis(), gap) == 0.0f && near(s.rainTotalMm, 106.5f));
    b.rainTotalMm = 0.5f;   // confirmed: the rain since the restart (0.3) and since the held packet (0.2)
    assert(near(rfStoreValues(s, b, millis(), gap), 0.5f) && near(s.rainTotalMm, 0.5f));

    // ---- phantom rain: a rain total that drops or jumps is held until the
    // next packet confirms it (like DavisRainFilter for the Davis tip counter) ----
    {
        RfSensorReading& g = gRfSensors[RF_MAX_SENSORS - 1];   // in the table: rfExpireValues() sees it
        g = RfSensorReading();
        g.used = true; g.id = 0x40012345UL; g.proto = RF_PROTO_FINEOFFSET;
        RfValues rain; rain.ok = RF_VAL_RAIN;
        unsigned long t = 10000000UL;
        auto feed = [&](float totalMm, unsigned long afterMs) { t += afterMs; rain.rainTotalMm = totalMm; return rfStoreValues(g, rain, t, gap); };
        assert(feed(1500.0f, 0) == 0.0f);                             // first value: reference only
        assert(near(feed(1500.2f, 49000), 0.2f));
        assert(feed(12.3f, 49000) == 0.0f && near(g.rainTotalMm, 1500.2f));   // one corrupt (lower) total: held
        assert(near(feed(1500.4f, 49000), 0.2f));                     // back on the old track: the held one is dropped
        assert(feed(4000.0f, 49000) == 0.0f && near(g.rainTotalMm, 1500.4f));  // corrupt jump up: held
        assert(near(feed(1500.6f, 49000), 0.2f));                     // dropped
        assert(feed(12.3f, 49000) == 0.0f && feed(3000.0f, 49000) == 0.0f);    // two contradicting corrupt totals
        assert(near(feed(1500.8f, 49000), 0.2f) && near(g.rainTotalMm, 1500.8f));
        // a real cloudburst of 8 mm in one 49-s interval: held, then confirmed and counted
        assert(feed(1508.8f, 49000) == 0.0f);
        assert(near(feed(1509.0f, 49000), 8.2f) && near(g.rainTotalMm, 1509.0f));
        // a confirmed jump far beyond any rain rate (100 mm in 98 s): a new
        // reference, only the rain after the held packet counts
        assert(feed(1609.0f, 49000) == 0.0f);
        assert(near(feed(1609.2f, 49000), 0.2f) && near(g.rainTotalMm, 1609.2f));
        // the 16-bit counter wraps (WH40: 6553.5 mm): a confirmed restart counts what the new counter holds
        g.rainTotalMm = 6553.4f;
        assert(feed(0.1f, 49000) == 0.0f);
        assert(near(feed(0.2f, 49000), 0.2f));   // (the 0.1 mm before the wrap is not in the sensor's counter any more)
        // after a long outage the total may rise more: 40 mm after 30 min is taken at once
        assert(near(feed(40.2f, 30 * 60000UL), 40.0f) && gap == 30 * 60000UL);
        // a held value keeps the rain value stale (no fresh rain from an unconfirmed packet)
        rfExpireValues(t + SRC_RF_FRESH_MS + 1);
        assert(g.staleMask & RF_VAL_RAIN);
        t += SRC_RF_FRESH_MS + 1;
        assert(feed(5.0f, 0) == 0.0f && (g.staleMask & RF_VAL_RAIN));
        // Davis (already filtered by DavisRainFilter) and network sources: taken as they come
        RfSensorReading d;
        d.used = true; d.id = RF_ID_DAVIS | 1; d.proto = RF_PROTO_DAVIS;
        rain.rainTotalMm = 10.0f; rfStoreValues(d, rain, t, gap);
        rain.rainTotalMm = 20.0f;
        assert(near(rfStoreValues(d, rain, t + 10000, gap), 10.0f));
        g = RfSensorReading();
    }

    // ---- expiry survives the millis() wrap: once stale, a value stays
    // stale even if its timestamp looks recent again ----
    delay(SRC_RF_FRESH_MS + 1000);
    rfExpireValues(millis());
    assert(s.staleMask == (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_UV | RF_VAL_RAIN | RF_VAL_LIGHT | RF_VAL_PRESS | RF_VAL_ANY));
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
    // IT+ new battery (new random ID): takes the slot of an IT+ sensor silent
    // for a minute, not only after the 5-min freshness window
    {
        const unsigned long now = millis();
        for (int k = 0; k < RF_MAX_SENSORS; k++) {
            RfSensorReading& r = gRfSensors[k];
            r.used = true; r.id = rfItPlusId(true, (uint8_t)k, true); r.proto = RF_PROTO_ITPLUS17; r.lastMs = now - 1000;
        }
        gRfSensors[5].lastMs = now - 50000UL;   // silent 50 s: not yet
        assert(rfSlotFor(rfItPlusId(true, 40, true), now, 60000UL, RF_PROTO_ITPLUS17) == -1);
        gRfSensors[5].lastMs = now - 120000UL;  // silent 2 min: the one whose battery was changed
        assert(rfSlotFor(rfItPlusId(true, 40, true), now, 60000UL, RF_PROTO_ITPLUS17) == 5);
        // a 10-min air quality sensor keeps its place between packets despite a short take-over limit
        rfSensorsClear();
        for (int k = 0; k < RF_MAX_SENSORS; k++) {
            RfSensorReading& r = gRfSensors[k];
            r.used = true; r.id = 0x46000000UL | (uint32_t)k; r.proto = RF_PROTO_FINEOFFSET; r.lastMs = now - 1000;
        }
        gRfSensors[3].lastMs = now - 8 * 60000UL;
        assert(rfFreshMs(gRfSensors[3]) > SRC_RF_FRESH_MS);
        assert(rfSlotFor(0x12345678UL, now, 60000UL) == -1);
        rfSensorsClear();
    }

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

    // ---- wireless 10-min wind: the window covers 10 min at any packet rate
    // (Davis ISS ID 1 2.5625 s, WS80 4.75 s, 12 s) ----
    gCfg.srcSel[SG_WIND] = SRC_RF1;
    w.hasWind = true; w.windDirDeg = 90.0f;
    for (unsigned long periodMs : { 2563UL, 4750UL, 12000UL }) {
        srcRfWindReset(0);
        const unsigned long t0 = millis();
        for (unsigned long t = 0; t < 780000UL; t = millis() - t0) {   // 3 min calm, 7 min at 10 m/s (gust 12), 3 min calm
            const bool windy = t >= 180000UL && t < 600000UL;
            w.windAvgMs = windy ? 10.0f : 0.0f; w.windGustMs = windy ? 12.0f : 0.0f;
            w.windMs = w.lastMs = millis();
            srcRfPacket(0, true, 0.0f, false);
            delay(periodMs);
        }
        const WindStats ten = srcRfWind(0, true);
        std::printf("10-min wind, a packet every %lu ms: mean %.2f m/s, gust %.1f m/s\n", periodMs, ten.meanMs, ten.gustMs);
        assert(ten.valid && std::fabs(ten.meanMs - 7.0f) < 0.3f && near(ten.gustMs, 12.0f) && ten.dirDeg == 90);
        const WindStats all = srcRfWind(0, false);     // the path window: everything since its reset (~13 min)
        assert(all.valid && all.meanMs > 4.5f && all.meanMs < 5.6f && near(all.gustMs, 12.0f));
        delay(616000UL);                                        // silent for more than 10 min: nothing left
        assert(!srcRfWind(0, true).valid);
    }
    w.hasWind = false;
    gCfg.srcSel[SG_WIND] = SRC_AUTO;
    // ---- 32 sensors: slot mapping with and without an ID list; only
    // slots 1-4 are value sources (SRC_RF1..(SRC_RF1 + 3) keep their meaning) ----
    static_assert(RF_MAX_SENSORS == 32 && RF_SOURCE_SLOTS == 4, "32 received, 4 source slots");
    static_assert(SRC_AUTO == 0 && SRC_BOARD == 1 && SRC_RF1 == 2 && SRC_OFF == 34,
                  "parameters 45-49 / 54: Wireless n = n + 1, Off = 34");
    assert(srcSelSlot(SRC_RF1) == 0 && srcSelSlot(SRC_RF1 + 31) == 31 && srcSelSlot(SRC_OFF) == -1 && srcSelSlot(SRC_BOARD) == -1);
    assert(srcSelValid(SRC_OFF) && !srcSelValid(SRC_OFF + 1) && !srcSelValid(-1));
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
    gCfg.srcSel[SG_LIGHT] = (SRC_RF1 + 3);
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
    assert(srcResolve(SG_LIGHT) == 5);                           // ★ sensors: Auto walks all of them in their order - sensor 6 (★ 27) before sensor 4 (★ 29)
    // a 33rd ID is ignored; a listed sensor not received has no table entry
    assert(rfParseIds((list + ", 3000AAAA").c_str()).size() == RF_MAX_SENSORS);
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "3000BBBB, 30000002");
    assert(rfSensorIndexForSlot(0) == -1 && rfSensorIndexForSlot(1) == 1);
    // the other (not listed) sensors follow the list in table order: idx 0 -> 2, idx 2 -> 3, ...
    assert(rfSlotOfIndex(1) == 1 && rfSlotOfIndex(0) == 2 && rfSlotOfIndex(2) == 3 && rfSensorIndexForSlot(2) == 0);
    assert(rfSlotOfIndex(31) == -1);                             // number 33: none
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

    // ---- pressure as a value source (sources.pressure) ----
    static_assert(SG_PRESS == 5 && SG_COUNT == 6, "pressure is the last group: src0..src4 / params 45-49 / bits 0-9 unchanged");
    assert(srcParamId(SG_AIR) == 45 && srcParamId(SG_LIGHT) == 49 && srcParamId(SG_PRESS) == 54);
    assert(!strcmp(srcGroupKey(SG_PRESS), "pressure"));
    // QNH: the BME280 / Adafruit formula
    const double qnh = 946.8 / pow(1.0 - 560.0 / 44330.0, 5.255);
    assert(std::fabs(seaLevelPressureHpa(946.8f, 560) - qnh) < 0.01 && std::fabs(seaLevelPressureHpa(1000.0f, 0) - 1000.0f) < 1e-3);
    // a Fine Offset frame's pressure becomes a timestamped value group
    FoFrame fp; fp.hasPressure = true; fp.pressureHpa = 946.8f; fp.hasTemp = true; fp.tempC = 21.0f;
    RfValues pv = rfFineOffsetValues(fp);
    assert((pv.ok & RF_VAL_PRESS) && std::fabs(pv.pressureHpa - 946.8f) < 1e-3);
    // Auto with a BME280: the BME280, exactly as before
    gCfg.altitudeM = 560;
    for (int k = 0; k < SG_COUNT; k++) gCfg.srcSel[k] = SRC_AUTO;
    bme280Present = true; bme280PressAbs = 950.0f; bme280PressSea = 1016.5f;
    RfSensorReading& wn = gRfSensors[0];
    wn.used = true; wn.id = 0x25000050; wn.proto = RF_PROTO_FINEOFFSET;   // WN32P (indoor: fine for pressure)
    unsigned long g2;
    rfStoreValues(wn, pv, millis(), g2);
    assert(wn.hasPressure && wn.pressMs == millis());
    assert(srcResolve(SG_PRESS) == SRC_IS_BOARD);
    srcUpdateCycle();
    assert(gSrc.pressValid && gSrc.pressAbsHpa == 950.0f && gSrc.pressSeaHpa == 1016.5f);
    assert(((srcBits() >> 10) & 3) == 1);                      // LoRaWAN bits 10-11: board
    const uint16_t otherBits = srcBits() & 0x03FC;              // wind, rain, UV, light (air follows the BME280)
    // no BME280: the wireless sensor (also an indoor one), reduced with the altitude
    bme280Present = false;
    assert(srcResolve(SG_PRESS) == 0);
    srcUpdateCycle();
    assert(gSrc.pressValid && std::fabs(gSrc.pressAbsHpa - 946.8f) < 1e-3 && std::fabs(gSrc.pressSeaHpa - qnh) < 0.01);
    assert(((srcBits() >> 10) & 3) == 2 && (srcBits() & 0x03FC) == otherBits);   // pressure wireless, other groups unchanged
    char d[24]; srcDescribe(SG_PRESS, d, sizeof(d));
    assert(!strcmp(d, "radio:25000050"));
    // explicit board without a BME280: nothing; Off: nothing even with a BME280
    gCfg.srcSel[SG_PRESS] = SRC_BOARD;
    srcUpdateCycle();
    assert(!gSrc.pressValid && ((srcBits() >> 10) & 3) == 0);
    gCfg.srcSel[SG_PRESS] = SRC_RF1;
    assert(srcResolve(SG_PRESS) == 0);
    bme280Present = true;
    gCfg.srcSel[SG_PRESS] = SRC_OFF;
    srcUpdateCycle();
    assert(!gSrc.pressValid);
    gCfg.srcSel[SG_PRESS] = SRC_RF1;
    srcUpdateCycle();
    assert(gSrc.pressValid && std::fabs(gSrc.pressAbsHpa - 946.8f) < 1e-3);   // wireless 1 chosen over the BME280
    // freshness: after 5 min without a pressure packet (other values still coming) the value drops out
    bme280Present = false;
    gCfg.srcSel[SG_PRESS] = SRC_AUTO;
    delay(SRC_RF_FRESH_MS + 1000);
    RfValues tv; tv.ok = RF_VAL_TEMP; tv.tempC = 21.5f;
    rfStoreValues(wn, tv, millis(), g2);
    rfExpireValues(millis());
    assert(!srcRfValueFresh(0, "pressure") && srcRfValueFresh(0, "temp"));
    srcUpdateCycle();
    assert(!gSrc.pressValid && srcResolve(SG_PRESS) == SRC_NONE);
    rfStoreValues(wn, pv, millis(), g2);
    srcUpdateCycle();
    assert(gSrc.pressValid);
    // a Bresser message never sets the pressure bit (its msg_ok has no such bit)
    assert((RF_VAL_PRESS & (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_RAIN | RF_VAL_UV | RF_VAL_LIGHT)) == 0);
    // parameter 54 (downlink / MQTT / web), range 0..6
    assert(applyParam(PARAM_SRC_PRESSURE, SRC_BOARD) && gCfg.srcSel[SG_PRESS] == SRC_BOARD);
    assert(!applyParam(PARAM_SRC_PRESSURE, 35) && gCfg.srcSel[SG_PRESS] == SRC_BOARD);
    assert(applyParam(PARAM_SRC_LIGHT, (SRC_RF1 + 3)) && gCfg.srcSel[SG_LIGHT] == (SRC_RF1 + 3) && gCfg.srcSel[SG_PRESS] == SRC_BOARD);
    gCfg.srcSel[SG_LIGHT] = gCfg.srcSel[SG_PRESS] = SRC_AUTO;
    // a config without the new key: Auto (Config default)
    Config fresh;
    assert(sizeof(fresh.srcSel) == SG_COUNT && fresh.srcSel[SG_PRESS] == SRC_AUTO);
    rfSensorsClear();

    // ---- sensor states: Mine (rfIds), Received, Hidden (rfIgnore), Only my sensors ----
    // migration: a stored ID list = the old filter = "Only my sensors" on
    assert(rfOnlyMineDefault("") == 0 && rfOnlyMineDefault(nullptr) == 0 && rfOnlyMineDefault("39582376") == 1);
    assert(fresh.rfOnlyMine == 0 && fresh.rfIgnore[0] == '\0');
    gCfg.rfIds[0] = gCfg.rfIgnore[0] = '\0';
    gCfg.rfOnlyMine = 0;
    const uint32_t A = 0x30000101, B = 0x30000202, C = 0x9000342B, D = 0x39582376, E = 0x25000050, N = 0x0000BEEF;
    assert(rfStateOf(A) == RF_STATE_RECEIVED && rfIdAccepted(A) && !rfOnlyMineActive());
    // marking moves an ID between the lists, normalised to 8 hex digits
    assert(rfSetState(D, RF_STATE_MINE) && rfSetState(B, RF_STATE_MINE) && rfSetState(E, RF_STATE_MINE));
    assert(!strcmp(gCfg.rfIds, "39582376, 30000202, 25000050"));
    assert(rfSetState(N, RF_STATE_HIDDEN) && !strcmp(gCfg.rfIgnore, "0000BEEF"));
    assert(rfSetState(B, RF_STATE_HIDDEN) && !strcmp(gCfg.rfIds, "39582376, 25000050") && !strcmp(gCfg.rfIgnore, "0000BEEF, 30000202"));
    assert(rfSetState(B, RF_STATE_MINE) && !strcmp(gCfg.rfIds, "39582376, 25000050, 30000202") && !strcmp(gCfg.rfIgnore, "0000BEEF"));
    assert(rfSetState(B, RF_STATE_MINE) && !strcmp(gCfg.rfIds, "39582376, 25000050, 30000202"));   // no duplicate
    assert(rfMoveMine(B, -1) && !strcmp(gCfg.rfIds, "39582376, 30000202, 25000050"));
    assert(!rfMoveMine(D, -1) && !rfMoveMine(E, 1) && !rfMoveMine(A, 1));
    assert(rfStateOf(N) == RF_STATE_HIDDEN && !rfIdAccepted(N) && rfIdAccepted(A) && rfIdAccepted(D));
    // a full list refuses (32 IDs), nothing changes
    {
        Config keep = gCfg;
        std::vector<uint32_t> many;
        for (uint32_t k = 1; k <= RF_MAX_SENSORS; k++) many.push_back(0x40000000u + k);
        rfFormatIds(many, gCfg.rfIgnore, sizeof(gCfg.rfIgnore));
        assert(rfParseIds(gCfg.rfIgnore).size() == RF_MAX_SENSORS);
        assert(!rfSetState(A, RF_STATE_HIDDEN) && rfStateOf(A) == RF_STATE_RECEIVED);
        assert(rfSetState(0x40000001u, RF_STATE_HIDDEN));          // already in it: fine
        gCfg = keep;
    }
    // slots with mixed Mine / Received: Mine first in list order (also not received),
    // the others after them in table (reception) order
    rfSensorsClear();
    auto put = [](int i, uint32_t id) { gRfSensors[i].used = true; gRfSensors[i].id = id; gRfSensors[i].lastMs = millis();
                                        gRfSensors[i].proto = RF_PROTO_FINEOFFSET; };
    put(0, A); put(1, B); put(2, C); put(3, D);          // list: D, B, E (E not received)
    assert(rfSlotOfIndex(3) == 0 && rfSlotOfIndex(1) == 1);
    assert(rfSlotOfIndex(0) == 3 && rfSlotOfIndex(2) == 4);      // A: 3 + 0, C: 3 + 2 - 1 Mine before it
    assert(rfSensorIndexForSlot(0) == 3 && rfSensorIndexForSlot(1) == 1 && rfSensorIndexForSlot(2) == -1);
    assert(rfSensorIndexForSlot(3) == 0 && rfSensorIndexForSlot(4) == 2 && rfSensorIndexForSlot(5) == -1);
    // the value sources follow: wireless 1 = D, wireless 4 = A
    gRfSensors[0].hasLight = true; gRfSensors[0].lightMs = millis();
    gCfg.srcSel[SG_LIGHT] = (SRC_RF1 + 3);
    assert(srcResolve(SG_LIGHT) == 0);
    gCfg.srcSel[SG_LIGHT] = SRC_AUTO;
    assert(srcResolve(SG_LIGHT) == SRC_NONE);                    // Auto with ★ sensors: only those (A is not ★)
    gRfSensors[1].hasLight = true; gRfSensors[1].lightMs = millis();
    assert(srcResolve(SG_LIGHT) == 1);                           // B = ★ 2
    gRfSensors[3].hasLight = true; gRfSensors[3].lightMs = millis();
    assert(srcResolve(SG_LIGHT) == 3);                           // D = ★ 1 comes first
    gRfSensors[1].hasLight = gRfSensors[3].hasLight = false;
    // without any Mine ID: slot = table index, as before
    {
        char keep[sizeof(gCfg.rfIds)]; strcpy(keep, gCfg.rfIds);
        gCfg.rfIds[0] = '\0';
        for (int i = 0; i < 4; i++) assert(rfSlotOfIndex(i) == i && rfSensorIndexForSlot(i) == i);
        strcpy(gCfg.rfIds, keep);
    }
    // "Only my sensors": the others are no longer accepted and leave the table, Mine keep their values
    gRfSensors[3].hasTemp = true; gRfSensors[3].tempC = 7.5f;
    gCfg.rfOnlyMine = 1;
    assert(rfOnlyMineActive() && !rfIdAccepted(A) && rfIdAccepted(D) && !rfIdAccepted(N));
    rfRemoveRejected();
    assert(!gRfSensors[0].used && gRfSensors[1].used && !gRfSensors[2].used && gRfSensors[3].used && gRfSensors[3].tempC == 7.5f);
    assert(rfSlotOfIndex(3) == 0 && rfSlotOfIndex(1) == 1);
    // with an empty list it has no effect (would accept nothing)
    {
        char keep[sizeof(gCfg.rfIds)]; strcpy(keep, gCfg.rfIds);
        gCfg.rfIds[0] = '\0';
        assert(!rfOnlyMineActive() && rfIdAccepted(A) && !rfIdAccepted(N));
        strcpy(gCfg.rfIds, keep);
    }
    gCfg.rfOnlyMine = 0;
    // hiding a received sensor frees exactly its entry; the others keep their place
    put(0, A); put(2, C);
    assert(rfSetState(C, RF_STATE_HIDDEN));
    rfRemoveRejected();
    assert(gRfSensors[0].used && gRfSensors[1].used && !gRfSensors[2].used && gRfSensors[3].used);
    assert(rfSlotOfIndex(0) == 3);
    // a Mine sensor gets an entry even when the table is full of others heard just now
    rfSensorsClear();
    for (int i = 0; i < RF_MAX_SENSORS; i++) put(i, 0x50000000u + i);
    assert(rfSlotFor(0x50000999u, millis()) == -1);              // an other one: no room
    gRfSensors[7].lastMs = millis() - 60000;                     // heard longest ago
    int mi = rfSlotFor(E, millis(), SRC_RF_FRESH_MS, 0, true);
    assert(mi == 7 && !gRfSensors[7].used);
    // hidden sensors in range: only hidden IDs heard within 10 min
    rfSensorsClear();
    rfNoteHidden(N, "WH31", -80.0f, millis());
    rfNoteHidden(N, nullptr, -78.0f, millis());
    rfNoteHidden(A, nullptr, -70.0f, millis());                  // not hidden (any more): not counted
    assert(rfHiddenInRange(millis()) == 1);
    {
        bool found = false;
        for (const RfHiddenSeen& h : gRfHidden)
            if (h.id == N) { found = true; assert(h.packets == 2 && h.rssi == -78.0f && !strcmp(h.model, "WH31")); }
        assert(found);
    }
    delay(RF_HIDDEN_RANGE_MS + 1000);
    assert(rfHiddenInRange(millis()) == 0);
    rfExpireValues(millis());   // out of range latched: 49.7 days later the old time stamp looks recent again
    for (RfHiddenSeen& h : gRfHidden) if (h.id == N) h.lastMs = millis() - 1000;
    assert(rfHiddenInRange(millis()) == 0);
    rfNoteHidden(N, nullptr, -78.0f, millis());   // heard again: in range
    assert(rfHiddenInRange(millis()) == 1);
    for (uint32_t k = 0; k < 20; k++) rfNoteHidden(0x60000000u + k, nullptr, -90.0f, millis());   // more than the list holds
    // parameters 55 / 56
    assert(applyParam(PARAM_RF_ONLY_MINE, 1) && gCfg.rfOnlyMine == 1 && !applyParam(PARAM_RF_ONLY_MINE, 2));
    assert(applyParam(PARAM_RF_ONLY_MINE, 0) && gCfg.rfOnlyMine == 0);
    assert(gCfg.rfIgnore[0] && !applyParam(PARAM_RF_HIDDEN_CLEAR, 1) && gCfg.rfIgnore[0]);
    assert(applyParam(PARAM_RF_HIDDEN_CLEAR, 0) && gCfg.rfIgnore[0] == '\0');
    gCfg.rfIds[0] = '\0';
    rfSensorsClear();
    // ---- Ecowitt-style short IDs (as the Ecowitt app shows them) ----
    {
        uint32_t e;
        assert(rfEcowittId(0x900181A2, e) && e == 0x181A2);      // WS90: the low 24 bits
        assert(rfEcowittId(0x300002B2, e) && e == 0xB2);         // WH31: the 8-bit ID (channel in bits 8-15)
        assert(rfEcowittId(0x25000050, e) && e == 0x50);         // WN32P
        assert(rfEcowittId(0x5530CF55, e) && e == 0x30CF55);     // WH55: ID incl. its channel bits
        assert(!rfEcowittId(0x39582376, e) && !rfEcowittId(0x1700002C, e) && !rfEcowittId(0xDA000001, e));   // Bresser, IT+, Davis
        assert(!rfEcowittId(0x1700012C, e));                     // IT+ with humidity
        // IT+ twins: the humidity variant gets its own ID; the twin of a non-IT+ ID is 0
        assert(rfItPlusId(true, 0x08, false) == 0x17000008 && rfItPlusId(true, 0x08, true) == 0x17000108);
        assert(rfItPlusId(false, 0x48, false) == 0x09000048);                  // 9.579 kbit/s, second probe
        assert(rfItPlusTwin(0x17000008) == 0x17000108 && rfItPlusTwin(0x17000108) == 0x17000008);
        assert(rfItPlusTwin(0x09000048) == 0x09000148);
        assert(!rfItPlusTwin(0x900181A2) && !rfItPlusTwin(0xDA000001) && !rfItPlusTwin(0x17010008));
        {
            for (auto& x : gRfSensors) x = RfSensorReading();
            gRfSensors[3].used = true; gRfSensors[3].id = 0x17000008;
            assert(rfTwinIndex(0x17000008) < 0);
            gRfSensors[5].used = true; gRfSensors[5].id = 0x17000108;
            assert(rfTwinIndex(0x17000008) == 5 && rfTwinIndex(0x17000108) == 3);
            for (auto& x : gRfSensors) x = RfSensorReading();
        }
        // short IDs in the lists are replaced by the full ID once the sensor is heard; order kept, no duplicates
        snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "39582376, 181A2, B2");
        snprintf(gCfg.rfIgnore, sizeof(gCfg.rfIgnore), "30CF55");
        assert(rfNormalizeShortId(0x900181A2));
        assert(std::string(gCfg.rfIds) == "39582376, 900181A2, 000000B2");
        assert(!rfNormalizeShortId(0x900181A2));                  // done
        assert(rfNormalizeShortId(0x300002B2) && std::string(gCfg.rfIds) == "39582376, 900181A2, 300002B2");
        assert(rfStateOf(0x300002B2) == RF_STATE_MINE);
        assert(rfNormalizeShortId(0x5530CF55) && std::string(gCfg.rfIgnore) == "5530CF55" && rfStateOf(0x5530CF55) == RF_STATE_HIDDEN);
        // a Bresser sensor never matches a short entry
        snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "2376");
        assert(!rfNormalizeShortId(0x39582376) && std::string(gCfg.rfIds) == "2376");
        // the full ID already listed as well: one entry left
        snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "900181A2, 181A2");
        assert(rfNormalizeShortId(0x900181A2) && std::string(gCfg.rfIds) == "900181A2");
        gCfg.rfIds[0] = gCfg.rfIgnore[0] = '\0';
    }
    // ---- freshness per family (Ecowitt send intervals): the 10-min WH41 stays fresh, a WS90 not ----
    {
        RfSensorReading aq; aq.used = true; aq.proto = RF_PROTO_FINEOFFSET; aq.id = 0x410000C7;
        RfSensorReading ws; ws.used = true; ws.proto = RF_PROTO_FINEOFFSET; ws.id = 0x900181A2;
        RfSensorReading br; br.used = true; br.proto = RF_PROTO_BRESSER;   br.id = 0x39582376;
        assert(rfNominalIntervalMs(aq) == 600000 && rfFreshMs(aq) == 1500000);   // 2.5 x 10 min
        assert(rfFreshMs(ws) == SRC_RF_FRESH_MS && rfFreshMs(br) == SRC_RF_FRESH_MS);
        RfSensorReading p57; p57.proto = RF_PROTO_FINEOFFSET; p57.id = 0x5700E21A;
        assert(rfNominalIntervalMs(p57) == 79000 && rfFreshMs(p57) == SRC_RF_FRESH_MS);   // 2.5 x 79 s < 5 min
        // stale marks after 12 min: the WS90 yes, the WH41 no
        gRfSensors[0] = aq; gRfSensors[1] = ws;
        RfValues v; v.ok = RF_VAL_TEMP | RF_VAL_HUM; v.tempC = 20; v.humidity = 50;
        unsigned long g2;
        rfStoreValues(gRfSensors[0], v, millis(), g2);
        rfStoreValues(gRfSensors[1], v, millis(), g2);
        delay(12 * 60000UL);
        rfExpireValues(millis());
        assert(srcRfValueFresh(0, "temp") && !(gRfSensors[0].staleMask & RF_VAL_TEMP));
        assert(!srcRfValueFresh(1, "temp") && (gRfSensors[1].staleMask & RF_VAL_TEMP));
        delay(14 * 60000UL);   // 26 min: now the WH41 is stale too
        rfExpireValues(millis());
        assert(!srcRfValueFresh(0, "temp"));
        gRfSensors[0] = RfSensorReading(); gRfSensors[1] = RfSensorReading();
    }
    // ---- Auto value sources: Ecowitt's sensor order among wireless 1-4 ----
    {
        RfSensorReading s;
        s.proto = RF_PROTO_FINEOFFSET;
        s.id = 0x32000011; assert(srcHierarchyRank(s, SG_AIR) == 0);   // WN32 outdoor first
        s.id = 0x900181A2; assert(srcHierarchyRank(s, SG_AIR) == 1 && srcHierarchyRank(s, SG_WIND) == 0 && srcHierarchyRank(s, SG_RAIN) == 3);
        s.id = 0x40001234; assert(srcHierarchyRank(s, SG_RAIN) == 0);   // WH40 before WN20, WH65 and the piezo arrays
        s.id = 0x20001234; assert(srcHierarchyRank(s, SG_RAIN) == 1);
        s.id = 0x24001234; assert(srcHierarchyRank(s, SG_RAIN) == 2 && srcHierarchyRank(s, SG_WIND) == 3);
        s.id = 0x68001234; assert(srcHierarchyRank(s, SG_WIND) == 4 && srcHierarchyRank(s, SG_LIGHT) == 3);
        s.id = 0x30000201; assert(srcHierarchyRank(s, SG_AIR) == 9);   // WH31: by number
        s.proto = RF_PROTO_BRESSER; s.id = 0x90000000; assert(srcHierarchyRank(s, SG_WIND) == 9);   // only Fine Offset families

        // resolve: WH65, WS90, WH40 (as ★ 1-3, then without ★)
        snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "24000001, 90000002, 40000003");
        for (int k = 0; k < 3; k++) {
            RfSensorReading& x = gRfSensors[k];
            x = RfSensorReading(); x.used = true; x.proto = RF_PROTO_FINEOFFSET;
            x.id = k == 0 ? 0x24000001 : k == 1 ? 0x90000002 : 0x40000003;
            RfValues vv;
            vv.ok = k == 2 ? RF_VAL_RAIN : (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_RAIN);
            vv.tempC = 10; vv.humidity = 60; vv.windAvgMs = 2; vv.rainTotalMm = 1;
            unsigned long g3;
            rfStoreValues(x, vv, millis(), g3);
        }
        gCfg.srcSel[SG_AIR] = gCfg.srcSel[SG_WIND] = gCfg.srcSel[SG_RAIN] = SRC_AUTO;
        const bool bme = bme280Present;
        const int we = gCfg.windEn, re = gCfg.rainEn;
        bme280Present = false; gCfg.windEn = gCfg.rainEn = 0;   // no board sensors (they would come first)
        // ★ sensors: their number decides (WH65 = ★ 1 for everything it sends)
        assert(srcResolve(SG_WIND) == 0 && srcResolve(SG_AIR) == 0 && srcResolve(SG_RAIN) == 0);
        gCfg.srcSel[SG_WIND] = SRC_RF1 + 1;   // chosen explicitly: kept
        assert(srcResolve(SG_WIND) == 1);
        gCfg.srcSel[SG_WIND] = SRC_AUTO;
        // no ★ sensors: wireless 1-4 in Ecowitt's order - wind and air from the WS90, rain from the WH40
        gCfg.rfIds[0] = '\0';
        assert(srcResolve(SG_WIND) == 1 && srcResolve(SG_AIR) == 1 && srcResolve(SG_RAIN) == 2);
        bme280Present = bme; gCfg.windEn = we; gCfg.rainEn = re;
        for (int k = 0; k < 3; k++) gRfSensors[k] = RfSensorReading();
        gCfg.rfIds[0] = '\0';
    }
    return 0;
}
