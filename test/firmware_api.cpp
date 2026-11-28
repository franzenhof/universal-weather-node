// API contract fixtures (test/api_contract.test.js): drives the real
// serializers - buildJsonPayload() (/api/data = MQTT data topic),
// buildHistoryJson() (/api/history) and buildInfoJson() (/api/info) -
// through a deterministic simulated run per fixture and prints the four
// documents, one per line ("<name> <json>"). Usage: firmware_api <fixture>
//
// history.cpp is compiled into this file (like firmware_history.cpp), so the
// trends and rolling rain sums come from the real ring buffer; runtime.cpp's
// "no history" / "no update" stubs are renamed away.
#define historyPressureAgo hostStubPressureAgo
#define historyTempAgo     hostStubTempAgo
#define historyRainSum     hostStubRainSum
#define wxUpdateAvailable  hostStubUpdateAvailable
#include "host/runtime.cpp"
#include "ls_host.h"
#undef historyPressureAgo
#undef historyTempAgo
#undef historyRainSum
#undef wxUpdateAvailable

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#ifndef portMAX_DELAY
#define portMAX_DELAY 0
#endif
#define xSemaphoreTake(m, t) ((void)(m), (void)(t), true)
#define xSemaphoreGive(m)    ((void)(m), true)
#include "../src/history.cpp"

#include "api_info.h"
#include "time_sync.h"
#include "records.h"
#include "out_status.h"
#include "astro.h"
#include "alarms.h"
#include "rf_stats.h"
#include "json_payload.h"
#include "board_io.h"
#include "rf_sensors.h"
#include "davis_diag.h"
#include "sources.h"
#include "sensors.h"
#include "influx_queue.h"
#include "series.h"
#include "vs_store.h"
#include "reset_history.h"
#include <DallasTemperature.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include "gnss.h"

// Reset history (reset_history.h): the device part is not built here
ResetHistory hostResetHistory;
const ResetHistory& resetHistory() { return hostResetHistory; }


// InfluxDB backfill queue (wx_upload.cpp on the device): a backlog after an outage
void influxBacklogStatus(InfluxBacklogStatus& out)
{
    out = InfluxBacklogStatus();
    out.active = true; out.pushes = 42; out.bytes = 39000; out.capacity = INFLUX_BACKLOG_BYTES;
    out.oldestEpoch = 0; out.droppedFull = 3; out.droppedRejected = 1;
}

// ---- host stubs for modules not built here ----
static bool hostClockSynced = true;
bool timeIsValid() { return hostClockSynced; }
// The fixtures: synced = NTP, 20 min since its last answer; unsynced = no source
void timeSyncStatus(TimeSyncStatus& out)
{
    out = TimeSyncStatus();
    if (!hostClockSynced) return;
    out.source = TSRC_NTP;
    out.fresh = true;
    out.ageSec = 1200;
}
// Wall clock for history.cpp's bucket alignment: set by run() so that the
// run ends at NOW_EPOCH (see there).
static int64_t hostEpochAtZero = 0;
bool epochNow(int64_t& out)
{
    if (!hostClockSynced) return false;
    out = hostEpochAtZero + (int64_t)(millis() / 1000);
    return true;
}
// rf_stats.h clock: the simulated wall clock
bool rfStatsClock(uint32_t& t, uint32_t& ymd)
{
    int64_t e;
    if (!epochNow(e)) { t = (uint32_t)(millis() / 1000); ymd = 0; return false; }
    time_t tt = (time_t)e;
    struct tm lt;
    localtime_r(&tt, &lt);
    t = (uint32_t)e;
    ymd = (uint32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday);
    return true;
}

bool localTimeNow(struct tm& out)
{
    int64_t e;
    if (!epochNow(e)) return false;
    time_t t = (time_t)e;
    localtime_r(&t, &out);
    return true;
}
void wxUploadLock() {}     // astroLocation() (astro_store.cpp): one task here
void wxUploadUnlock() {}
static const char* hostUpdate = "";
void wxUpdateAvailable(char* out, size_t len) { if (len) snprintf(out, len, "%s", hostUpdate); }

