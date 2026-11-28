#include "metrics.h"
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

// api_info.cpp (weak); the host test overrides it. 0 = free, 1 = min ever.
size_t hostOrDeviceFreeHeap(int what);

namespace {

// Prometheus sample value: integers without a decimal point, otherwise up to
// three decimals; NaN / Inf become "NaN" (a valid Prometheus value).
void num(std::string& out, double v)
{
    char b[32];
    if (std::isnan(v) || std::isinf(v)) { out += "NaN"; return; }
    if (v == (double)(long long)v && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.3f", v);
    out += b;
}

// Escape a label value (backslash, double quote, newline).
void esc(std::string& out, const char* s)
{
    for (; s && *s; s++) {
        const char c = *s;
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n')        { out += "\\n"; }
        else                         out += c;
    }
}

// A single global gauge (HELP + TYPE + one sample).
void gauge(std::string& out, const char* name, const char* help, double v)
{
    out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
    out += "# TYPE "; out += name; out += " gauge\n";
    out += name; out += ' '; num(out, v); out += '\n';
}

// {sensor="garten",radio_id="900181A2",model="WS90",name="Garten",channel="3"}
// for wireless sensor i. sensor = the custom ID of a ★ sensor (A5c D7: stays across
// a battery change), else the radio ID; name "" when none is set.
std::string rfLabels(int i)
{
    const RfSensorReading& r = gRfSensors[i];
    const RfLabel lb = rfLabelOf(r.id);   // copied under the logical sensor table's lock
    std::string l = "{sensor=\"";
    esc(l, lb.customId);   l += '"';
    l += ",radio_id=\""; l += lb.radio; l += '"';
    l += ",model=\""; esc(l, r.model ? r.model : ""); l += '"';
    l += ",name=\"";  esc(l, lb.displayName);                l += '"';
    if (r.channel) { l += ",channel=\""; l += std::to_string(r.channel); l += '"'; }
    l += '}';
    return l;
}

// One metric family across all tracked wireless sensors: HELP / TYPE once, then
// a sample for every used sensor for which have(i) is true (value val(i)). The
// family is omitted entirely when no sensor has it.
template <class Have, class Val>
void rfMetric(std::string& out, const char* name, const char* help, const char* type, Have have, Val val)
{
    bool header = false;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        if (!gRfSensors[i].used || !have(i)) continue;
        if (!header) {
            out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
            out += "# TYPE "; out += name; out += ' '; out += type; out += '\n';
            header = true;
        }
        out += name; out += rfLabels(i); out += ' '; num(out, val(i)); out += '\n';
    }
}

} // namespace

