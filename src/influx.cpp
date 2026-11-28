#include "influx.h"
#include "globals.h"
#include "config.h"
#include "series.h"
#include "sensors.h"
#include "sources.h"
#include "rf_sensors.h"
#include "rf_names.h"
#include "board_io.h"
#include <esp_timer.h>
#include <cmath>
#include <cstdio>

// api_info.cpp (weak); the host test overrides it. 0 = free heap, 1 = min ever.
size_t hostOrDeviceFreeHeap(int what);

namespace {

// A line-protocol field value: integers without a decimal point, otherwise up
// to three decimals. All fields are written as floats (no "i" suffix) so a
// value that is whole in one write and fractional in the next keeps one type.
void num(std::string& out, double v)
{
    char b[32];
    if (v == (double)(long long)v && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.3f", v);
    out += b;
}

// Escape a tag key / value: space, comma, equals and the backslash itself get a
// backslash (a trailing one would otherwise escape the separator after it);
// control characters are dropped (a newline would end the line).
void escTag(std::string& out, const char* s)
{
    for (; s && *s; s++) {
        const char c = *s;
        if ((unsigned char)c < 0x20 || c == 0x7F) continue;
        if (c == ' ' || c == ',' || c == '=' || c == '\\') out += '\\';
        out += c;
    }
}

// The measurement name: comma and space escaped (an equals sign is allowed there).
std::string escMeasurement(const char* s)
{
    std::string out;
    for (; s && *s; s++) {
        const char c = *s;
        if ((unsigned char)c < 0x20 || c == 0x7F) continue;
        if (c == ' ' || c == ',' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// Append ",key=" escaped value, but only when the value is non-empty (Influx
// forbids empty tag values).
void tag(std::string& out, const char* key, const char* val)
{
    if (!val || !val[0]) return;
    out += ','; out += key; out += '='; escTag(out, val);
}

// A field, prefixed with ',' unless it is the first in the line. NaN / Inf are
// left out: writing them as 0 would store a wrong reading.
void field(std::string& out, bool& first, const char* key, double v)
{
    if (std::isnan(v) || std::isinf(v)) return;
    out += first ? ' ' : ',';
    first = false;
    out += key; out += '='; num(out, v);
}

} // namespace

std::string influxAuthHeader(const char* token)
{
    std::string t = token ? token : "";
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.empty() || t.find(' ') != std::string::npos) return t;
    return "Token " + t;
}

void buildInfluxLineProtocol(std::string& out, int64_t epochSec, int rssiDbm, bool wifiConnected, const char* measurement)
{
    out.clear();
    out.reserve(2048);
    const std::string meas = escMeasurement(measurement && measurement[0] ? measurement : "weather");
    char ts[24];
    snprintf(ts, sizeof ts, " %lld\n", (long long)epochSec * 1000000000LL);   // nanoseconds

    // ---- main line: measurement + host tag + the selected weather + system values ----
    {
        std::string line = meas;
        tag(line, "host", gCfg.deviceName);
        bool first = true;
        if (gSrc.tempValid)                  field(line, first, "temperature", gSrc.tempC);
        if (gSrc.tempValid && gSrc.humValid) field(line, first, "humidity", gSrc.humidity);
        if (gSrc.pressValid) {
            field(line, first, "pressure_abs", gSrc.pressAbsHpa);
            field(line, first, "pressure_sea", gSrc.pressSeaHpa);
        }
        if (srcActive(SG_WIND) && windCycleValid) {
            field(line, first, "wind_speed", windSpeedMs);
            field(line, first, "wind_gust", windGustMs);
            field(line, first, "wind_dir", windDirection);
        }
        if (srcActive(SG_RAIN)) {
            field(line, first, "rain_rate", rainRateMmH);
            field(line, first, "rain_total", rainMMSinceStart);   // running total since the start (like the _rf lines' counter)
        }
        if (gSrc.uvRadio)                                  field(line, first, "uv", gSrc.uvIndexRadio);
        else if (gSrc.uvBoard && !std::isnan(gIo.uvIndex)) field(line, first, "uv", gIo.uvIndex);
        if (gSrc.lightValid)                               field(line, first, "illuminance", gSrc.lightLux);
        for (int i = 0; i < SERIES_MAX; i++) {   // custom series (series.h): series_<slot>, metric units
            float v;
            char key[12];
            snprintf(key, sizeof key, "series_%d", i + 1);
            if (seriesNow(i, v)) field(line, first, key, v);
        }
        field(line, first, "uptime", (double)(esp_timer_get_time() / 1000000LL));
        field(line, first, "heap_free", (double)hostOrDeviceFreeHeap(0));
        if (wifiConnected) field(line, first, "rssi", (double)rssiDbm);
        if (!first) { out += line; out += ts; }   // at least one field: skip an empty line
    }

    // ---- one line per wireless sensor ----
    if (!rfTableOn()) return;
    const std::string rfMeas = meas + "_rf";
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (!r.used || !rfSensorFresh(r)) continue;   // silent: no line (old values would get today's timestamp)
        std::string line = rfMeas;
        // A5c D7: a ★ sensor by its custom ID (stays across a battery change; a new
        // name starts a new series under `name` - group by `sensor`), a
        // received-only one by its radio ID
        const RfLabel l = rfLabelOf(r.id);
        tag(line, "sensor", l.customId);
        tag(line, "radio_id", l.radio);
        tag(line, "host", gCfg.deviceName);
        if (r.model) tag(line, "model", r.model);
        tag(line, "name", l.displayName);   // only if set
        if (r.channel) { char ch[4]; snprintf(ch, sizeof ch, "%u", r.channel); tag(line, "channel", ch); }
        bool first = true;
        field(line, first, "rssi", r.rssi);
        field(line, first, "packets", (double)r.packets);
        field(line, first, "battery_ok", r.batteryOk ? 1 : 0);
        if (r.hasTemp)         field(line, first, "temperature", r.tempC);
        if (r.hasHumidity)     field(line, first, "humidity", r.humidity);
        if (r.hasWind)       { field(line, first, "wind_speed", r.windAvgMs);
                               field(line, first, "wind_gust", r.windGustMs);
                               field(line, first, "wind_dir", r.windDirDeg); }
        if (r.hasRain)         field(line, first, "rain_total", r.rainTotalMm);
        if (r.hasUv)           field(line, first, "uv", r.uvIndex);
        if (r.hasLight)        field(line, first, "illuminance", r.lightLux);
        if (r.hasPressure)     field(line, first, "pressure_abs", r.pressureHpa);
        if (r.hasMoisture)     field(line, first, "soil_moisture", r.moisturePct);
        if (r.hasConductivity) field(line, first, "soil_conductivity", r.conductivityUsCm);
        if (r.hasLeafWetness)  field(line, first, "leaf_wetness", r.leafWetnessPct);
        if (r.hasDistance)     field(line, first, "distance_mm", r.distanceMm);
        if (r.hasHeatCount)    field(line, first, "heat_count", r.heatCount);
        if (r.hasLightning)  { field(line, first, "lightning_count", r.lightningCount);
                               field(line, first, "lightning_distance_km", r.lightningKm); }
        if (r.hasPm25)         field(line, first, "pm25", r.pm25);
        if (r.hasPm10)         field(line, first, "pm10", r.pm10);
        if (r.hasCo2)          field(line, first, "co2", r.co2Ppm);
        if (r.hasLeak)         field(line, first, "leak", r.leak ? 1 : 0);
        if (r.batteryMv)       field(line, first, "battery_volts", r.batteryMv / 1000.0);
        out += line; out += ts;
    }
}