namespace {

constexpr float PI_F = 3.14159265f;
uint32_t lcg = 12345;
float noise()   // deterministic, -1 .. 1
{
    lcg = lcg * 1103515245u + 12345u;
    return ((lcg >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}
float round1(float v) { return roundf(v * 10.0f) / 10.0f; }

// Simulated weather at hour h of the run (base units). The run ends at
// local 14:00, so h = hours - 1 .. 0 back from "now".
struct Wx { float tC, rh, qnh, windMs, gustMs, rainMm; int dir; float uv, lux; };
Wx weather(float hoursAgo, float rainLastHours)
{
    float hod = fmodf(14.0f - hoursAgo + 48.0f, 24.0f);           // local hour of day
    float day = sinf((hod - 9.0f) / 24.0f * 2.0f * PI_F);          // max at 15:00
    Wx w;
    w.tC     = 12.0f + 6.5f * day + 0.15f * noise();
    w.rh     = fminf(98.0f, 70.0f - 22.0f * day + 1.5f * noise());
    w.qnh    = 1013.0f + 0.35f * hoursAgo / 3.0f - 0.0002f * hoursAgo * hoursAgo + 0.05f * noise();   // slowly falling
    w.windMs = fmaxf(0.0f, 2.8f + 1.6f * day + 0.6f * noise());
    w.gustMs = w.windMs * 1.7f + 0.8f + 0.4f * noise();
    w.dir    = ((int)lroundf(235.0f + 35.0f * noise() + 20.0f * day) + 360) % 360;
    bool rains = (hoursAgo >= 5.0f && hoursAgo < 7.5f) || hoursAgo < rainLastHours;
    w.rainMm = rains ? 0.2f * (1 + (int)((noise() + 1.0f) * 1.6f)) : 0.0f;   // 0.2 .. 0.8 mm per cycle
    w.uv     = fmaxf(0.0f, round1(4.2f * day));
    w.lux    = fmaxf(0.0f, 52000.0f * day + 3000.0f * noise());
    return w;
}

RfSensorReading& rfAdd(int slot, uint32_t id, uint8_t type, uint8_t proto, const char* model = nullptr)
{
    RfSensorReading& r = gRfSensors[slot];
    r = RfSensorReading{};
    r.used = true; r.id = id; r.type = type; r.proto = proto; r.model = model;
    r.rssi = -74.0f; r.batteryOk = true;
    return r;
}

void rfHeard(RfSensorReading& r, unsigned long agoMs)
{
    unsigned long t = millis() - agoMs;
    r.lastMs = r.tempMs = r.humMs = r.windMs = r.rainMs = r.uvMs = r.lightMs = r.pressMs = t;
}

constexpr unsigned long CYCLE_MS = 300000;
constexpr int64_t NOW_EPOCH = 1781524800;   // end of every run: 2026-06-15T12:00:00Z (14:00 CEST)

// Rain bookkeeping of the run for the daily statistics (wx_upload.cpp is
// not built here): today = the last 14 h (since local midnight), yesterday
// = the 24 h before.
struct RunRain { float todayMm = 0, yesterdayMm = 0; int64_t lastRainEpoch = 0; };
// records.h, fed like records_store.cpp's recordsCycle() does on the node
Records hostRecords;
float   hostDayRainMm = 0.0f;
RunRain runRain;

// Runs `hours` of 5-min cycles: set(w) puts the cycle's values into the
// globals, then the cycle completes like loop() does (srcUpdateCycle,
// pushHistorySample).
void run(float hours, float rainLastHours, const std::function<void(const Wx&)>& set)
{
    int cycles = (int)lroundf(hours * 3600000.0f / CYCLE_MS);
    setenv("TZ", gCfg.timeZone, 1);   // local day for the records
    tzset();
    // the response follows 63 s after the last cycle, at NOW_EPOCH
    hostEpochAtZero = NOW_EPOCH - 63 - (int64_t)((millis() + (unsigned long)cycles * CYCLE_MS) / 1000);
    for (int i = cycles - 1; i >= 0; i--) {
        delay(CYCLE_MS);
        float hoursAgo = i * CYCLE_MS / 3600000.0f;
        Wx w = weather(hoursAgo, rainLastHours);
        set(w);
        if (hoursAgo < 14.0f)      runRain.todayMm += w.rainMm;
        else if (hoursAgo < 38.0f) runRain.yesterdayMm += w.rainMm;
        if (w.rainMm > 0.0f) runRain.lastRainEpoch = NOW_EPOCH - (int64_t)i * (CYCLE_MS / 1000) - 63;
        windCycleValid = true;
        rainRateMmH = w.rainMm * 3600000.0f / CYCLE_MS;
        rainMM = w.rainMm;
        srcUpdateCycle();
        pushHistorySample(seriesNowValues(), seriesNowOk());   // custom series: set(w) gives their values (seriesSetNow)
        int64_t e;
        if (epochNow(e)) {
            time_t tt = (time_t)e;
            struct tm lt;
            localtime_r(&tt, &lt);
            if (recordsRoll(hostRecords, lt)) hostDayRainMm = 0.0f;
            const uint32_t t = (uint32_t)e;
            if (gSrc.tempValid)  recordsUpdate(hostRecords, RQ_TEMP, gSrc.tempC, t);
            if (gSrc.humValid)   recordsUpdate(hostRecords, RQ_HUM, gSrc.humidity, t);
            if (gSrc.pressValid) recordsUpdate(hostRecords, RQ_PRESS, gSrc.pressSeaHpa, t);
            if (srcActive(SG_WIND)) recordsUpdate(hostRecords, RQ_GUST, windGustMs, t);
            if (srcActive(SG_RAIN)) {
                recordsUpdate(hostRecords, RQ_RAINRATE, rainRateMmH, t);
                hostDayRainMm += w.rainMm;
                recordsDailyRain(hostRecords, hostDayRainMm);
            }
        }
    }
    gBaseCycleMs = CYCLE_MS;
    astroCycle();
    // send status (out_status.h) of the last cycle; shown for the enabled outputs only
    for (uint8_t i = 0; i < OUT_COUNT; i++) outReport(i, true);
    outReport(OUT_WX_FIRST + 1, false, 401, "Unauthorized");   // Weather Underground: wrong key
    delay(63000);                 // 63 s into the next cycle: nextCycleInSec = 237
    lastSendMs = millis() - 63000;
}

void boardWind(const Wx& w) { windSpeedMs = w.windMs; windGustMs = w.gustMs; windDirection = (uint16_t)w.dir; }
void boardAir(const Wx& w)
{
    bme280TempC = w.tC; bme280Humidity = w.rh; bme280PressSea = w.qnh;
    bme280PressAbs = w.qnh - 66.2f;   // 560 m
}
void radioWeather(RfSensorReading& r, const Wx& w, bool uvLight)
{
    r.hasTemp = r.hasHumidity = r.hasWind = r.hasRain = true;
    r.tempC = round1(w.tC); r.humidity = (uint8_t)lroundf(w.rh);
    r.windAvgMs = round1(w.windMs); r.windGustMs = round1(w.gustMs); r.windDirDeg = (float)w.dir;
    r.rainTotalMm += w.rainMm;
    {   // per-sensor rain today / last hour (rf_stats.h), fed like rfStoreValues() does
        uint32_t t, ymd;
        rfStatsClock(t, ymd);
        if (RfStat* st = rfStatFor(r.id, true)) rfStatRain(*st, w.rainMm, t, ymd);
    }
    if (uvLight) { r.hasUv = r.hasLight = true; r.uvIndex = w.uv; r.lightLux = roundf(w.lux); }
    rfHeard(r, 9000);
    rainMMSinceStart += w.rainMm;
}

// Clock synced: daily values from the run; week/month/year = the run's two
// days plus the given earlier rain of the period.
void syncedDay(float earlierWeekMm, float earlierMonthMm, float earlierYearMm, float sunTodayMin, float sunYesterdayMin)
{
    hostClockSynced = true;
    hostDailyStats.valid = true;
    hostDailyStats.rainTodayMm = round1(runRain.todayMm);
    hostDailyStats.yesterdayValid = true;
    hostDailyStats.rainYesterdayMm = round1(runRain.yesterdayMm);
    float twoDays = hostDailyStats.rainTodayMm + hostDailyStats.rainYesterdayMm;
    hostDailyStats.rainWeekMm = twoDays + earlierWeekMm;
    hostDailyStats.rainMonthMm = twoDays + earlierMonthMm;
    hostDailyStats.rainYearMm = twoDays + earlierYearMm;
    hostDailyStats.sunTodaySec = sunTodayMin * 60.0f; hostDailyStats.sunYesterdaySec = sunYesterdayMin * 60.0f;
    hostDailyStats.lastRainEpoch = runRain.lastRainEpoch;
}

void common(const char* name)
{
    gCfg = Config{};   // a new node's defaults (wireless sensors on, Wi-Fi + web API on)
    vsSettingsWrite(VsSettings{});   // virtual sensors off (own store, vs_store.h)
    hostResetHistory = ResetHistory();
    gBoard = BoardInfo{};   // Heltec V4 without PCB unless a fixture sets gBoard.pcb
    rfStatsClear();
    snprintf(gCfg.deviceName, sizeof(gCfg.deviceName), "%s", name);
    gResetReason = "power-on";
    sendFailCount = 0;
    cpuTemperature = 41.5f;
    hostNow = 1000;
    snprintf(gCfg.latitude, sizeof(gCfg.latitude), "47.80");    // sun / moon (astro.h)
    snprintf(gCfg.longitude, sizeof(gCfg.longitude), "13.04");
}

// ---- fixtures ----

// Minimal wireless build: Heltec V3, BME280 on four wires, Bresser 7-in-1.
void fixtureBresser()
{
    common("Garden");
    gCfg.rfProto = RF_PROTO_BRESSER;
    gCfg.mqttEn = 1;
    snprintf(gCfg.awekasUser, sizeof(gCfg.awekasUser), "secret-user");
    snprintf(gCfg.wuStationId, sizeof(gCfg.wuStationId), "ISECRET1");
    bme280Present = true;
    RfSensorReading& r = rfAdd(0, 0x39582376, 1, RF_PROTO_BRESSER);
    run(50, 0.75f, [&](const Wx& w) { boardAir(w); radioWeather(r, w, true); boardWind(w); });
    r.rssi = -71.0f;
    srcUpdateCycle();
    syncedDay(0.0f, 24.6f, 498.2f, 205, 312);
}

// Ecowitt / Froggit with an ID list: WS90 array, WH31 (not heard for 7 min), WN32P
// (pressure source), WH51, WH57, WN34
// temperature probe and a second WH31 (wireless 5 and 6), WH46, WH55, WN32P,
// WH52, WN35 and WH41 (wireless 7-12); no BME280.
void fixtureEcowitt()
{
    common("Ecowitt Hub");
    gCfg.rfProto = RF_PROTO_FINEOFFSET;
    gCfg.windUnitMs = 0;   // km/h
    gCfg.loxEn = 1;
    gCfg.influxEn = 1;   // InfluxDB push with a backlog (status + backlog object, no URL / token in the API)
    snprintf(gCfg.influxUrl, sizeof(gCfg.influxUrl), "http://secret-host:8086/write?db=secret");
    snprintf(gCfg.influxToken, sizeof(gCfg.influxToken), "secret-token");
    gCfg.altitudeM = 560;
    // ID list: the order numbers the sensors (wireless 1-12); the WN32P indoor
    // sensor is wireless 3 - the pressure source (no BME280), but never the
    // temperature source (Auto skips indoor sensors for temperature / humidity)
    hostSetMine("900C3A7F, 300002B2, 25000050, 5700E21A, 5100D0C7, 3400A1B2, 300003C4, "
             "460027F1, 5530CF55, A2005D8D, 3500305E, 410000C7");
    bme280Present = false;
    RfSensorReading& ws = rfAdd(0, 0x900C3A7F, 1, RF_PROTO_FINEOFFSET, "WS90");
    ws.hasSupercap = true;
    RfSensorReading& wh31 = rfAdd(1, 0x300002B2, 2, RF_PROTO_FINEOFFSET, "WH31");
    // names of ★ sensors live in their logical sensor entries (lsensors.h)
    snprintf(gLogicalSensors.s[lsFindByRadioId(gLogicalSensors, 0x300002B2)].displayName, LS_DISPLAY_NAME_MAX + 1, "Greenhouse");
    snprintf(gLogicalSensors.s[lsFindByRadioId(gLogicalSensors, 0x5100D0C7)].displayName, LS_DISPLAY_NAME_MAX + 1, "Raised bed");
    // keys (what MQTT, InfluxDB, /metrics, Loxone and the REST API call them); the others keep the proposed radio ID
    lsSetCustomId(gLogicalSensors, lsFindByRadioId(gLogicalSensors, 0x300002B2), "greenhouse");
    lsSetCustomId(gLogicalSensors, lsFindByRadioId(gLogicalSensors, 0x5100D0C7), "raised_bed");
    wh31.channel = 2; wh31.hasTemp = wh31.hasHumidity = true;
    RfSensorReading& wh51 = rfAdd(2, 0x5100D0C7, 4, RF_PROTO_FINEOFFSET, "WH51");
    wh51.hasMoisture = true;
    RfSensorReading& wh57 = rfAdd(3, 0x5700E21A, 9, RF_PROTO_FINEOFFSET, "WH57");
    wh57.hasLightning = true;
    // sensors 5 and 6 (up to 32 are received): shown everywhere, not value sources
    RfSensorReading& wn34 = rfAdd(4, 0x3400A1B2, 3, RF_PROTO_FINEOFFSET, "WN34");
    wn34.hasTemp = true;
    RfSensorReading& wh31b = rfAdd(5, 0x300003C4, 2, RF_PROTO_FINEOFFSET, "WH31");
    wh31b.channel = 3; wh31b.hasTemp = wh31b.hasHumidity = true;
    RfSensorReading& wn32p = rfAdd(8, 0x25000050, 2, RF_PROTO_FINEOFFSET, "WN32P");
    wn32p.hasTemp = wn32p.hasHumidity = wn32p.hasPressure = true;
    wn32p.tempC = 22.6f; wn32p.humidity = 40;
    {   // custom series (series.h): soil moisture, lightning strikes and the storm distance (own aggregation: max)
        SeriesDef* d = seriesDefs();
        auto def = [&](int slot, const char* name, uint32_t radioId, uint8_t field) {
            snprintf(d[slot].name, sizeof(d[slot].name), "%s", name);
            snprintf(d[slot].sensorInternalId, sizeof(d[slot].sensorInternalId), "%s",
                     gLogicalSensors.s[lsFindByRadioId(gLogicalSensors, radioId)].internalId);
            d[slot].field = field;
        };
        def(0, "Raised bed", 0x5100D0C7, SF_MOIST);
        def(1, "Lightning", 0x5700E21A, SF_STRIKES);
        def(3, "Storm distance", 0x5700E21A, SF_DIST);
        seriesSetAgg(d[3], SA_MAX);
        historySeriesConfigure(d, SERIES_MAX);
    }
    run(30, 0.5f, [&](const Wx& w) {
        wn32p.pressureHpa = roundf((w.qnh - 66.2f) * 10.0f) / 10.0f; rfHeard(wn32p, 30000);   // station pressure at 560 m
        radioWeather(ws, w, true); boardWind(w);
        ws.supercapV = 3.86f;
        wh31.tempC = round1(w.tC + 8.5f); wh31.humidity = 48; rfHeard(wh31, 20000);
        wh51.moisturePct = 41; rfHeard(wh51, 40000);
        // the WH57's counter: a strike now and then in the last hours (wraps at 255)
        if (w.rainMm > 0.0f) wh57.lightningCount = (uint8_t)(wh57.lightningCount + 3);
        wh57.lightningKm = 8; rfHeard(wh57, 30000);
        {
            float sv[SERIES_MAX] = {};
            bool sok[SERIES_MAX] = {};
            sv[0] = 41; sok[0] = true;
            sv[1] = w.rainMm > 0.0f ? 3 : 0; sok[1] = true;
            sv[3] = w.rainMm > 0.0f ? 8 : 0; sok[3] = w.rainMm > 0.0f;   // a distance only while it storms
            seriesSetNow(sv, sok);
        }
        {
            uint32_t t, ymd;
            rfStatsClock(t, ymd);
            if (RfStat* st = rfStatFor(wh57.id, true)) rfStatLightning(*st, wh57.lightningCount, t, ymd);
        }
        wn34.tempC = round1(w.tC * 0.4f + 9.0f); rfHeard(wn34, 25000);
        wh31b.tempC = round1(w.tC + 1.1f); wh31b.humidity = 63; rfHeard(wh31b, 45000);
    });
    // air quality (USB powered), leak, indoor T/H/P, soil EC and leaf wetness sensors (wireless 7-12)
    RfSensorReading& wh45 = rfAdd(6, 0x460027F1, 8, RF_PROTO_FINEOFFSET, "WH46");
    wh45.hasTemp = wh45.hasHumidity = wh45.hasPm25 = wh45.hasPm10 = wh45.hasCo2 = wh45.hasPm1 = wh45.hasPm4 = true;
    wh45.tempC = 29.3f; wh45.humidity = 51; wh45.pm25 = 5.0f; wh45.pm10 = 5.7f; wh45.co2Ppm = 779; wh45.extPower = true;
    wh45.pm1 = 4.2f; wh45.pm4 = 5.4f;
    RfSensorReading& wh55 = rfAdd(7, 0x5530CF55, 5, RF_PROTO_FINEOFFSET, "WH55");
    wh55.channel = 4; wh55.hasLeak = true; wh55.leakHighSensitivity = true; wh55.leakRaw = 649;
    RfSensorReading& wh52 = rfAdd(9, 0xA2005D8D, 4, RF_PROTO_FINEOFFSET, "WH52");
    wh52.hasTemp = wh52.hasMoisture = wh52.hasConductivity = true;
    wh52.tempC = 22.3f; wh52.moisturePct = 91; wh52.conductivityUsCm = 643.4f;
    RfSensorReading& wn35 = rfAdd(10, 0x3500305E, 4, RF_PROTO_FINEOFFSET, "WN35");
    wn35.hasLeafWetness = true; wn35.leafWetnessPct = 7;
    RfSensorReading& wh41 = rfAdd(11, 0x410000C7, 8, RF_PROTO_FINEOFFSET, "WH41");
    wh41.hasPm25 = wh41.hasPm10 = wh41.pm10Estimated = true; wh41.pm25 = 43.0f; wh41.pm10 = 45.0f;
    rfHeard(wh45, 120000); rfHeard(wh55, 50000); rfHeard(wh52, 60000);
    rfHeard(wn35, 70000); rfHeard(wh41, 240000);
    wn34.rssi = -79.0f; wh31b.rssi = -84.0f;
    rfHeard(wh31, 420000);   // WH31 lost: values stale, no longer a source
    wh31.rssi = -88.0f; wh51.batteryOk = false; wh51.rssi = -81.0f; wh57.rssi = -69.0f;
    srcUpdateCycle();
    syncedDay(3.4f, 31.5f, 466.1f, 170, 264);
}

// V4 Full fully equipped: Davis wind + rain, BME280, 3 DS18B20 (one gone),
// LiPo, calibrated UV, wind 2 (speed + direction), heater/fan auto with
// tacho, Victron MPPT; LoRaWAN + MQTT with commands.
void fixturePcbFull()
{
    common("Weather Mast");
    gCfg.pcbVersion = PCB_REV_V4_FULL; gBoard.pcb = true;
    gCfg.rfEn = RF_RADIO_OFF;
    gCfg.windEn = gCfg.rainEn = 1;
    gCfg.loraEn = 1; gCfg.mqttEn = 1; gCfg.mqttCtrlEn = 1;
    gCfg.lipoEn = 1;
    gCfg.uvEn = gCfg.uvIndexEn = gCfg.uvCalibrated = 1;
    gCfg.wind2Mode = 2;
    gCfg.heaterMode = 2; gCfg.fanMode = 2; gCfg.tachoEn = 1; gCfg.veDirectEn = 1;
    snprintf(gCfg.windyId, sizeof(gCfg.windyId), "secret-windy");
    bme280Present = true;
    bus1Count = 3;
    run(50, 0.75f, [&](const Wx& w) {
        boardAir(w); boardWind(w);
        rainMMSinceStart += w.rainMm;
        bus1Temps[0] = round1(w.tC - 0.4f); bus1Temps[1] = round1(9.5f + 0.5f * noise()); bus1Temps[2] = round1(w.tC + 1.2f);
        batteryVolt = 3.98f + 0.05f * noise();
        gIo.uvSensorVolts = w.uv / 4.0f; gIo.uvIndexCalibrated = true; gIo.uvIndex = w.uv;
        gIo.wind2Valid = true; gIo.wind2SpeedMs = w.windMs * 0.8f; gIo.wind2GustMs = w.gustMs * 0.8f; gIo.wind2DirDeg = (uint16_t)((w.dir + 10) % 360);
    });
    bus1Temps[1] = DEVICE_DISCONNECTED_C;   // probe 2 unplugged
    batteryVolt = 4.07f;
    gIo.heaterOn = false; gIo.heaterDuty = 0;
    gIo.fanOn = true; gIo.fanDuty = 100; gIo.fanRpm = 2950.0f; gIo.fanStalled = false;
    gIo.veValid = true;
    gIo.ve = { 13.28f, 1.23f, 37.5f, 45.0f, 0.12f, NAN, 0.31f, 3, 0 };
    srcUpdateCycle();
    syncedDay(5.2f, 28.8f, 530.4f, 0, 0);
}

// V4 Light (pcbRev 21): Davis wind + rain, BME280, one DS18B20 - with the I/O
// settings of a V4 Full left switched on (moved module / restored backup).
// Not a docs example: test/api_contract.test.js checks that none of the
// I/O block (UV, AUX, wind 2, heater, fan, VE.Direct) reaches the API.
void fixtureV4Light()
{
    common("Light");
    gCfg.pcbVersion = PCB_REV_V4_LIGHT; gBoard.pcb = true; gBoard.pcbLight = true;
    gCfg.rfEn = RF_RADIO_OFF;
    gCfg.windEn = gCfg.rainEn = 1;
    gCfg.uvEn = gCfg.uvIndexEn = gCfg.uvCalibrated = 1;
    gCfg.auxAinEn = 1; gCfg.auxPulseEn = 1; gCfg.wind2Mode = 2;
    gCfg.heaterMode = 2; gCfg.fanMode = 1; gCfg.tachoEn = 1; gCfg.veDirectEn = 1;
    bme280Present = true;
    bus1Count = 1;
    run(10, 0.2f, [&](const Wx& w) {
        boardAir(w); boardWind(w);
        rainMMSinceStart += w.rainMm;
        bus1Temps[0] = round1(w.tC - 0.4f);
    });
    srcUpdateCycle();
    syncedDay(1.0f, 20.0f, 400.0f, 0, 0);
}

// Imperial units, V4 Full with Davis, BME280, LiPo, AUX analog + pulse
// counter, heater held off by the battery lockout, debug mode, update check.
void fixtureImperial()
{
    common("");   // unnamed: host name from the MAC
    gCfg.unitSystem = 1;
    gCfg.pcbVersion = PCB_REV_V4_FULL; gBoard.pcb = true;
    gCfg.rfEn = RF_RADIO_OFF;
    gCfg.windEn = gCfg.rainEn = 1;
    gCfg.lipoEn = 1;
    gCfg.auxAinEn = 1; gCfg.auxPulseEn = 1;
    gCfg.heaterMode = 1;
    gCfg.debugMode = 1;
    gCfg.updateCheckEn = 1;
    gCfg.uiFlashEn = 1;   // Web UI: flash on, age off in this example
    snprintf(gCfg.timeZone, sizeof(gCfg.timeZone), "EST5EDT,M3.2.0,M11.1.0");
    hostUpdate = "2.0.1";
    loopCount = 17;
    bme280Present = true;
    run(30, 0.0f, [&](const Wx& w) {
        boardAir(w); boardWind(w);
        rainMMSinceStart += w.rainMm;
        batteryVolt = 3.52f;
        gIo.auxAinV = 1.874f; gIo.auxPulsesCycle = 37; gIo.auxPulsesTotal += 37;
    });
    gIo.battLockout = true; gIo.heaterOn = false; gIo.heaterDuty = 0;
    srcUpdateCycle();
    syncedDay(0.0f, 12.7f, 301.7f, 0, 0);
}

// Heltec V4 (16 MB, KCT8103L front end) 40 min after a task watchdog reset
// (the reset history example: power-on, settings restart, the crash): BME280,
// Bresser 5-in-1 and a TFA IT+ thermo-hygro sensor; clock not synced yet,
// so no daily totals, no trends - fields are absent, not zero.
void fixtureV4()
{
    common("Balcony V4");
    gBoard.flashBytes = 16UL * 1024 * 1024; gBoard.psramCap = 2; gBoard.fem = FEM_KCT8103L;
    gCfg.rfProto = RF_PROTO_BRESSER | RF_PROTO_ITPLUS17;
    gCfg.windUnitMs = 0;
    bme280Present = true;
    hostClockSynced = false;
    RfSensorReading& r = rfAdd(0, 0x0000A1F3, 1, RF_PROTO_BRESSER);
    RfSensorReading& tfa = rfAdd(1, 0x0000002C, 2, RF_PROTO_ITPLUS17);
    tfa.hasTemp = tfa.hasHumidity = true;
    run(40.0f / 60.0f, 0.0f, [&](const Wx& w) {
        boardAir(w);
        radioWeather(r, w, false); boardWind(w);
        tfa.tempC = round1(w.tC - 1.3f); tfa.humidity = 77; rfHeard(tfa, 31000);
    });
    tfa.batteryOk = false; tfa.rssi = -90.0f;
    srcUpdateCycle();
    // virtual sensors (vsensor.h): the BME280 as a WN32P, a WH31 on channel 2 from the TFA sensor
    VsSettings vs;
    vs.enabled = 1;
    vs.slot[0].type = VS_WN32P; vs.slot[0].enabled = 1;
    snprintf(vs.slot[0].src[VF_TEMP], VS_SRC_LEN, "temp"); snprintf(vs.slot[0].src[VF_HUM], VS_SRC_LEN, "hum");
    snprintf(vs.slot[0].src[VF_PRESS], VS_SRC_LEN, "press");
    vs.slot[1].type = VS_WH31; vs.slot[1].enabled = 1; vs.slot[1].channel = 2;
    snprintf(vs.slot[1].src[VF_TEMP], VS_SRC_LEN, "rf0000002C_temp");
    vsSettingsWrite(vs);
    {   // reset history: a power-on, a settings restart, then a task watchdog with its core dump summary
        ResetHeartbeat hb;
        heartbeatStart(hb);
        resetHistoryPush(hostResetHistory, resetRecordMake(RC_POWER_ON, nullptr, nullptr), nullptr);
        heartbeatUpdate(hb, 86400 * 3 + 600, 1795940000u);
        heartbeatNoteRestart(hb, "settings saved");
        resetHistoryPush(hostResetHistory, resetRecordMake(RC_SOFTWARE, &hb, nullptr), nullptr);
        heartbeatStart(hb);
        heartbeatUpdate(hb, 7260, 1796100000u);
        CrashSummary c;
        c.valid = 1; c.depth = 3; c.pc = 0x42013a2cu;
        c.backtrace[0] = 0x42013a2cu; c.backtrace[1] = 0x42013b5fu; c.backtrace[2] = 0x4200d7e1u;
        snprintf(c.task, sizeof(c.task), "IDLE0");
        crashReasonCompact("Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:\n - loopTask (CPU 1)",
                           c.reason, sizeof(c.reason));
        snprintf(c.firmware, sizeof(c.firmware), "4.0.0");
        snprintf(c.elfSha, sizeof(c.elfSha), "3f9a2c1be07d4410");
        const ResetRecord r = resetRecordMake(RC_TASK_WATCHDOG, &hb, &c);
        c.epoch = r.epoch;
        resetHistoryPush(hostResetHistory, r, &c);
    }
    gVsStatus[0].sent = 42; gVsStatus[0].lastTxMs = millis() - 20000; gVsStatus[0].skipMissing = 1;
    gVsBandLastHourMs = 1200;
}

// Davis Vantage Pro2 ISS (exclusive mode, transmitter ID 1) tracked for 6 h,
// BME280 for pressure; LoRaWAN configured but paused by the Davis mode.
void fixtureDavis()
{
    common("Davis Garden");
    gCfg.rfProto = RF_PROTO_DAVIS;
    gCfg.loraEn = 1;
    gCfg.davisOffsetHz = -3000;
    gCfg.davisSniffer = 1;
    bme280Present = true;
    RfSensorReading& r = rfAdd(0, 0xDA000001, 1, RF_PROTO_DAVIS, "Vantage Pro2");
    r.channel = 1;
    run(6, 0.5f, [&](const Wx& w) {
        boardAir(w); radioWeather(r, w, true); boardWind(w);
        r.hasSolar = true; r.solarWm2 = roundf(w.lux / RF_DAVIS_LUX_PER_WM2);
        r.lightLux = r.solarWm2 * RF_DAVIS_LUX_PER_WM2;
        rfHeard(r, 1500);
    });
    r.rssi = -83.0f;
    DavisDiag& d = gDavisDiag;
    d = DavisDiag();
    d.running = true; d.state = DAVIS_DIAG_TRACK; d.txId = 1; d.channel = 3; d.freqMHz = 868.1762f;
    d.periodMs = 2562.53f; d.offsetHz = -3000; d.learnedOffsetHz = -3000;
    d.packets = 8391; d.crcErrors = 37; d.otherPackets = 4; d.misses = 41; d.trackSlots = 8430; d.resyncs = 1;
    d.havePacket = true; d.lastPacketMs = millis() - 1500; d.lastRssi = -83.0f;
    d.haveTiming = true; d.lastTimingErrUs = 1200;
    d.hasRainRate = true; d.rainRateMmH = 1.2f; d.hasGust = true; d.gust10MinMs = 6.7f;
    struct { uint8_t t; uint8_t b[6]; const char* text; uint32_t n; unsigned long ago; } m[] = {
        { 0x4, { 0x40, 0x03, 0x88, 0x12, 0x45, 0x00 }, "UV index 1.5", 420, 31000 },
        { 0x5, { 0x50, 0x03, 0x88, 0xFF, 0x75, 0x00 }, "no rain", 2100, 6500 },
        { 0x6, { 0x60, 0x03, 0x88, 0x89, 0x05, 0x00 }, "965 W/m2", 420, 18000 },
        { 0x8, { 0x80, 0x03, 0x88, 0x03, 0x18, 0x00 }, "61.7 F / 16.5 C", 2100, 1500 },
        { 0x9, { 0x90, 0x03, 0x88, 0x0F, 0x05, 0x30 }, "10-min gust 15 mph (index 3)", 420, 44000 },
        { 0xA, { 0xA0, 0x03, 0x88, 0x6E, 0x28, 0x00 }, "62.2 %", 840, 11500 },
        { 0xE, { 0xE0, 0x03, 0x88, 0x2B, 0x05, 0x00 }, "tip counter 43", 2100, 4000 },
    };
    for (auto& x : m) {
        DavisTypeDiag& t = d.types[x.t];
        t.count = x.n; t.lastMs = millis() - x.ago;
        memcpy(t.bytes, x.b, 6);
        snprintf(t.text, sizeof(t.text), "%s", x.text);
    }
    srcUpdateCycle();
    syncedDay(0.0f, 18.2f, 402.6f, 240, 188);
}

void print(const char* name, JsonDocument& doc)
{
    std::string s;
    serializeJson(doc, s);
    printf("%s %s\n", name, s.c_str());
}

} // namespace

// records_store.cpp is not built here: today's records of the simulated run
bool recordsToday(Records& out) { out = hostRecords; return hostClockSynced && hostRecords.ymd != 0; }

// deviceHostname() (api_info.cpp): DHCP and mDNS host name.
void checkHostname()
{
    auto host = [](const char* name) {
        snprintf(gCfg.deviceName, sizeof(gCfg.deviceName), "%s", name);
        static char out[HOSTNAME_MAX_LEN + 1];
        deviceHostname(out, sizeof(out));
        return std::string(out);
    };
    assert(host("Garden") == "garden");
    assert(host("Weather Mast") == "weather-mast");
    assert(host("  --Wetter__Station #2!! ") == "wetter-station-2");
    assert(host("weather-node") == "weather-node");
    assert(host("Wetterstation Höhe") == "wetterstation-h-he");   // UTF-8 bytes are "other characters"
    assert(host("") == "weathernode-123456");                     // last 3 MAC bytes (24:AB:CD:12:34:56)
    assert(host("!!!") == "weathernode-123456");
    // at most 31 characters (the core's DHCP hostname buffer), never a trailing '-'
    assert(host("abcdefghijklmnopqrstuvwxyz0123456789") == "abcdefghijklmnopqrstuvwxyz01234");
    assert(host("abcdefghijklmnopqrstuvwxyz0123 x") == "abcdefghijklmnopqrstuvwxyz0123");
    assert(host("abcdefghijklmnopqrstuvwxyz012 xy") == "abcdefghijklmnopqrstuvwxyz012-x");
    char small[6];
    hostnameFromName("Weather Mast", small, sizeof(small));
    assert(std::string(small) == "weath");
    hostnameFromName("ab cd", small, 4);
    assert(std::string(small) == "ab");
}

// GNSS module (gnss.cpp; overrides api_info.cpp's weak defaults): off in
// every fixture, on only for checkGnssPositionPrivate()
static bool hostGnssOn = false;
bool gnssActive() { return hostGnssOn; }
void gnssStatus(GnssStatus& out)
{
    out = GnssStatus();
    out.active = out.seen = out.fix = out.posOk = out.altOk = hostGnssOn;
    out.lat = 47.123456; out.lon = 11.654321; out.altM = 560.4f; out.sats = 9;
}

// /api/info: the station's position only with withPosition (an admin
// login), the other GNSS fields for everyone
void checkGnssPositionPrivate()
{
    hostGnssOn = true;
    JsonDocument doc;
    buildInfoJson(doc);
    JsonObjectConst pub = doc["hardware"]["gnss"];
    assert(!pub.isNull() && pub["fix"] == true && pub["satellites"] == 9 && pub["altitudeM"].is<float>());
    assert(!pub["lat"].is<JsonVariantConst>() && !pub["lon"].is<JsonVariantConst>());
    buildInfoJson(doc, true);
    JsonObjectConst admin = doc["hardware"]["gnss"];
    assert(admin["lat"] == 47.12346 && admin["lon"] == 11.65432);
    hostGnssOn = false;
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    std::string f = argv[1];
    if (f == "hostname") { checkHostname(); return 0; }
    if (f == "gnssprivacy") { checkGnssPositionPrivate(); return 0; }
    if      (f == "bresser-7in1-bme280")            fixtureBresser();
    else if (f == "ecowitt-ws90-wh31-wh51-wh57")    fixtureEcowitt();
    else if (f == "pcb-v2-full")                    fixturePcbFull();
    else if (f == "pcb-v2-imperial")                fixtureImperial();
    else if (f == "heltec-v4-wireless")             fixtureV4();
    else if (f == "davis-vantage-pro2")             fixtureDavis();
    else if (f == "v4-light")                       fixtureV4Light();   // gating check, no docs example
    else { fprintf(stderr, "unknown fixture %s\n", f.c_str()); return 2; }

    JsonDocument doc;
    buildJsonPayload(doc);   assert(!doc.overflowed()); print("data", doc);
    for (const HistoryRangeDef& r : HISTORY_RANGES) {
        HistoryQuery q;
        assert(historyParseQuery(r.name, nullptr, nullptr, q));
        buildHistoryJson(doc, q); assert(!doc.overflowed());
        print((std::string("history-") + r.name).c_str(), doc);
    }
    buildInfoJson(doc);      assert(!doc.overflowed()); print("info", doc);
    recordsJson(doc, hostRecords, gCfg, hostClockSynced); assert(!doc.overflowed()); print("records", doc);
    {   // alarms (alarms.h): two ready-made rules and a wireless-sensor leak rule, frost active for 20 min
        AlarmRule rules[ALARM_RULES];
        AlarmState st[ALARM_RULES];
        alarmTemplate(0, rules[0]);
        rules[0].channels = (1 << AC_NTFY) | (1 << AC_MQTT);
        alarmTemplate(3, rules[1]);
        rules[2].src = AS_RF; rules[2].enabled = 1; snprintf(rules[2].sensorInternalId, sizeof(rules[2].sensorInternalId), "L39A582"); rules[2].rfField = AF_LEAK;
        rules[2].raise = rules[2].clear = 0.5f; rules[2].priority = 5; rules[2].repeatMin = 30;
        rules[2].channels = 1 << AC_PUSHOVER;
        snprintf(rules[2].name, sizeof(rules[2].name), "Cellar leak");
        st[0].active = true; st[0].since = 1000; st[0].valid = true; st[0].value = 1.0f;
        st[1].valid = true; st[1].value = -0.8f;
        AlarmChannels ch;
        snprintf(ch.ntfyTopic, sizeof(ch.ntfyTopic), "secret-topic");
        snprintf(ch.poToken, sizeof(ch.poToken), "secret-token");
        snprintf(ch.poUser, sizeof(ch.poUser), "secret-user");
        alarmsJson(doc, rules, st, ch, gCfg, 2200, [](const char*, char* customId) -> uint32_t { strcpy(customId, "wetter"); return 0x39582376; });
        assert(!doc.overflowed()); print("alarms", doc);
    }
    return 0;
}
