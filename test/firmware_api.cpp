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
#include "json_payload.h"
#include "board_io.h"
#include "rf_sensors.h"
#include "sources.h"
#include "sensors.h"
#include <DallasTemperature.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

// ---- host stubs for modules not built here ----
static bool hostClockSynced = true;
bool timeIsValid() { return hostClockSynced; }
// Wall clock for history.cpp's bucket alignment: set by run() so that the
// run ends at NOW_EPOCH (see there).
static int64_t hostEpochAtZero = 0;
bool epochNow(int64_t& out)
{
    if (!hostClockSynced) return false;
    out = hostEpochAtZero + (int64_t)(millis() / 1000);
    return true;
}
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
    r.lastMs = r.tempMs = r.humMs = r.windMs = r.rainMs = r.uvMs = r.lightMs = t;
}

constexpr unsigned long CYCLE_MS = 300000;
constexpr int64_t NOW_EPOCH = 1781524800;   // end of every run: 2026-06-15T12:00:00Z (14:00 CEST)

// Rain bookkeeping of the run for the daily statistics (wx_upload.cpp is
// not built here): today = the last 14 h (since local midnight), yesterday
// = the 24 h before.
struct RunRain { float todayMm = 0, yesterdayMm = 0; int64_t lastRainEpoch = 0; };
RunRain runRain;

// Runs `hours` of 5-min cycles: set(w) puts the cycle's values into the
// globals, then the cycle completes like loop() does (srcUpdateCycle,
// pushHistorySample).
void run(float hours, float rainLastHours, const std::function<void(const Wx&)>& set)
{
    int cycles = (int)lroundf(hours * 3600000.0f / CYCLE_MS);
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
        pushHistorySample();
    }
    gBaseCycleMs = CYCLE_MS;
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
    snprintf(gCfg.deviceName, sizeof(gCfg.deviceName), "%s", name);
    gResetReason = "power-on";
    sendFailCount = 0;
    cpuTemperature = 41.5f;
    hostNow = 1000;
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

// Ecowitt / Froggit: WS90 array, WH31 (not heard for 7 min), WH51, WH57; no BME280.
void fixtureEcowitt()
{
    common("Ecowitt Hub");
    gCfg.rfProto = RF_PROTO_FINEOFFSET;
    gCfg.windUnitMs = 0;   // km/h
    gCfg.loxEn = 1;
    bme280Present = false;
    RfSensorReading& ws = rfAdd(0, 0x000C3A7F, 1, RF_PROTO_FINEOFFSET, "WS90");
    ws.hasSupercap = true;
    RfSensorReading& wh31 = rfAdd(1, 0x000000B2, 2, RF_PROTO_FINEOFFSET, "WH31");
    wh31.channel = 2; wh31.hasTemp = wh31.hasHumidity = true;
    RfSensorReading& wh51 = rfAdd(2, 0x0000D0C7, 4, RF_PROTO_FINEOFFSET, "WH51");
    wh51.hasMoisture = true;
    RfSensorReading& wh57 = rfAdd(3, 0x0000E21A, 9, RF_PROTO_FINEOFFSET, "WH57");
    wh57.hasLightning = true;
    run(30, 0.5f, [&](const Wx& w) {
        radioWeather(ws, w, true); boardWind(w);
        ws.supercapV = 3.86f;
        wh31.tempC = round1(w.tC + 8.5f); wh31.humidity = 48; rfHeard(wh31, 20000);
        wh51.moisturePct = 41; rfHeard(wh51, 40000);
        wh57.lightningCount = 12; wh57.lightningKm = 8; rfHeard(wh57, 30000);
    });
    rfHeard(wh31, 420000);   // WH31 lost: values stale, no longer a source
    wh31.rssi = -88.0f; wh51.batteryOk = false; wh51.rssi = -81.0f; wh57.rssi = -69.0f;
    srcUpdateCycle();
    syncedDay(3.4f, 31.5f, 466.1f, 170, 264);
}

// PCB v2.0 fully equipped: Davis wind + rain, BME280, 3 DS18B20 (one gone),
// LiPo, calibrated UV, wind 2 (speed + direction), heater/fan auto with
// tacho, Victron MPPT; LoRaWAN + MQTT with commands.
void fixturePcbFull()
{
    common("Weather Mast");
    gCfg.pcbVersion = PCB_REV_2_0;
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
        gIo.wind2SpeedMs = w.windMs * 0.8f; gIo.wind2GustMs = w.gustMs * 0.8f; gIo.wind2DirDeg = (uint16_t)((w.dir + 10) % 360);
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

// Imperial units, PCB v2.0 with Davis, BME280, LiPo, AUX analog + pulse
// counter, heater held off by the battery lockout, debug mode, update check.
void fixtureImperial()
{
    common("");   // unnamed: host name from the MAC
    gCfg.unitSystem = 1;
    gCfg.pcbVersion = PCB_REV_2_0;
    gCfg.rfEn = RF_RADIO_OFF;
    gCfg.windEn = gCfg.rainEn = 1;
    gCfg.lipoEn = 1;
    gCfg.auxAinEn = 1; gCfg.auxPulseEn = 1;
    gCfg.heaterMode = 1;
    gCfg.debugMode = 1;
    gCfg.updateCheckEn = 1;
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

// Heltec V4 (16 MB, KCT8103L front end) 40 min after power-on: BME280,
// Bresser 5-in-1 and a TFA IT+ thermo-hygro sensor; clock not synced yet,
// so no daily totals, no trends - fields are absent, not zero.
void fixtureV4()
{
    common("Balcony V4");
    gBoard.kind = gBoard.detected = BOARD_V4;
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
}

void print(const char* name, JsonDocument& doc)
{
    std::string s;
    serializeJson(doc, s);
    printf("%s %s\n", name, s.c_str());
}

} // namespace

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

int main(int argc, char** argv)
{
    assert(argc == 2);
    std::string f = argv[1];
    if (f == "hostname") { checkHostname(); return 0; }
    if      (f == "bresser-7in1-bme280")            fixtureBresser();
    else if (f == "ecowitt-ws90-wh31-wh51-wh57")    fixtureEcowitt();
    else if (f == "pcb-v2-full")                    fixturePcbFull();
    else if (f == "pcb-v2-imperial")                fixtureImperial();
    else if (f == "heltec-v4-wireless")             fixtureV4();
    else { fprintf(stderr, "unknown fixture %s\n", f.c_str()); return 2; }

    JsonDocument doc;
    buildJsonPayload(doc);   assert(!doc.overflowed()); print("data", doc);
    buildHistoryJson(doc, false); assert(!doc.overflowed()); print("history-24h", doc);
    buildHistoryJson(doc, true);  assert(!doc.overflowed()); print("history-7d", doc);
    buildInfoJson(doc);      assert(!doc.overflowed()); print("info", doc);
    return 0;
}
