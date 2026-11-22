#include "series.h"
#include "config.h"
#include "globals.h"
#include "psram.h"
#include "rf_sensors.h"
#include "sources.h"
#include <Arduino.h>
#include <DallasTemperature.h>

extern Config gCfg;

int seriesLimit() { return psramActive() ? SERIES_MAX : 4; }

namespace {

const RfSensorReading* sensorOf(uint32_t id)
{
    if (!gCfg.rfEn) return nullptr;
    for (int i = 0; i < RF_MAX_SENSORS; i++)
        if (gRfSensors[i].used && gRfSensors[i].id == id) return &gRfSensors[i];
    return nullptr;
}

int indexOf(const RfSensorReading* r) { return r ? (int)(r - gRfSensors) : -1; }

// a value the sensor sends in every packet: fresh while the sensor is
bool recent(const RfSensorReading& r) { return millis() - r.lastMs <= rfFreshMs(r); }

// The raw value; lightning: the sensor's counter (the increment is made by the caller)
bool rawValue(const SeriesDef& d, float& v)
{
    const uint8_t f = d.field;
    if (f >= SF_COUNT) return false;
    if (SERIES_FIELDS[f].board) {
        const int p = f - SF_DS1;
        if (p >= bus1Count || bus1Temps[p] == DEVICE_DISCONNECTED_C) return false;
        v = bus1Temps[p];
        return true;
    }
    const RfSensorReading* r = sensorOf(d.rfId);
    if (!r) return false;
    const int i = indexOf(r);
    switch (f) {
        case SF_TEMP:     if (!r->hasTemp || !srcRfValueFresh(i, "temp")) return false; v = r->tempC; return true;
        case SF_HUM:      if (!r->hasHumidity || !srcRfValueFresh(i, "humidity")) return false; v = r->humidity; return true;
        case SF_PRESS:    if (!r->hasPressure || !srcRfValueFresh(i, "pressure")) return false; v = r->pressureHpa; return true;
        case SF_UV:       if (!r->hasUv || !srcRfValueFresh(i, "uv")) return false; v = r->uvIndex; return true;
        case SF_LUX:      if (!r->hasLight || !srcRfValueFresh(i, "light")) return false; v = r->lightLux; return true;
        default: break;
    }
    if (!recent(*r)) return false;
    switch (f) {
        case SF_MOIST:    if (!r->hasMoisture) return false;     v = r->moisturePct; return true;
        case SF_LEAF:     if (!r->hasLeafWetness) return false;  v = r->leafWetnessPct; return true;
        case SF_STRIKES:  if (!r->hasLightning) return false;    v = r->lightningCount; return true;
        case SF_DIST:     if (!r->hasLightning || !r->lightningKm) return false; v = r->lightningKm; return true;   // 0 = none detected
        case SF_PM25:     if (!r->hasPm25) return false;         v = r->pm25; return true;
        case SF_PM10:     if (!r->hasPm10) return false;         v = r->pm10; return true;
        case SF_CO2:      if (!r->hasCo2) return false;          v = r->co2Ppm; return true;
        case SF_EC:       if (!r->hasConductivity) return false; v = r->conductivityUsCm; return true;
        case SF_BATT:     if (!r->batteryMv) return false;       v = r->batteryMv / 1000.0f; return true;
        case SF_SUPERCAP: if (!r->hasSupercap) return false;     v = r->supercapV; return true;
        default:          return false;
    }
}

// lightning counter of the previous cycle per slot (-1 = none yet), and whose
int16_t  prevCount[SERIES_MAX] = { -1, -1, -1, -1, -1, -1, -1, -1 };
uint32_t prevKey[SERIES_MAX]   = {};

} // namespace

bool seriesValueNow(const SeriesDef& d, float& v)
{
    return d.used() && rawValue(d, v);
}

void seriesCycleValues(float v[SERIES_MAX], bool ok[SERIES_MAX])
{
    const int lim = seriesLimit();
    for (int i = 0; i < SERIES_MAX; i++) {
        ok[i] = false;
        v[i] = 0.0f;
        const SeriesDef d = gCfg.series[i];   // a copy: the web task may change it meanwhile
        if (i >= lim || !d.used()) { prevCount[i] = -1; continue; }
        float x;
        const bool have = rawValue(d, x);
        if (d.field != SF_STRIKES) { ok[i] = have; v[i] = x; continue; }
        // strikes: the increase since the previous cycle (8-bit counter, wraps)
        const uint32_t key = seriesKey(d);
        if (key != prevKey[i]) { prevKey[i] = key; prevCount[i] = -1; }
        if (!have) continue;   // keep the previous count: strikes during a gap are counted once it is back
        const int cur = (int)x & 0xFF;
        if (prevCount[i] >= 0) { ok[i] = true; v[i] = (float)((cur - prevCount[i]) & 0xFF); }
        prevCount[i] = (int16_t)cur;
    }
}
