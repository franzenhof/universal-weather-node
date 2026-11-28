// InfluxDB line-protocol builder (src/influx.cpp): fields, tags, escaping,
// nanosecond timestamp and the rfEn gating.
#include "series.h"
#include "globals.h"
#include "config.h"
#include "sensors.h"
#include "sources.h"
#include "rf_sensors.h"
#include "board_io.h"
#include "influx.h"
#include "lsensors.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

size_t hostOrDeviceFreeHeap(int what) { return what == 1 ? 90000 : 123456; }

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

int main()
{
    gCfg.rfEn = 1;
    strcpy(gCfg.deviceName, "node1");
    strcpy(gCfg.influxMeasurement, "weather");

    gSrc.tempValid = true;  gSrc.tempC = 12.5f;
    gSrc.humValid = true;   gSrc.humidity = 61.0f;
    gSrc.pressValid = true; gSrc.pressAbsHpa = 1013.2f; gSrc.pressSeaHpa = 1019.0f;
    gSrc.lightValid = true; gSrc.lightLux = 54000.0f;
    gSrc.uvRadio = true;    gSrc.uvIndexRadio = 3.0f;

    RfSensorReading& a = gRfSensors[0];
    a.used = true; a.id = 0x900181A2; a.model = "WS90"; a.rssi = -72.0f; a.packets = 125; a.batteryOk = true;
    a.hasTemp = true; a.tempC = 4.2f;
    a.hasWind = true; a.windAvgMs = 3.5f; a.windGustMs = 7.1f; a.windDirDeg = 180.0f;

    RfSensorReading& b = gRfSensors[1];
    b.used = true; b.id = 0x300007DD; b.model = "WH31"; b.channel = 7; b.rssi = -74.0f; b.packets = 207; b.batteryOk = false;
    b.hasTemp = true; b.tempC = 21.3f;
    b.hasHumidity = true; b.humidity = 55.0f;

    // the WH31 is a ★ sensor (logical sensor): tagged by its key; the WS90 is received only
    gLogicalSensors = LogicalSensorTable();
    assert(lsAdd(gLogicalSensors, 0x300007DD, nullptr, lsRandom) == 0 && lsSetCustomId(gLogicalSensors, 0, "keller"));

    // custom series (series.h): a field series_<slot> on the main line, only with a value
    SeriesDef* sd = seriesDefs();
    strcpy(sd[2].name, "Beet"); strcpy(sd[2].sensorInternalId, "L3F9A2C"); sd[2].field = SF_MOIST;
    strcpy(sd[3].name, "CO2"); strcpy(sd[3].sensorInternalId, "L3F9A2C"); sd[3].field = SF_CO2;
    float sv[SERIES_MAX] = {}; bool sok[SERIES_MAX] = {};
    sv[2] = 41.0f; sok[2] = true;
    seriesSetNow(sv, sok);
    std::string out;
    buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
    assert(has(out, "illuminance=54000,series_3=41,uptime=") && !has(out, "series_4"));

    // main line: measurement + host tag, selected weather + system fields, ns timestamp
    assert(has(out, "weather,host=node1 temperature=12.500,humidity=61,"));
    assert(has(out, "pressure_abs=1013.200,pressure_sea=1019,"));
    assert(has(out, "uv=3,illuminance=54000,"));
    assert(has(out, ",heap_free=123456,rssi=-55 1700000000000000000\n"));

    // per-sensor lines: tags sensor/radio_id/host/model(/name)(/channel), fields, same ns timestamp
    assert(has(out, "weather_rf,sensor=rf900181A2,radio_id=900181A2,host=node1,model=WS90 rssi=-72,packets=125,battery_ok=1,"
                    "temperature=4.200,wind_speed=3.500,wind_gust=7.100,wind_dir=180 1700000000000000000\n"));
    assert(has(out, "weather_rf,sensor=keller,radio_id=300007DD,host=node1,model=WH31,channel=7 rssi=-74,packets=207,battery_ok=0,"
                    "temperature=21.300,humidity=55 1700000000000000000\n"));
    assert(!has(out, gLogicalSensors.s[0].internalId) && !has(out, "name="));   // never the internal ID; no name set: no tag
    // a name (escaped as a tag); after a battery change (re-bind) the series stays sensor=keller
    snprintf(gLogicalSensors.s[0].displayName, sizeof gLogicalSensors.s[0].displayName, "Keller Nord");
    assert(lsRebindRadioId(gLogicalSensors, 0, 0x300007DE));
    b.id = 0x300007DE;
    buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
    assert(has(out, "weather_rf,sensor=keller,radio_id=300007DE,host=node1,model=WH31,name=Keller\\ Nord,channel=7 "));
    // collision case: a ★ sensor whose custom ID equals the radio ID of the received-only WS90 - the two series stay apart
    assert(lsSetCustomId(gLogicalSensors, 0, "900181A2"));
    buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
    assert(has(out, "weather_rf,sensor=900181A2,radio_id=300007DE,") && has(out, "weather_rf,sensor=rf900181A2,radio_id=900181A2,"));
    assert(lsSetCustomId(gLogicalSensors, 0, "keller"));
    // a received-only sensor's name (rfNames) is tagged too
    snprintf(gCfg.rfNames, sizeof gCfg.rfNames, "900181A2:Dach");
    buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
    assert(has(out, "weather_rf,sensor=rf900181A2,radio_id=900181A2,host=node1,model=WS90,name=Dach "));
    gCfg.rfNames[0] = '\0';

    // tag escaping (space, comma)
    strcpy(gCfg.deviceName, "a b,c");
    buildInfluxLineProtocol(out, 1700000000LL, 0, false, gCfg.influxMeasurement);
    assert(has(out, "weather,host=a\\ b\\,c "));
    assert(has(out, ",heap_free=123456 1700000000000000000\n"));   // Wi-Fi down: main line ends at heap_free, no rssi

    // rfEn off -> no per-sensor lines; main line still present
    gCfg.rfEn = 0;
    buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
    assert(!has(out, "weather_rf"));
    assert(has(out, "weather,host=a\\ b\\,c temperature=12.500"));

    // backslash and control characters in a tag, measurement escaping, NaN / Inf left out
    gCfg.rfEn = 1;
    strcpy(gCfg.deviceName, "x\\y\\");
    strcpy(gCfg.influxMeasurement, "my meas,1");
    gSrc.tempC = NAN;
    a.tempC = INFINITY;
    buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
    assert(has(out, "my\\ meas\\,1,host=x\\\\y\\\\ "));          // my\ meas\,1,host=x\\y\\  (escaped)
    assert(!has(out, "temperature=0") && !has(out, "=nan") && !has(out, "=inf") && !has(out, "=-inf"));
    assert(has(out, "my\\ meas\\,1_rf,sensor=rf900181A2"));
    assert(!has(out, " temperature=") || has(out, "temperature=21.300"));   // only the WH31 still has one
    gCfg.influxMeasurement[0] = '\0';

    // a silent sensor (no packet within its freshness window): no line at all
    {
        RfSensorReading& c = gRfSensors[2];
        c.used = true; c.id = 0x17000008; c.rssi = -80.0f; c.packets = 9; c.hasTemp = true; c.tempC = 10.0f;
        hostNow = 10UL * 60UL * 1000UL; c.lastMs = 0;                       // 10 min ago
        a.lastMs = b.lastMs = hostNow - 5000;                               // the others fresh
        buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
        assert(!has(out, "sensor=rf17000008"));
        assert(has(out, "sensor=rf900181A2") && has(out, "sensor=keller"));
        c.lastMs = hostNow - 5000;                                          // it sends again
        buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
        assert(has(out, "sensor=rf17000008"));
        c = RfSensorReading(); hostNow = 0; a.lastMs = b.lastMs = 0;
    }

    // rain (board gauge): rain_total is the running total since the start (as on the _rf lines), not the cycle's amount
    {
        const uint8_t rainSel = gCfg.srcSel[SG_RAIN]; const auto rainEn = gCfg.rainEn;
        gCfg.srcSel[SG_RAIN] = SRC_BOARD; gCfg.rainEn = 1;
        rainMM = 0.4f; rainMMSinceStart = 123.4f; rainRateMmH = 1.2f;
        buildInfluxLineProtocol(out, 1700000000LL, -55, true, gCfg.influxMeasurement);
        assert(has(out, "rain_total=123.4") && !has(out, "rain_total=0.4"));
        gCfg.srcSel[SG_RAIN] = rainSel; gCfg.rainEn = rainEn;
    }

    // Authorization header: a bare token gets "Token ", a scheme is kept, empty stays empty
    assert(influxAuthHeader("test123") == "Token test123");
    assert(influxAuthHeader(" test123 ") == "Token test123");
    assert(influxAuthHeader("Token abc") == "Token abc");
    assert(influxAuthHeader("Bearer abc") == "Bearer abc");
    assert(influxAuthHeader("Basic dXNlcjpwdw==") == "Basic dXNlcjpwdw==");
    assert(influxAuthHeader("") == "" && influxAuthHeader(nullptr) == "");

    extern int hostLsLockDepth;
    assert(hostLsLockDepth == 0);   // every locked section was left
    printf("Firmware influx regressions passed.\n");
    return 0;
}
