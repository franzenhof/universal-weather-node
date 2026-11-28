// Prometheus /metrics exposition (src/metrics.cpp): format, labels, SI units,
// per-sensor families, and the rfEn / Wi-Fi gating.
#include "series.h"
#include "globals.h"
#include "sensors.h"
#include "sources.h"
#include "rf_sensors.h"
#include "board_io.h"
#include "metrics.h"
#include "lsensors.h"
#include <cstring>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <string>

// Override the weak heap accessor (api_info.cpp is not linked into this test).
size_t hostOrDeviceFreeHeap(int what) { return what == 1 ? 90000 : 123456; }

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
static int count(const std::string& s, const std::string& sub)
{
    int n = 0;
    for (size_t p = 0; (p = s.find(sub, p)) != std::string::npos; p += sub.size()) n++;
    return n;
}

int main()
{
    gCfg.rfEn = 1;

    // selected-source weather values
    gSrc.tempValid = true;  gSrc.tempC = 12.5f;
    gSrc.humValid = true;   gSrc.humidity = 61.0f;
    gSrc.pressValid = true; gSrc.pressAbsHpa = 1013.2f; gSrc.pressSeaHpa = 1019.0f;
    gSrc.lightValid = true; gSrc.lightLux = 54000.0f;
    gSrc.uvRadio = true;    gSrc.uvIndexRadio = 3.0f;

    // two wireless sensors: a WS90 (wind, no channel) and a WH31 (temp+hum, channel 7)
    RfSensorReading& a = gRfSensors[0];
    a.used = true; a.id = 0x900181A2; a.model = "WS90"; a.rssi = -72.0f; a.packets = 125; a.batteryOk = true;
    a.hasTemp = true; a.tempC = 4.2f;
    a.hasWind = true; a.windAvgMs = 3.5f; a.windGustMs = 7.1f; a.windDirDeg = 180.0f;

    RfSensorReading& b = gRfSensors[1];
    b.used = true; b.id = 0x300007DD; b.model = "WH31"; b.channel = 7; b.rssi = -74.0f; b.packets = 207; b.batteryOk = false;
    b.hasTemp = true; b.tempC = 21.3f;
    b.hasHumidity = true; b.humidity = 55.0f;

    // the WH31 is a ★ sensor (logical sensor): labelled by its key and name; the WS90 is received only
    gLogicalSensors = LogicalSensorTable();
    assert(lsAdd(gLogicalSensors, 0x300007DD, "Keller Nord", lsRandom) == 0 && lsSetCustomId(gLogicalSensors, 0, "keller"));

    // custom series (series.h): slot 3 with a value, slot 4 defined but without one
    SeriesDef* sd = seriesDefs();
    strcpy(sd[2].name, "Beet \"Nord\""); strcpy(sd[2].sensorInternalId, "L3F9A2C"); sd[2].field = SF_MOIST;
    strcpy(sd[3].name, "CO2"); strcpy(sd[3].sensorInternalId, "L3F9A2C"); sd[3].field = SF_CO2;
    float sv[SERIES_MAX] = {}; bool sok[SERIES_MAX] = {};
    sv[2] = 41.0f; sok[2] = true;
    seriesSetNow(sv, sok);
    std::string out;
    buildMetrics(out, -55, true);
    assert(has(out, "# TYPE uwn_series gauge\nuwn_series{slot=\"3\",name=\"Beet \\\"Nord\\\"\",field=\"moist\",unit=\"%\"} 41\n"));
    assert(!has(out, "slot=\"4\""));
    assert(!has(out, gLogicalSensors.s[0].internalId) && !has(out, "{id="));   // never the internal ID; the old id label is gone

    // rain (board gauge): the running total since the start, never the last cycle's amount
    const uint8_t rainSel = gCfg.srcSel[SG_RAIN]; const auto rainEn = gCfg.rainEn;
    gCfg.srcSel[SG_RAIN] = SRC_BOARD; gCfg.rainEn = 1;
    rainMM = 0.4f; rainMMSinceStart = 123.4f; rainRateMmH = 1.2f;
    buildMetrics(out, -55, true);
    assert(has(out, "uwn_rain_total_mm 123.4") && !has(out, "uwn_rain_total_mm 0.4"));
    gCfg.srcSel[SG_RAIN] = rainSel; gCfg.rainEn = rainEn;
    buildMetrics(out, -55, true);

    // system
    assert(has(out, "# TYPE uwn_up gauge\nuwn_up 1\n"));
    assert(has(out, "uwn_uptime_seconds "));
    assert(has(out, "uwn_heap_free_bytes 123456\n"));
    assert(has(out, "uwn_heap_min_bytes 90000\n"));
    assert(has(out, "uwn_wifi_rssi_dbm -55\n"));

    // selected source, SI units, integer vs decimal formatting
    assert(has(out, "uwn_temperature_celsius 12.500\n"));
    assert(has(out, "uwn_humidity_percent 61\n"));           // whole number: no decimals
    assert(has(out, "uwn_pressure_hpa{type=\"absolute\"} 1013.200\n"));
    assert(has(out, "uwn_pressure_hpa{type=\"sealevel\"} 1019\n"));
    assert(has(out, "uwn_uv_index 3\n"));
    assert(has(out, "uwn_illuminance_lux 54000\n"));

    // per-sensor families, labels and types
    assert(has(out, "# TYPE uwn_rf_packets_total counter\n"));
    assert(has(out, "uwn_rf_up{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 1\n"));
    assert(has(out, "uwn_rf_temperature_celsius{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 4.200\n"));
    assert(has(out, "uwn_rf_packets_total{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 125\n"));
    assert(has(out, "uwn_rf_wind_speed_mps{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 3.500\n"));
    assert(has(out, "uwn_rf_battery_ok{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 1\n"));
    // the WH31 carries its channel as a label and is "battery low"
    assert(has(out, "uwn_rf_humidity_percent{sensor=\"keller\",radio_id=\"300007DD\",model=\"WH31\",name=\"Keller Nord\",channel=\"7\"} 55\n"));
    assert(has(out, "uwn_rf_battery_ok{sensor=\"keller\",radio_id=\"300007DD\",model=\"WH31\",name=\"Keller Nord\",channel=\"7\"} 0\n"));
    // collision case: a ★ sensor whose custom ID equals the radio ID of the received-only WS90 - separate series
    assert(lsSetCustomId(gLogicalSensors, 0, "900181A2"));
    buildMetrics(out, -55, true);
    assert(has(out, "uwn_rf_up{sensor=\"900181A2\",radio_id=\"300007DD\"") && has(out, "uwn_rf_up{sensor=\"rf900181A2\",radio_id=\"900181A2\""));
    assert(lsSetCustomId(gLogicalSensors, 0, "keller"));
    buildMetrics(out, -55, true);

    // each metric family has exactly one HELP/TYPE header even with two sensors
    assert(count(out, "# TYPE uwn_rf_temperature_celsius ") == 1);
    assert(count(out, "uwn_rf_temperature_celsius{") == 2);   // both sensors have temperature

    // families no sensor has are omitted entirely
    assert(!has(out, "uwn_rf_co2_ppm"));
    assert(!has(out, "uwn_rf_soil_moisture_percent"));
    assert(!has(out, "uwn_rf_humidity_percent{sensor=\"rf900181A2\""));   // the WS90 sent no humidity

    // a silent sensor: up 0, its age, no measured values (they would look current); RSSI / packets stay
    hostNow = 10UL * 60UL * 1000UL;
    gRfSensors[0].lastMs = 0;                       // WS90: last packet 10 min ago
    gRfSensors[1].lastMs = hostNow - 5000;          // WH31: 5 s ago
    buildMetrics(out, -55, true);
    assert(has(out, "uwn_rf_up{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 0\n"));
    assert(has(out, "uwn_rf_age_seconds{sensor=\"rf900181A2\",radio_id=\"900181A2\",model=\"WS90\",name=\"\"} 600\n"));
    assert(!has(out, "uwn_rf_temperature_celsius{sensor=\"rf900181A2\""));
    assert(!has(out, "uwn_rf_wind_speed_mps"));
    assert(has(out, "uwn_rf_packets_total{sensor=\"rf900181A2\""));
    assert(has(out, "uwn_rf_up{sensor=\"keller\",radio_id=\"300007DD\",model=\"WH31\",name=\"Keller Nord\",channel=\"7\"} 1\n"));
    assert(has(out, "uwn_rf_temperature_celsius{sensor=\"keller\""));
    hostNow = 0; gRfSensors[0].lastMs = gRfSensors[1].lastMs = 0;

    // Wi-Fi down -> no RSSI line; rfEn off -> no per-sensor families
    gCfg.rfEn = 0;
    buildMetrics(out, 0, false);
    assert(!has(out, "uwn_wifi_rssi_dbm"));
    assert(!has(out, "uwn_rf_up"));
    assert(has(out, "uwn_temperature_celsius 12.500\n"));   // non-RF values still there

    printf("Firmware metrics regressions passed.\n");
    return 0;
}