void buildMetrics(std::string& out, int wifiRssiDbm, bool wifiConnected)
{
    out.clear();
    out.reserve(4096);

    // ---- system ----
    gauge(out, "uwn_up", "1 while the node is serving metrics.", 1);
    gauge(out, "uwn_uptime_seconds", "Uptime in seconds.", (double)(esp_timer_get_time() / 1000000LL));
    gauge(out, "uwn_heap_free_bytes", "Free internal heap, bytes.", (double)hostOrDeviceFreeHeap(0));
    gauge(out, "uwn_heap_min_bytes", "Lowest free internal heap seen, bytes.", (double)hostOrDeviceFreeHeap(1));
    if (wifiConnected) gauge(out, "uwn_wifi_rssi_dbm", "Wi-Fi signal strength, dBm.", (double)wifiRssiDbm);

    // ---- selected weather values (the same source the dashboard / API use) ----
    if (gSrc.tempValid)                  gauge(out, "uwn_temperature_celsius", "Outdoor temperature (selected source), Celsius.", gSrc.tempC);
    if (gSrc.tempValid && gSrc.humValid) gauge(out, "uwn_humidity_percent", "Relative humidity (selected source), percent.", gSrc.humidity);
    if (gSrc.pressValid) {
        out += "# HELP uwn_pressure_hpa Barometric pressure (selected source), hPa.\n# TYPE uwn_pressure_hpa gauge\n";
        out += "uwn_pressure_hpa{type=\"absolute\"} "; num(out, gSrc.pressAbsHpa); out += '\n';
        out += "uwn_pressure_hpa{type=\"sealevel\"} "; num(out, gSrc.pressSeaHpa); out += '\n';
    }
    if (srcActive(SG_WIND) && windCycleValid) {
        gauge(out, "uwn_wind_speed_mps", "Wind speed (selected source), m/s.", windSpeedMs);
        gauge(out, "uwn_wind_gust_mps", "Wind gust (selected source), m/s.", windGustMs);
        gauge(out, "uwn_wind_direction_degrees", "Wind direction (selected source), degrees.", windDirection);
    }
    if (srcActive(SG_RAIN)) {
        gauge(out, "uwn_rain_rate_mm_per_hour", "Rain rate (selected source), mm/h.", rainRateMmH);
        gauge(out, "uwn_rain_total_mm", "Rain running total since the start (selected source; 0 after a rain reset), mm.", rainMMSinceStart);
    }
    if (gSrc.uvRadio)                              gauge(out, "uwn_uv_index", "UV index (selected source).", gSrc.uvIndexRadio);
    else if (gSrc.uvBoard && !std::isnan(gIo.uvIndex)) gauge(out, "uwn_uv_index", "UV index (selected source).", gIo.uvIndex);
    if (gSrc.lightValid)                           gauge(out, "uwn_illuminance_lux", "Illuminance (selected source), lux.", gSrc.lightLux);

    // ---- custom series (series.h): one sample per slot with a value, metric units ----
    bool seriesHeader = false;
    for (int i = 0; i < SERIES_MAX; i++) {
        float v;
        if (!seriesNow(i, v)) continue;
        SeriesDef d = seriesDefAt(i);
        if (!seriesHeader) {
            out += "# HELP uwn_series Custom series: the last cycle's value (metric units, label unit).\n# TYPE uwn_series gauge\n";
            seriesHeader = true;
        }
        out += "uwn_series{slot=\""; out += std::to_string(i + 1);
        out += "\",name=\""; esc(out, d.name);
        out += "\",field=\""; out += SERIES_FIELDS[d.field].key;
        out += "\",unit=\""; esc(out, SERIES_FIELDS[d.field].unit);
        out += "\"} "; num(out, v); out += '\n';
    }

    // ---- per wireless sensor (labels id, model, name, channel) ----
    if (!rfTableOn()) return;
    rfMetric(out, "uwn_rf_up", "1 while the wireless sensor is received, 0 when it is silent (no packet within its freshness window).", "gauge",
             [](int){ return true; }, [](int i){ return rfSensorFresh(gRfSensors[i]) ? 1.0 : 0.0; });
    rfMetric(out, "uwn_rf_age_seconds", "Seconds since the last packet of the wireless sensor.", "gauge",
             [](int){ return true; }, [](int i){ const RfSensorReading& r = gRfSensors[i];
                         unsigned long age = (millis() - r.lastMs) / 1000UL;   // after a millis() wrap: at least the window
                         if ((r.staleMask & RF_VAL_ANY) && age < rfFreshMs(r) / 1000UL) age = rfFreshMs(r) / 1000UL;
                         return (double)age; });
    rfMetric(out, "uwn_rf_rssi_dbm", "Last signal strength per wireless sensor, dBm.", "gauge",
             [](int){ return true; }, [](int i){ return (double)gRfSensors[i].rssi; });
    rfMetric(out, "uwn_rf_packets_total", "Packets received per wireless sensor since start.", "counter",
             [](int){ return true; }, [](int i){ return (double)gRfSensors[i].packets; });
    rfMetric(out, "uwn_rf_battery_ok", "1 if the wireless sensor battery is ok, 0 if low.", "gauge",
             [](int){ return true; }, [](int i){ return gRfSensors[i].batteryOk ? 1.0 : 0.0; });
    rfMetric(out, "uwn_rf_temperature_celsius", "Temperature per wireless sensor, Celsius.", "gauge",
             [](int i){ return gRfSensors[i].hasTemp && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].tempC; });
    rfMetric(out, "uwn_rf_humidity_percent", "Humidity per wireless sensor, percent.", "gauge",
             [](int i){ return gRfSensors[i].hasHumidity && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].humidity; });
    rfMetric(out, "uwn_rf_wind_speed_mps", "Wind speed per wireless sensor, m/s.", "gauge",
             [](int i){ return gRfSensors[i].hasWind && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].windAvgMs; });
    rfMetric(out, "uwn_rf_wind_gust_mps", "Wind gust per wireless sensor, m/s.", "gauge",
             [](int i){ return gRfSensors[i].hasWind && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].windGustMs; });
    rfMetric(out, "uwn_rf_wind_direction_degrees", "Wind direction per wireless sensor, degrees.", "gauge",
             [](int i){ return gRfSensors[i].hasWind && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].windDirDeg; });
    rfMetric(out, "uwn_rf_rain_total_mm", "Rain counter per wireless sensor, mm.", "gauge",
             [](int i){ return gRfSensors[i].hasRain && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].rainTotalMm; });
    rfMetric(out, "uwn_rf_uv_index", "UV index per wireless sensor.", "gauge",
             [](int i){ return gRfSensors[i].hasUv && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].uvIndex; });
    rfMetric(out, "uwn_rf_illuminance_lux", "Illuminance per wireless sensor, lux.", "gauge",
             [](int i){ return gRfSensors[i].hasLight && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].lightLux; });
    rfMetric(out, "uwn_rf_pressure_hpa", "Station pressure per wireless sensor, hPa.", "gauge",
             [](int i){ return gRfSensors[i].hasPressure && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].pressureHpa; });
    rfMetric(out, "uwn_rf_soil_moisture_percent", "Soil moisture per wireless sensor, percent.", "gauge",
             [](int i){ return gRfSensors[i].hasMoisture && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].moisturePct; });
    rfMetric(out, "uwn_rf_soil_conductivity_uscm", "Soil conductivity per wireless sensor, uS/cm.", "gauge",
             [](int i){ return gRfSensors[i].hasConductivity && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].conductivityUsCm; });
    rfMetric(out, "uwn_rf_leaf_wetness_percent", "Leaf wetness per wireless sensor, percent.", "gauge",
             [](int i){ return gRfSensors[i].hasLeafWetness && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].leafWetnessPct; });
    rfMetric(out, "uwn_rf_distance_meters", "Laser distance (LDS01 air gap) per wireless sensor, meters.", "gauge",
             [](int i){ return gRfSensors[i].hasDistance && rfSensorFresh(gRfSensors[i]); }, [](int i){ return gRfSensors[i].distanceMm / 1000.0; });
    rfMetric(out, "uwn_rf_heat_count", "Heater-on count per wireless sensor (LDS01, 8-bit, wraps).", "gauge",
             [](int i){ return gRfSensors[i].hasHeatCount && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].heatCount; });
    rfMetric(out, "uwn_rf_lightning_count", "Lightning strike counter per wireless sensor (8-bit, wraps).", "gauge",
             [](int i){ return gRfSensors[i].hasLightning && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].lightningCount; });
    rfMetric(out, "uwn_rf_lightning_distance_km", "Distance to the storm front per wireless sensor, km (0 = none).", "gauge",
             [](int i){ return gRfSensors[i].hasLightning && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].lightningKm; });
    rfMetric(out, "uwn_rf_pm25_ugm3", "PM2.5 per wireless sensor, ug/m3.", "gauge",
             [](int i){ return gRfSensors[i].hasPm25 && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].pm25; });
    rfMetric(out, "uwn_rf_pm10_ugm3", "PM10 per wireless sensor, ug/m3.", "gauge",
             [](int i){ return gRfSensors[i].hasPm10 && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].pm10; });
    rfMetric(out, "uwn_rf_co2_ppm", "CO2 per wireless sensor, ppm.", "gauge",
             [](int i){ return gRfSensors[i].hasCo2 && rfSensorFresh(gRfSensors[i]); }, [](int i){ return (double)gRfSensors[i].co2Ppm; });
    rfMetric(out, "uwn_rf_leak", "1 if the leak sensor detects water, else 0.", "gauge",
             [](int i){ return gRfSensors[i].hasLeak && rfSensorFresh(gRfSensors[i]); }, [](int i){ return gRfSensors[i].leak ? 1.0 : 0.0; });
    rfMetric(out, "uwn_rf_battery_volts", "Reported battery voltage per wireless sensor, volts (0 = not sent).", "gauge",
             [](int i){ return gRfSensors[i].batteryMv != 0; }, [](int i){ return gRfSensors[i].batteryMv / 1000.0; });
}
