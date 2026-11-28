#include "ota_upload.h"
#include "wx_upload.h"
#include "globals.h"
#include "time_sync.h"
#include "wifi_mqtt.h"
#include "generated_version.h"
#include "serial_log.h"
#include "sources.h"
#include "derived.h"
#include "history.h"
#include "board_io.h"
#include "sensors.h"
#include "rain_stats.h"
#include "system_restart.h"
#include "awekas.h"
#include "records.h"
#include "out_status.h"
#include "rf_sensors.h"
#include "alarms.h"
#include "notify.h"
#include "custom_upload.h"
#include "ph_live.h"
#include "wx_formats.h"
#include "tls_gate.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <MD5Builder.h>
#include <esp_attr.h>
#include <stddef.h>
#include <functional>
#include <memory>
#include <new>

// The status slots (out_status.h) are laid out by hand: one per weather
// network, per alarm channel without MQTT, per custom upload.
static_assert((size_t)(OUT_NOTIFY_FIRST - OUT_WX_FIRST) == WX_TARGET_COUNT, "out_status.h: one slot per weather network");
static_assert(OUT_CUSTOM_FIRST - OUT_NOTIFY_FIRST == AC_COUNT - 1, "out_status.h: one slot per alarm channel (not MQTT)");
static_assert(OUT_COUNT - OUT_CUSTOM_FIRST == CUSTOM_UPLOADS, "out_status.h: one slot per custom upload");

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

// Unit conversions, same constants as weather-data-bridge's common.py.
constexpr float MS_TO_KMH   = 3.6f;
constexpr float MPH_TO_KMH  = 1.609344f;
constexpr float IN_TO_MM    = 25.4f;
constexpr float INHG_TO_HPA = 33.8639f;

constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr uint16_t HTTP_TIMEOUT_MS         = 8000;
constexpr uint32_t TLS_HANDSHAKE_TIMEOUT_S = 8;

// One network's aggregation window — the equivalent of the bridge's
// WindowAggregator: average for wind speed/pressure/humidity/temperature/
// rain rate, max for gust, circular mean for wind direction (a plain
// average is wrong across 0°/360°: 350° and 10° would "average" to 180°).
// Fed once per measurement cycle, which is already an average over that
// cycle's sub-samples (see computeFinalValues() in main.cpp).
struct Agg {
    uint16_t windCount = 0, rainCount = 0, bmeCount = 0, humCount = 0, pressCount = 0;   // bmeCount: temperature
    float    windSum = 0, gustMax = 0, dirSin = 0, dirCos = 0;
    float    rainRateSum = 0;
    float    tempSum = 0, humSum = 0, pressSum = 0;
};

// Aggregated values in the units the networks are built around (metric).
// A field is only valid if its group's count was non-zero.
struct Reading {
    bool  hasWind = false, hasRain = false, hasBme = false, hasHum = false, hasPress = false;   // hasBme: temperature
    float windKmh = 0, gustKmh = 0;
    bool  hasDir = false;     // with hasWind: the direction is known (calm / no vane: left out, never 0 = north)
    int   dirDeg = 0;
    float rainRateMmH = 0, dailyRainMm = 0;
    float tempC = 0, humidity = 0, pressureHpa = 0;
    // filled in the upload task right before sending (outside wxMutex)
    float rain1hMm = 0;
    bool  hasUv = false, hasLux = false;
    float uvIndex = 0, lux = 0;
    bool  hasSolar = false;       // measured solar radiation (Davis solar sensor as the light source)
    float solarWm2 = 0;
    bool  hasSunshine = false;    // sunshine duration today (needs a light source)
    float sunshineHours = 0;
    bool  hasTendency = false;    // 3-h pressure tendency (QNH now minus 3 h ago)
    float pressDelta3hHpa = 0;
    float windMs() const { return windKmh / MS_TO_KMH; }
};

struct Target {
    WxTargetStatus st;
    Agg            agg;
    unsigned long  windowStartMs;   // 0 = window not started yet (static storage: zero-initialized)
};

enum TargetIdx { T_AWEKAS = 0, T_WU = 1, T_WCLOUD = 2, T_WNETPRO = 3, T_WINDY = 4, T_OSEM = 5, T_WOW = 6, T_CWOP = 7 };

// Wind for every network: WMO-style 10-min values from the wind ring
// (main.cpp) - 10-min mean, highest 3-s gust and vector-mean direction of the
// last 10 minutes - independent of each network's upload interval, so the
// station is comparable with others. Updated every base cycle (wxMutex).
WindStats wind10;

Target targets[WX_TARGET_COUNT] = {
    { { "AWEKAS" } },
    { { "Weather Underground" } },
    { { "Weathercloud" } },
    { { "Wetternetzwerk.pro" } },
    { { "Windy" } },
    { { "openSenseMap" } },
    { { "Met Office WOW" } },
    { { "CWOP" } },
};

// Guards targets[] (aggregators + status), rtcRain and the upload-related
// Config fields — see wxUploadLock() in the header. pushCycle runs on the
// main loop task, the upload loop on its own task, the status page on the
// web server task.
SemaphoreHandle_t wxMutex = xSemaphoreCreateMutex();

// Rain/sunshine bookkeeping (rain_stats.h): today and yesterday in local
// days, the last rain, rain this week/month/year and what was collected
// while the clock was unknown. RTC_NOINIT_ATTR: lives in RTC RAM, which
// survives a software restart/watchdog reset (unlike normal RAM), but not a
// power cycle - so a reboot mid-day doesn't drop the totals, without writing
// anything to flash. The bridge solves the same problem by re-reading its
// database on start.
//
// RTC_NOINIT variables have no fixed address: the linker places .rtc_noinit
// right after .rtc.data (LoRaWAN session etc.) in link order, so a firmware
// that adds/changes an RTC variable placed before these (or within this
// file) moves them - after such an OTA update the magic/check no longer
// matches and the RTC copy is dropped (seen on the update to 2.0.0-rc.2,
// whose new week/month/year part moved the daily part by 32 bytes: "rain
// since start" in sensors.cpp kept its address and survived, yesterday/last
// rain were lost). Hence (a) all of it in ONE RTC_NOINIT object, so adding
// variables in this file can't reorder its parts - in the same order and
// layout as the separate variables of 2.0.0-rc.2 (periods, pending, daily at
// +0/+32/+48), so the update from rc.2 keeps them - and (b) with
// gCfg.rainPersist a flash copy (cfgdata NVS, namespace "rainstat",
// RAIN_BLOB_* in rain_stats.h) written at most hourly when changed, at the
// day roll-over / a new period and right before every planned restart
// (systemRestart() -> rainStatsFlush()); used at boot when the RTC copy is
// not valid (power loss, moved layout). Each part keeps its magic and check.
struct DailyRain {
    uint32_t magic;
    int32_t  ymd;          // RainDaily (rain_stats.h), flat: layout of 2.0.0-rc.2
    float    mm;
    int32_t  prevYmd;
    float    prevMm;
    float    sunSec;
    float    prevSunSec;
    int64_t  lastRainEpoch;
    uint32_t check;
};
constexpr uint32_t DAILY_RAIN_MAGIC = 0x52414E32;  // "RAN2"

// Rain/sunshine collected while the clock is unknown (after a restart,
// before the first NTP sync): the date in DailyRain is the one from before
// the restart, so these can't be added to it yet - the roll-over adds them
// to "today" once the date is known. Without this, a sync on a new day moved
// everything since the restart into "yesterday".
struct PendingRain {
    uint32_t magic;
    float    mm;
    float    sunSec;
    uint32_t check;
};
constexpr uint32_t PENDING_RAIN_MAGIC = 0x524E5031;  // "RNP1"

// Rain this week / month / year.
struct PeriodRain {
    uint32_t    magic;
    RainPeriods p;
    uint32_t    check;
};
constexpr uint32_t PERIOD_RAIN_MAGIC = 0x52505731;  // "RPW1"

struct RainRtc {
    PeriodRain  periods;   // +0
    PendingRain pending;   // +32
    DailyRain   daily;     // +48 (8-aligned)
};
static_assert(offsetof(RainRtc, pending) == 32 && offsetof(RainRtc, daily) == 48 && sizeof(RainRtc) == 96,
              "RTC layout of 2.0.0-rc.2 (keeps the totals over the update from it)");
RTC_NOINIT_ATTR RainRtc rtcRainState;
DailyRain&   rtcRain    = rtcRainState.daily;
PendingRain& rtcPending = rtcRainState.pending;
PeriodRain&  rtcPeriods = rtcRainState.periods;

// NVS copy: one blob (rain_stats.h, version 2: daily + periods + pending).
// Same key as the version 1 blob (periods only) of older firmware: that one
// is read and migrated, and overwritten in place by the next write.
constexpr const char* RAIN_NVS_NAMESPACE = "rainstat";
constexpr const char* RAIN_NVS_KEY       = "periods";

// Guards rainSave and the NVS writes of the rain record (main loop task's
// pushCycle and rainStatsFlush() from any task). Lock order: rainNvsMutex,
// then wxMutex.
SemaphoreHandle_t rainNvsMutex = xSemaphoreCreateMutex();
RainSaveState rainSave;
bool          rainReady = false;   // wxUploadSetup() has validated/restored the record

// FNV-1a over everything but the trailing check word.
uint32_t fnv1a(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

uint32_t dailyRainCheck(const DailyRain& r)     { return fnv1a(&r, offsetof(DailyRain, check)); }
uint32_t pendingRainCheck(const PendingRain& r) { return fnv1a(&r, offsetof(PendingRain, check)); }
uint32_t periodRainCheck(const PeriodRain& r)   { return fnv1a(&r, offsetof(PeriodRain, check)); }

void sealDailyRain()   { rtcRain.check = dailyRainCheck(rtcRain); }
void sealPendingRain() { rtcPending.check = pendingRainCheck(rtcPending); }
void sealPeriodRain()  { rtcPeriods.check = periodRainCheck(rtcPeriods); }

bool dailyRainOk()   { return rtcRain.magic == DAILY_RAIN_MAGIC && rtcRain.check == dailyRainCheck(rtcRain); }
bool pendingRainOk() { return rtcPending.magic == PENDING_RAIN_MAGIC && rtcPending.check == pendingRainCheck(rtcPending); }
bool periodRainOk()  { return rtcPeriods.magic == PERIOD_RAIN_MAGIC && rtcPeriods.check == periodRainCheck(rtcPeriods); }

// Caller holds wxMutex.
RainRecord rainRecordNow()
{
    RainRecord r;
    r.d.ymd = rtcRain.ymd;           r.d.mm = rtcRain.mm;
    r.d.prevYmd = rtcRain.prevYmd;   r.d.prevMm = rtcRain.prevMm;
    r.d.sunSec = rtcRain.sunSec;     r.d.prevSunSec = rtcRain.prevSunSec;
    r.d.lastRainEpoch = rtcRain.lastRainEpoch;
    r.p = rtcPeriods.p;
    r.pendingMm = rtcPending.mm;
    r.pendingSunSec = rtcPending.sunSec;
    return r;
}

// Caller holds wxMutex: stores r into RTC RAM (all parts, valid and sealed).
void rainRecordStore(const RainRecord& r)
{
    rtcRain.magic = DAILY_RAIN_MAGIC;
    rtcRain.ymd = r.d.ymd;           rtcRain.mm = r.d.mm;
    rtcRain.prevYmd = r.d.prevYmd;   rtcRain.prevMm = r.d.prevMm;
    rtcRain.sunSec = r.d.sunSec;     rtcRain.prevSunSec = r.d.prevSunSec;
    rtcRain.lastRainEpoch = r.d.lastRainEpoch;
    sealDailyRain();
    rtcPeriods.magic = PERIOD_RAIN_MAGIC;
    rtcPeriods.p = r.p;
    sealPeriodRain();
    rtcPending.magic = PENDING_RAIN_MAGIC;
    rtcPending.mm = r.pendingMm;
    rtcPending.sunSec = r.pendingSunSec;
    sealPendingRain();
}

int64_t uptimeSec() { return esp_timer_get_time() / 1000000LL; }

bool loadRainNvs(RainRecord& out, int& version)
{
    Preferences prefs;
    if (!prefs.begin(RAIN_NVS_NAMESPACE, true, "cfgdata")) return false;   // never written
    uint8_t buf[RAIN_BLOB_SIZE];
    const size_t len = prefs.getBytesLength(RAIN_NVS_KEY);
    version = len > 0 && len <= sizeof(buf) && prefs.getBytes(RAIN_NVS_KEY, buf, len) == len
            ? rainBlobDecode(buf, len, out) : 0;
    prefs.end();
    return version != 0;
}

bool writeRainNvs(const RainRecord& r)
{
    Preferences prefs;
    if (!prefs.begin(RAIN_NVS_NAMESPACE, false, "cfgdata")) return false;
    uint8_t buf[RAIN_BLOB_SIZE];
    rainBlobEncode(r, buf);
    bool ok = prefs.putBytes(RAIN_NVS_KEY, buf, sizeof(buf)) == sizeof(buf);
    prefs.end();
    return ok;
}

// Caller holds wxMutex. Rolls the record over onto the current local date
// (rain_stats.h: today -> yesterday, new week/month/year, the rain collected
// while the clock was unknown into today) - at worst off by whatever fell
// between midnight and the first sync after a reboot.
// Returns false (nothing changed) while the clock is unknown.
bool rollDailyRain()
{
    struct tm lt;
    if (!localTimeNow(lt)) return false;
    RainRecord r = rainRecordNow();
    if (rainRecordRoll(r, lt)) rainRecordStore(r);
    return true;
}

// Caller holds wxMutex: true if the stored "yesterday" date is the day before today.
bool prevIsYesterday()
{
    struct tm lt;
    if (!localTimeNow(lt) || rtcRain.prevYmd == 0) return false;
    return rainYmdDayNumber(rainYmd(lt)) - rainYmdDayNumber(rtcRain.prevYmd) == 1;
}

// Caller holds wxMutex.
void targetConfig(int idx, bool& enabled, int& intervalSec)
{
    switch (idx) {
        case T_AWEKAS:  enabled = gCfg.awekasUser[0];  intervalSec = gCfg.awekasIntervalSec;  break;
        case T_WU:      enabled = gCfg.wuStationId[0]; intervalSec = gCfg.wuIntervalSec;      break;
        case T_WCLOUD:  enabled = gCfg.wcloudId[0];    intervalSec = gCfg.wcloudIntervalSec;  break;
        case T_WNETPRO: enabled = gCfg.wnetproId[0];   intervalSec = gCfg.wnetproIntervalSec; break;
        case T_WINDY:   enabled = gCfg.windyId[0];     intervalSec = gCfg.windyIntervalSec;   break;
        case T_WOW:     enabled = gCfg.wowSiteId[0] && gCfg.wowPin[0]; intervalSec = gCfg.wowIntervalSec; break;
        case T_CWOP:    enabled = gCfg.cwopCall[0] && gCfg.latitude[0] && gCfg.longitude[0]; intervalSec = gCfg.cwopIntervalSec; break;
        default:        enabled = gCfg.osemBoxId[0] && gCfg.osemSensors[0]; intervalSec = gCfg.osemIntervalSec; break;
    }
    if (!gCfg.wifiEn) enabled = false;
}

bool aggEmpty(const Agg& a) { return !(a.windCount || a.rainCount || a.bmeCount || a.pressCount); }

Reading readingFrom(const Agg& a, float dailyRainMm, bool rainEn)
{
    Reading r;
    if (a.windCount && wind10.valid) {
        r.hasWind = true;
        r.windKmh = wind10.meanMs * MS_TO_KMH;
        r.gustKmh = wind10.gustMs * MS_TO_KMH;
        r.hasDir  = wind10.dirValid;
        r.dirDeg  = wind10.dirValid ? wind10.dirDeg : 0;
    } else if (a.windCount) {
        r.hasWind = true;
        r.windKmh = a.windSum / a.windCount * MS_TO_KMH;
        r.gustKmh = a.gustMax * MS_TO_KMH;
        int deg   = (int)roundf(atan2f(a.dirSin / a.windCount, a.dirCos / a.windCount) * RAD_TO_DEG);
        r.hasDir  = a.dirSin != 0.0f || a.dirCos != 0.0f;   // no direction vectors summed: unknown
        r.dirDeg  = (deg % 360 + 360) % 360;
    }
    if (rainEn) {
        r.hasRain     = true;
        r.dailyRainMm = dailyRainMm;
        r.rainRateMmH = a.rainCount ? a.rainRateSum / a.rainCount : 0.0f;
    }
    if (a.bmeCount)   { r.hasBme = true;   r.tempC = a.tempSum / a.bmeCount; }
    if (a.humCount)   { r.hasHum = true;   r.humidity = a.humSum / a.humCount; }
    if (a.pressCount) { r.hasPress = true; r.pressureHpa = a.pressSum / a.pressCount; }
    return r;
}

String urlEncode(const char* s)
{
    static const char hex[] = "0123456789ABCDEF";
    String out;
    out.reserve(strlen(s) * 3);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

String num(float v, int decimals) { return String(v, decimals); }

String formatTime(const char* fmt, bool utc)
{
    time_t now = time(nullptr);
    struct tm t;
    if (utc) gmtime_r(&now, &t);
    else localTimeOf(now, t);
    char buf[24];
    strftime(buf, sizeof(buf), fmt, &t);
    return String(buf);
}

// Credentials/settings copied out under wxMutex, so the actual (slow) HTTP
// request runs without holding it.
struct Creds {
    char id[33];
    char key[81];
    char lat[12];
    char lon[12];
    char map[321];   // openSenseMap sensor IDs
};

// --- Per-network request builders. Field sets and formats mirror the
// bridge's targets/*.py one-to-one (see there for protocol references). ---

// AWEKAS: one semicolon-separated "val" parameter in a fixed field order;
// unused positions stay empty. Local date/time, metric units, password as
// MD5 hex. Expected reply: "OK".
String awekasUrl(const Reading& r, const Creds& c)
{
    MD5Builder md5;
    md5.begin();
    md5.add(c.key);
    md5.calculate();
    String pass = md5.toString(), date = formatTime("%d.%m.%Y", false), time = formatTime("%H:%M", false);

    AwekasData d;
    d.user = c.id;  d.md5Pass = pass.c_str();  d.date = date.c_str();  d.time = time.c_str();
    d.hasTemp = r.hasBme;      d.tempC = r.tempC;
    d.hasHum = r.hasHum;       d.humidity = r.humidity;
    d.hasPress = r.hasPress;   d.pressureHpa = r.pressureHpa;
    d.hasRain = r.hasRain;     d.dailyRainMm = r.dailyRainMm;  d.rainRateMmH = r.rainRateMmH;
    d.hasWind = r.hasWind;     d.windKmh = r.windKmh;  d.gustKmh = r.gustKmh;  d.dirDeg = r.dirDeg;  d.hasDir = r.hasDir;
    d.hasTendency = r.hasTendency;  d.tendency = awekasTendency(r.pressDelta3hHpa);
    d.hasSolar = r.hasSolar;   d.solarWm2 = r.solarWm2;
    d.hasUv = r.hasUv;         d.uvIndex = r.uvIndex;
    // brightness only from a lux sensor; a Davis "lux" is converted from W/m2 - that one goes to solar instead
    d.hasLux = r.hasLux && !r.hasSolar;  d.lux = r.lux;
    d.hasSunshine = r.hasSunshine;  d.sunshineHours = r.sunshineHours;
    d.software = "weather-node-" FIRMWARE_VERSION;
    d.lon = c.lon;  d.lat = c.lat;
    char v[320];
    awekasVal(d, v, sizeof(v));
    return "http://data.awekas.at/eingabe_pruefung.php?val=" + urlEncode(v);
}

// Weather Underground's PWS Upload Protocol — also spoken by
// Wetternetzwerk.pro ("CustomServer" option), just with another host.
// Imperial units, UTC timestamp. Expected reply: "success".
String wuStyleUrl(const char* base, const Reading& r, const Creds& c)
{
    String u = base;
    u += "?action=updateraw&ID=";  u += urlEncode(c.id);
    u += "&PASSWORD=";             u += urlEncode(c.key);
    u += "&dateutc=";              u += urlEncode(formatTime("%Y-%m-%d %H:%M:%S", true).c_str());
    if (r.hasRain) { u += "&dailyrainin=";  u += num(r.dailyRainMm / IN_TO_MM, 2); }
    if (r.hasRain) { u += "&rainin=";       u += num(r.rain1hMm / IN_TO_MM, 2); }
    if (r.hasBme)   { u += "&tempf=";    u += num(r.tempC * 1.8f + 32.0f, 1); }
    if (r.hasHum)   { u += "&humidity="; u += num(r.humidity, 0); }
    if (r.hasBme && r.hasHum) { u += "&dewptf="; u += num(dewPointC(r.tempC, r.humidity) * 1.8f + 32.0f, 1); }
    if (r.hasUv)    { u += "&UV=";       u += num(r.uvIndex, 1); }
    if (r.hasPress) { u += "&baromin=";  u += num(r.pressureHpa / INHG_TO_HPA, 2); }
    if (r.hasWind) {
        u += "&windspeedmph="; u += num(r.windKmh / MPH_TO_KMH, 1);
        u += "&windgustmph=";  u += num(r.gustKmh / MPH_TO_KMH, 1);
        if (r.hasDir) { u += "&winddir="; u += String(r.dirDeg); }
    }
    return u;
}

// Weathercloud: values x10 as integers, wind in m/s, local timestamp.
// Expected reply: "200".
String wcloudUrl(const Reading& r, const Creds& c)
{
    auto x10 = [](float v) { return String((long)lroundf(v * 10.0f)); };
    String u = "http://api.weathercloud.net/v01/set?wid=";
    u += urlEncode(c.id);
    u += "&key=";  u += urlEncode(c.key);
    u += "&time="; u += urlEncode(formatTime("%Y%m%d %H%M%S", false).c_str());
    if (r.hasRain) {
        u += "&rain=";     u += x10(r.dailyRainMm);
        u += "&rainrate="; u += x10(r.rainRateMmH);
    }
    if (r.hasBme)   { u += "&temp="; u += x10(r.tempC); }
    if (r.hasHum)   { u += "&hum=";  u += num(r.humidity, 0); }
    if (r.hasBme && r.hasHum) {
        u += "&dew=";  u += x10(dewPointC(r.tempC, r.humidity));
        u += "&heat="; u += x10(heatIndexC(r.tempC, r.humidity));
    }
    if (r.hasBme && r.hasWind) { u += "&chill="; u += x10(windChillC(r.tempC, r.windMs())); }
    if (r.hasUv)    { u += "&uvi=";  u += x10(r.uvIndex); }
    if (r.hasPress) { u += "&bar=";  u += x10(r.pressureHpa); }
    if (r.hasWind) {
        u += "&wspd=";   u += x10(r.windKmh / MS_TO_KMH);
        u += "&wspdhi="; u += x10(r.gustKmh / MS_TO_KMH);
        if (r.hasDir) { u += "&wdir="; u += String(r.dirDeg); }
    }
    return u;
}

// Windy (stations.windy.com, PWS API v2): station ID + station password,
// metric (°C, m/s, Pa), precip = rain of the last 60 min. Expected: HTTP 200.
String windyUrl(const Reading& r, const Creds& c)
{
    String u = "https://stations.windy.com/api/v2/observation/update?id=";
    u += urlEncode(c.id);
    u += "&PASSWORD="; u += urlEncode(c.key);
    u += "&time=now";
    if (r.hasBme)   { u += "&temp=";     u += num(r.tempC, 1); }
    if (r.hasHum)   { u += "&humidity="; u += num(r.humidity, 0); }
    if (r.hasBme && r.hasHum) { u += "&dewpoint="; u += num(dewPointC(r.tempC, r.humidity), 1); }
    if (r.hasPress) { u += "&pressure="; u += String((long)lroundf(r.pressureHpa * 100.0f)); }
    if (r.hasWind) {
        u += "&wind=";    u += num(r.windMs(), 1);
        u += "&gust=";    u += num(r.gustKmh / MS_TO_KMH, 1);
        if (r.hasDir) { u += "&winddir="; u += String(r.dirDeg); }
    }
    if (r.hasRain)  { u += "&precip=";   u += num(r.rain1hMm, 1); }
    if (r.hasUv)    { u += "&uv=";       u += num(r.uvIndex, 1); }
    u += "&softwaretype=universal-weather-node-" FIRMWARE_VERSION;
    return u;
}

// openSenseMap: one JSON object {sensorId: value} per box. The sensor IDs
// come from the "key=sensorId" list (Creds::map); keys: temp (°C), hum (%),
// press (hPa, QNH), wind/gust (m/s), dir (°), rain (mm, last hour),
// rainday (mm today), dew (°C), uv (index), lux (lx).
String osemBody(const Reading& r, const Creds& c)
{
    JsonDocument doc;
    String list = c.map;
    int pos = 0;
    while (pos < (int)list.length()) {
        int comma = list.indexOf(',', pos);
        if (comma < 0) comma = list.length();
        String item = list.substring(pos, comma);
        pos = comma + 1;
        int eq = item.indexOf('=');
        if (eq <= 0) continue;
        String k = item.substring(0, eq), id = item.substring(eq + 1);
        k.trim(); id.trim(); k.toLowerCase();
        if (id.isEmpty()) continue;
        String v;
        if      (k == "temp"    && r.hasBme)   v = num(r.tempC, 1);
        else if (k == "hum"     && r.hasHum)   v = num(r.humidity, 0);
        else if (k == "press"   && r.hasPress) v = num(r.pressureHpa, 1);
        else if (k == "wind"    && r.hasWind)  v = num(r.windMs(), 1);
        else if (k == "gust"    && r.hasWind)  v = num(r.gustKmh / MS_TO_KMH, 1);
        else if (k == "dir"     && r.hasWind && r.hasDir) v = String(r.dirDeg);
        else if (k == "rain"    && r.hasRain)  v = num(r.rain1hMm, 1);
        else if (k == "rainday" && r.hasRain)  v = num(r.dailyRainMm, 1);
        else if (k == "dew"     && r.hasBme && r.hasHum) v = num(dewPointC(r.tempC, r.humidity), 1);
        else if (k == "uv"      && r.hasUv)    v = num(r.uvIndex, 1);
        else if (k == "lux"     && r.hasLux)   v = String((long)lroundf(r.lux));
        if (v.length()) doc[id] = v;
    }
    String body;
    serializeJson(doc, body);
    return body;
}

// The start of a reply body, for a status text or an "OK" check: at most
// maxBytes, never the whole body - getString() reserves a server-chosen
// Content-Length up front and reads a chunked / unbounded body to the end,
// which a large error page or a hostile endpoint would turn into memory
// pressure and a long block of the upload task. The rest is discarded by
// http.end() (no connection reuse). A chunked reply's first chunk-size line is
// dropped (the start of its data is what matters here).
constexpr size_t REPLY_START_MAX = 256;
constexpr unsigned long REPLY_READ_MS = 3000;
String readReplyStart(HTTPClient& http, size_t maxBytes = REPLY_START_MAX)
{
    String out;
    WiFiClient* s = http.getStreamPtr();   // Arduino-ESP32 2.0.x: WiFiClient
    if (!s) return out;
    const int size = http.getSize();   // -1 = not announced (chunked / until close)
    const size_t want = size >= 0 && (size_t)size < maxBytes ? (size_t)size : maxBytes;
    out.reserve(want);
    for (const unsigned long start = millis(); out.length() < want && millis() - start < REPLY_READ_MS;) {
        const int c = s->read();
        if (c >= 0) { out += (char)c; continue; }
        if (!s->connected()) break;
        delay(2);
    }
    if (size < 0) {   // chunked: "1a\r\n<data>" - drop the size line
        const int eol = out.indexOf("\r\n");
        bool hex = eol > 0 && eol <= 8;
        for (int i = 0; hex && i < eol; i++) hex = isxdigit((unsigned char)out[i]);
        if (hex) out.remove(0, eol + 2);
    }
    return out;
}

// The WiFi clients are declared before the HTTPClient using them: locals are
// destroyed in reverse order, so ~HTTPClient (which may still touch its
// client, e.g. after a failed begin()) runs while the client still exists.
int httpPostJson(const String& url, const String& auth, const String& json, String& body)
{
    WiFiClientSecure tls;
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    tls.setInsecure();
    tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
    if (!http.begin(tls, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
    http.addHeader("Content-Type", "application/json");
    if (auth.length()) http.addHeader("Authorization", auth);
    tlsBegin(); tlsLogHeap("POST", "before");
    int code = http.POST(json);
    if (code > 0) body = readReplyStart(http);
    http.end();
    tlsLogHeap("POST", "after"); tlsEnd();
    return code;
}

// Plain or TLS GET depending on the scheme. TLS is unverified (setInsecure)
// — same tradeoff as MQTT_TLS_INSECURE: encrypted, not authenticated; no CA
// bundle to keep up to date on the device for a handful of upload hosts.
// `onBody` (optional) reads a 200 reply straight from the stream instead of
// into `body` - for replies too large to hold as a String; any other reply
// is not read at all then. Without onBody, `body` = the start of the reply.
int httpGet(const String& url, String& body, std::function<void(Stream&)> onBody = nullptr)
{
    WiFiClient       plain;   // before `http`, see httpPostJson()
    WiFiClientSecure tls;
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    if (onBody) http.useHTTP10(true);   // no chunked encoding: getStream() is the raw body

    bool begun;
    if (url.startsWith("https://")) {
        tls.setInsecure();
        tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        begun = http.begin(tls, url);
    } else {
        begun = http.begin(plain, url);
    }
    if (!begun) return HTTPC_ERROR_CONNECTION_REFUSED;

    const bool isTls = url.startsWith("https://");
    if (isTls) { tlsBegin(); tlsLogHeap("GET", "before"); }
    int code = http.GET();
    if (onBody) { if (code == HTTP_CODE_OK) onBody(http.getStream()); }
    else if (code > 0) body = readReplyStart(http);
    http.end();
    if (isTls) { tlsLogHeap("GET", "after"); tlsEnd(); }
    return code;
}

void setReply(WxTargetStatus& st, const char* text)
{
    size_t n = 0;
    for (; *text && n < sizeof(st.lastReply) - 1; text++) {
        char c = *text;
        st.lastReply[n++] = (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
    st.lastReply[n] = '\0';
}

WxObs obsFrom(const Reading& r)
{
    WxObs o;
    o.hasTemp = r.hasBme; o.tempC = r.tempC;
    o.hasHum = r.hasHum;  o.humidity = r.humidity;
    o.hasPress = r.hasPress; o.qnhHpa = r.pressureHpa;
    o.hasWind = r.hasWind; o.windMs = r.windMs(); o.gustMs = r.gustKmh / MS_TO_KMH; o.dirDeg = r.dirDeg; o.hasDir = r.hasDir;
    o.hasRain = r.hasRain; o.rain1hMm = r.rain1hMm; o.rainDayMm = r.dailyRainMm; o.rain24hMm = historyRainSum(24 * 3600);
    return o;
}

// CWOP: one APRS packet over a short APRS-IS TCP session (login, packet, close).
int cwopSend(const Reading& r, const Creds& c, String& reply)
{
    int64_t e;
    if (!epochNow(e)) { reply = "no time"; return -1; }
    time_t t = (time_t)e;
    struct tm u;
    gmtime_r(&t, &u);
    char login[96], pkt[200];
    cwopLogin(c.id, gCfg.cwopPasscode, "UWN", FIRMWARE_VERSION, login, sizeof(login));
    if (!cwopPacket(obsFrom(r), c.id, atof(c.lat), atof(c.lon), u.tm_mday, u.tm_hour, u.tm_min, "UWN", pkt, sizeof(pkt))) {
        reply = "packet too long";
        return -1;
    }
    WiFiClient tcp;
    tcp.setTimeout(5);
    if (!tcp.connect("cwop.aprs.net", 14580, HTTP_CONNECT_TIMEOUT_MS)) { reply = "connect failed"; return -1; }
    String banner = tcp.readStringUntil('\n');   // "# aprsc ..."
    tcp.print(login); tcp.print("\r\n");
    String ack = tcp.readStringUntil('\n');      // "# logresp CW1234 unverified, server ..."
    tcp.print(pkt); tcp.print("\r\n");
    tcp.flush();
    delay(200);
    tcp.stop();
    ack.trim();
    reply = ack.length() ? ack : banner;
    return ack.indexOf("logresp") >= 0 ? 200 : -1;
}

void sendOne(int idx, const Reading& r, const Creds& c)
{
    String url, expect;
    switch (idx) {
        case T_AWEKAS: url = awekasUrl(r, c); expect = "OK"; break;
        case T_WU:
            url = wuStyleUrl("https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php", r, c);
            expect = "success";
            break;
        case T_WCLOUD: url = wcloudUrl(r, c); expect = "200"; break;
        case T_WINDY:  url = windyUrl(r, c);  expect = ""; break;
        case T_OSEM:   break;
        case T_CWOP:   break;
        case T_WOW: {
            static char u[512];
            wowUrl(obsFrom(r), c.id, c.key, formatTime("%Y-%m-%d %H:%M:%S", true).c_str(),
                   "universal-weather-node-" FIRMWARE_VERSION, u, sizeof(u));
            url = u;
            expect = "";
            break;
        }
        default:
            url = wuStyleUrl("http://api.wetternetzwerk.pro/weatherstation/updateweatherstation.php", r, c);
            expect = "success";
            break;
    }

    String body;
    int code;
    bool ok;
    if (idx == T_CWOP) {
        code = cwopSend(r, c, body);
        ok = code == 200;
    } else if (idx == T_OSEM) {
        String json = osemBody(r, c);
        code = httpPostJson(String("https://api.opensensemap.org/boxes/") + urlEncode(c.id) + "/data", c.key, json, body);
        body.trim();
        ok = code == 201;
    } else {
        code = httpGet(url, body);
        body.trim();
        ok = code == HTTP_CODE_OK && body.indexOf(expect) >= 0;
    }

    String reply = code > 0 ? body : HTTPClient::errorToString(code);
    if (reply.isEmpty()) reply = "(empty reply)";

    // Never log the URL itself — it carries the station key/password.
    dbgSerial.printf("WX %s: HTTP %d, '%s' -> %s\n",
                     targets[idx].st.name, code, reply.c_str(), ok ? "OK" : "FAILED");

    xSemaphoreTake(wxMutex, portMAX_DELAY);
    WxTargetStatus& st = targets[idx].st;
    st.lastAttemptMs = millis();
    st.lastHttpCode  = code;
    setReply(st, reply.c_str());
    if (ok) { st.okCount++; st.lastOkMs = st.lastAttemptMs; }
    else    { st.failCount++; }
    xSemaphoreGive(wxMutex);
    outReport(OUT_WX_FIRST + idx, ok, code, reply.c_str());
}

void copyCreds(int idx, Creds& c)
{
    const char* id;
    const char* key;
    switch (idx) {
        case T_AWEKAS: id = gCfg.awekasUser;  key = gCfg.awekasPass;   break;
        case T_WU:     id = gCfg.wuStationId; key = gCfg.wuStationKey; break;
        case T_WCLOUD: id = gCfg.wcloudId;    key = gCfg.wcloudKey;    break;
        case T_WNETPRO: id = gCfg.wnetproId;  key = gCfg.wnetproKey;   break;
        case T_WINDY:  id = gCfg.windyId;     key = gCfg.windyPass;    break;
        case T_WOW:    id = gCfg.wowSiteId;   key = gCfg.wowPin;       break;
        case T_CWOP:   id = gCfg.cwopCall;    key = "";                break;
        default:       id = gCfg.osemBoxId;   key = gCfg.osemToken;    break;
    }
    strlcpy(c.map, idx == T_OSEM ? gCfg.osemSensors : "", sizeof(c.map));
    strlcpy(c.id,  id,  sizeof(c.id));
    strlcpy(c.key, key, sizeof(c.key));
    strlcpy(c.lat, gCfg.latitude,  sizeof(c.lat));
    strlcpy(c.lon, gCfg.longitude, sizeof(c.lon));
}

// ---- Firmware update check: a tags (Gitea) or releases (GitHub) list,
// highest version name compared with the running firmware. ----
char          updateLatest[24] = "";        // newer version found, "" = none (guarded by wxMutex)
char          updateStatus[48] = "not checked yet";
unsigned long updateNextMs     = 0;         // 0 = schedule after WiFi is up

void updateCheckPoll()
{
    if (!gCfg.updateCheckEn || !gCfg.updateUrl[0] || !timeIsValid()) return;
    unsigned long now = millis();
    if (updateNextMs == 0) { updateNextMs = now + 120000UL; return; }   // 2 min after start
    if ((long)(now - updateNextMs) < 0) return;
    if (!strncmp(gCfg.updateUrl, "https://", 8) && tlsMayStart("update check") != TLS_GO) {   // tls_gate.h
        updateNextMs = now + 30000UL;
        return;
    }

    char url[sizeof(gCfg.updateUrl)];
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    strlcpy(url, gCfg.updateUrl, sizeof(url));
    xSemaphoreGive(wxMutex);

    // GitHub's releases list carries every release's notes and assets (easily
    // >100 KB): ask for the newest 20 only (2.x among newer 4.x), and parse the reply as it streams
    // in (filtered to the names) instead of buffering it in the heap.
    String u = url;
    if (u.indexOf("api.github.com/") >= 0 && u.indexOf("/releases") >= 0 && u.indexOf("per_page=") < 0)
        u += (u.indexOf('?') >= 0 ? "&per_page=20" : "?per_page=20");

    JsonDocument filter;
    filter[0]["name"] = true;
    filter[0]["tag_name"] = true;
    JsonDocument doc;
    DeserializationError jsonErr = DeserializationError::EmptyInput;
    String body;
    int code = httpGet(u, body, [&](Stream& s) {
        jsonErr = deserializeJson(doc, s, DeserializationOption::Filter(filter));
    });
    char best[24] = "", status[48];
    if (code == HTTP_CODE_OK) {
        // the releases list is an array; an object (e.g. an error document) is no answer
        if (jsonErr == DeserializationError::Ok && doc.is<JsonArray>()) {
            for (JsonObject o : doc.as<JsonArray>()) {
                const char* n = o["tag_name"].is<const char*>() ? o["tag_name"].as<const char*>() : o["name"].as<const char*>();
                // the name is shown on the status page and sent via MQTT: only
                // plain version characters are accepted (wxValidVersionTag)
                if (!n || strlen(n) >= sizeof(best) || !wxValidVersionTag(n)) continue;
                if (versionOffered(n, FIRMWARE_VERSION) && (!best[0] || compareVersions(n, best) > 0)) strlcpy(best, n, sizeof(best));
            }
            snprintf(status, sizeof(status), best[0] ? "newer firmware: %s" : "up to date", best);
        } else {
            strlcpy(status, "unexpected reply", sizeof(status));
        }
        updateNextMs = now + 24UL * 3600000UL;
    } else {
        snprintf(status, sizeof(status), "check failed (HTTP %d)", code);
        updateNextMs = now + 3600000UL;
    }
    dbgSerial.printf("Update check: %s\n", status);
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    strlcpy(updateLatest, best, sizeof(updateLatest));
    strlcpy(updateStatus, status, sizeof(updateStatus));
    xSemaphoreGive(wxMutex);
}

// One notification / custom-upload request (notify.h), plain or TLS. The
// URL may carry a token: never logged.
int notifyHttp(const NotifyReq& req, char* reply, size_t len)
{
    WiFiClient       plain;   // before `http`, see httpPostJson()
    WiFiClientSecure tls;
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    bool begun;
    if (!strncmp(req.url, "https://", 8)) {
        tls.setInsecure();
        tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        begun = http.begin(tls, req.url);
    } else {
        begun = http.begin(plain, req.url);
    }
    if (!begun) { strlcpy(reply, "invalid URL", len); return HTTPC_ERROR_CONNECTION_REFUSED; }
    if (req.contentType[0]) http.addHeader("Content-Type", req.contentType);
    if (req.auth[0]) http.addHeader("Authorization", req.auth);
    const bool isTls = !strncmp(req.url, "https://", 8);
    if (isTls) { tlsBegin(); tlsLogHeap("request", "before"); }
    const int code = req.post ? http.POST((uint8_t*)req.body, strlen(req.body)) : http.GET();
    String b = code > 0 ? readReplyStart(http, len + 16) : HTTPClient::errorToString(code);
    http.end();
    if (isTls) { tlsLogHeap("request", "after"); tlsEnd(); }
    b.trim();
    size_t n = 0;
    for (size_t i = 0; i < b.length() && n + 1 < len; i++) reply[n++] = (b[i] >= 0x20 && b[i] < 0x7F) ? b[i] : ' ';
    reply[n] = '\0';
    return code;
}

// Custom uploads (custom_upload.h): due ones, one per call.
unsigned long customLastMs[CUSTOM_UPLOADS] = {};
void customUploadService()
{
    if (!timeIsValid()) return;
    static PhLive live;
    for (int i = 0; i < CUSTOM_UPLOADS; i++) {
        customUploadLock();
        const bool on = customUploads()[i].url[0] != '\0';
        customUploadUnlock();
        if (!on) { customLastMs[i] = 0; continue; }
        std::unique_ptr<CustomUpload> uBuf(new (std::nothrow) CustomUpload());   // ~0.5 KB while sending
        if (!uBuf) return;
        CustomUpload& u = *uBuf;
        customUploadLock();
        u = customUploads()[i];
        customUploadUnlock();
        if (!u.url[0]) { customLastMs[i] = 0; continue; }
        const unsigned long now = millis();
        if (!customLastMs[i]) { customLastMs[i] = now; continue; }   // first send one interval after enabling / start
        if (now - customLastMs[i] < (unsigned long)u.intervalSec * 1000UL) continue;
        if (!strncmp(u.url, "https://", 8) && tlsMayStart("custom upload") != TLS_GO) {
            customLastMs[i] = now - (unsigned long)u.intervalSec * 1000UL + 10000UL;   // retry in 10 s (tls_gate.h)
            continue;
        }
        customLastMs[i] = now;
        xSemaphoreTake(wxMutex, portMAX_DELAY);
        const WindStats w = wind10;
        xSemaphoreGive(wxMutex);
        phLiveInit(live, &w);
        std::unique_ptr<NotifyReq> reqBuf(new (std::nothrow) NotifyReq());   // ~1 KB while sending
        if (!reqBuf) return;
        NotifyReq& req = *reqBuf;
        bool urlCut = false, bodyCut = false;
        phRender(u.url, req.url, sizeof(req.url), PE_URL, phLiveLookup, &live, &urlCut);
        if (u.post) {
            req.post = true;
            const bool json = u.body[0] == '{' || u.body[0] == '[';
            strcpy(req.contentType, json ? "application/json" : "text/plain");
            phRender(u.body, req.body, sizeof(req.body), json ? PE_JSON : PE_NONE, phLiveLookup, &live, &bodyCut);
        }
        if (urlCut || bodyCut) {   // never send a cut URL / body (e.g. invalid JSON)
            dbgSerial.printf("Custom upload %d: %s too long with the values filled in - not sent\n", i + 1, urlCut ? "URL" : "body");
            outReport(OUT_CUSTOM_FIRST + i, false, 0, urlCut ? "URL too long, not sent" : "body too long, not sent");
            return;
        }
        char reply[48];
        const int code = notifyHttp(req, reply, sizeof(reply));
        const bool ok = code >= 200 && code < 300;
        dbgSerial.printf("Custom upload %d: HTTP %d -> %s\n", i + 1, code, ok ? "OK" : "FAILED");
        outReport(OUT_CUSTOM_FIRST + i, ok, code, reply);
        return;   // one request per loop pass
    }
}

void wxUploadTask(void*)
{
    for (;;) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if (!gCfg.wifiEn || !wifiIsConnected()) continue;
        // Firmware upload in progress (ota_upload.h): no update check,
        // notifications, custom uploads or weather networks meanwhile - the
        // windows keep aggregating and are sent afterwards.
        if (otaUploadActive()) continue;
        updateCheckPoll();
        alarmsNotifyService(notifyHttp);   // one request at a time, never in parallel with the uploads
        customUploadService();

        for (int i = 0; i < (int)WX_TARGET_COUNT; i++) {
            Target& t = targets[i];
            const unsigned long now = millis();

            xSemaphoreTake(wxMutex, portMAX_DELAY);
            bool enabled;
            int  intervalSec;
            targetConfig(i, enabled, intervalSec);
            if (!enabled) {
                t.windowStartMs = 0;
                t.agg = Agg{};
                xSemaphoreGive(wxMutex);
                continue;
            }
            if (t.windowStartMs == 0) t.windowStartMs = now;
            const unsigned long intervalMs = (unsigned long)intervalSec * 1000UL;
            if (now - t.windowStartMs < intervalMs) {
                xSemaphoreGive(wxMutex);
                continue;
            }
            // Normally sent within seconds of the interval ending. Much older
            // means WiFi or the clock was gone for a while (this loop skips
            // everything without WiFi, pushCycle keeps aggregating): start a
            // fresh window rather than upload an hours-long average as current.
            if (now - t.windowStartMs >= 2 * intervalMs) {
                t.agg = Agg{};
                t.windowStartMs = now;
                setReply(t.st, "window restarted after an outage");
                xSemaphoreGive(wxMutex);
                continue;
            }
            // Local date/time is part of every request and the daily rain
            // total resets at local midnight — keep aggregating until the
            // clock is known instead of sending wrong timestamps.
            if (!timeIsValid()) {
                setReply(t.st, "waiting for NTP time sync");
                xSemaphoreGive(wxMutex);
                continue;
            }
            if (aggEmpty(t.agg)) {
                t.windowStartMs = now;
                setReply(t.st, "no sensor data in this window");
                xSemaphoreGive(wxMutex);
                continue;
            }
            // HTTPS networks (WU, Windy, openSenseMap): only with enough memory and no heavy
            // web page in progress (tls_gate.h) - the window keeps aggregating meanwhile
            if ((i == T_WU || i == T_WINDY || i == T_OSEM) && tlsMayStart(t.st.name) != TLS_GO) {
                setReply(t.st, "waiting for free memory / web page");
                xSemaphoreGive(wxMutex);
                continue;
            }
            rollDailyRain();
            Reading r = readingFrom(t.agg, rtcRain.mm, srcActive(SG_RAIN));
            t.agg = Agg{};
            t.windowStartMs = now;
            Creds c;
            copyCreds(i, c);
            xSemaphoreGive(wxMutex);

            // values outside the aggregation (own locks): rolling rain, UV, light
            if (r.hasRain) r.rain1hMm = historyRainSum(3600);
            if (gSrc.uvRadio) { r.hasUv = true; r.uvIndex = gSrc.uvIndexRadio; }
            else if (gSrc.uvBoard && gIo.uvIndexCalibrated && isfinite(gIo.uvIndex)) { r.hasUv = true; r.uvIndex = gIo.uvIndex; }
            if (gSrc.lightValid) { r.hasLux = true; r.lux = gSrc.lightLux; }
            {
                int li = srcResolve(SG_LIGHT);   // Davis solar radiation sensor: the measured W/m2
                if (li >= 0 && gRfSensors[li].hasSolar) { r.hasSolar = true; r.solarWm2 = gRfSensors[li].solarWm2; }
            }
            if (gSrc.lightValid) {               // sunshine is only counted with a light source
                WxDailyStats ds;
                wxDailyStats(ds);
                if (ds.valid) { r.hasSunshine = true; r.sunshineHours = ds.sunTodaySec / 3600.0f; }
            }
            float p3h;
            if (gSrc.pressValid && historyPressureAgo(3 * 3600, p3h)) {
                r.hasTendency = true;
                r.pressDelta3hHpa = gSrc.pressSeaHpa - p3h;
            }

            sendOne(i, r, c);
        }
    }
}

} // namespace

void wxUploadSetup()
{
    // RTC RAM if valid (restart, watchdog, OTA keeping the layout), else -
    // after a power loss or a moved RTC layout - the NVS copy (only with
    // rainPersist on; off = start at 0, the copy is not read). Daily values,
    // pending rain and week/month/year are taken together from one source,
    // so nothing is counted twice. The roll-over to the current date happens
    // once the clock is known (rollDailyRain()).
    RainRecord nvs = {};
    int nvsVersion = 0;
    const bool nvsOk = gCfg.rainPersist && loadRainNvs(nvs, nvsVersion);
    const bool dailyOk = dailyRainOk(), pendingOk = pendingRainOk(), periodsOk = periodRainOk();
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    if (dailyOk && pendingOk && periodsOk) {
        dbgSerial.printf("WX: daily rain total restored from RTC RAM: %.2f mm (date %ld)\n",
                         rtcRain.mm, (long)rtcRain.ymd);
    } else if (nvsOk) {
        rainRecordStore(nvs);
        dbgSerial.printf("WX: rain restored from flash%s: today %.2f mm (date %ld), yesterday %.2f mm, "
                         "week/month/year %.1f / %.1f / %.1f mm\n",
                         nvsVersion == 1 ? " (old format: week/month/year only)" : "",
                         nvs.d.mm, (long)nvs.d.ymd, nvs.d.prevMm, nvs.p.weekMm, nvs.p.monthMm, nvs.p.yearMm);
    } else {
        // reset only the invalid parts
        RainRecord r = rainRecordNow();
        if (!dailyOk)   r.d = RainDaily{};
        if (!periodsOk) r.p = RainPeriods{};
        if (!pendingOk) r.pendingMm = r.pendingSunSec = 0.0f;
        rainRecordStore(r);
    }
    xSemaphoreGive(wxMutex);
    xSemaphoreTake(rainNvsMutex, portMAX_DELAY);
    rainSave = RainSaveState{};
    rainSave.saved = nvsOk;         // no copy yet: the first change is written right away
    if (nvsOk) rainSave.last = nvs;
    rainSave.lastSaveSec = uptimeSec();
    rainReady = true;
    xSemaphoreGive(rainNvsMutex);

    // 12 KB: mbedTLS's handshake (Weather Underground is HTTPS-only) needs
    // noticeably more stack than plain HTTP. Core 0, same as the web server
    // task — keeps network work off the core running the sampling loop.
    xTaskCreatePinnedToCore(wxUploadTask, "wxUpload", 12288, nullptr, 1, nullptr, 0);
}

void wxUploadPushCycle(const WindStats& w10)
{
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    wind10 = w10;

    for (size_t i = 0; i < WX_TARGET_COUNT; i++) {
        Target& t = targets[i];
        // Only aggregate while a window is actually running, so a network
        // enabled later doesn't send hours of stale data in its first upload.
        if (t.windowStartMs == 0) continue;
        Agg& a = t.agg;
        if (srcActive(SG_WIND)) {
            a.gustMax  = a.windCount ? max(a.gustMax, windGustMs) : windGustMs;
            a.windSum += windSpeedMs;
            float rad  = windDirection * DEG_TO_RAD;
            a.dirSin  += sinf(rad);
            a.dirCos  += cosf(rad);
            a.windCount++;
        }
        if (srcActive(SG_RAIN)) {
            a.rainRateSum += rainRateMmH;
            a.rainCount++;
        }
        // temperature/humidity and pressure (QNH, like the bridge) from the
        // selected sources
        if (gSrc.tempValid) {
            a.tempSum += gSrc.tempC;
            a.bmeCount++;
            if (gSrc.humValid) { a.humSum += gSrc.humidity; a.humCount++; }
        }
        if (gSrc.pressValid) { a.pressSum += gSrc.pressSeaHpa; a.pressCount++; }
    }

    const bool clock = rollDailyRain();
    if (srcActive(SG_RAIN) && rainMM > 0.0f) {
        RainRecord r = rainRecordNow();
        rainRecordAddRain(r, rainMM, clock, (int64_t)time(nullptr));   // clock unknown: pending
        rainRecordStore(r);
    }
    xSemaphoreGive(wxMutex);

    // Flash copy: at most hourly and only when changed, plus at the day
    // roll-over (rain_stats.h). Written outside wxMutex.
    if (!clock) return;
    xSemaphoreTake(rainNvsMutex, portMAX_DELAY);
    if (!rainReady) { xSemaphoreGive(rainNvsMutex); return; }
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    const RainRecord toSave = rainRecordNow();
    const int32_t ymd = rtcRain.ymd;
    xSemaphoreGive(wxMutex);
    if (rainSaveDue(rainSave, toSave, uptimeSec(), ymd, gCfg.rainPersist) && !writeRainNvs(toSave)) {
        rainSaveFailed(rainSave);
        dbgSerial.println("WX: FAILED to save rain statistics to flash (NVS full?), retry in 1 h.");
    }
    xSemaphoreGive(rainNvsMutex);
}

// Settings -> System: one part of the rain / sunshine record back to 0, the
// flash copy updated right away (rain_stats.h). The rain reset also sets
// "rain since start" to 0; each keeps the other's values.
static void rainRecordReset(bool rain)
{
    xSemaphoreTake(rainNvsMutex, portMAX_DELAY);
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    RainRecord r = rainRecordNow();
    if (rain) {
        r.d.mm = r.d.prevMm = 0.0f;
        r.d.lastRainEpoch = 0;
        r.p.weekMm = r.p.monthMm = r.p.yearMm = 0.0f;
        r.pendingMm = 0.0f;
    } else {
        r.d.sunSec = r.d.prevSunSec = 0.0f;
        r.pendingSunSec = 0.0f;
    }
    rainRecordStore(r);
    rollDailyRain();
    const RainRecord cur = rainRecordNow();
    xSemaphoreGive(wxMutex);
    if (rainReady && gCfg.rainPersist && writeRainNvs(cur)) {
        rainSave.saved = true;
        rainSave.last = cur;
        rainSave.lastSaveSec = uptimeSec();
    }
    xSemaphoreGive(rainNvsMutex);
    if (rain) resetRainAccumulator();   // "rain since start"; the per-path windows keep counting
    dbgSerial.printf("WX: %s statistics reset (settings).\n", rain ? "rain" : "sunshine");
}

void rainStatsReset()     { rainRecordReset(true); }
void sunshineStatsReset() { rainRecordReset(false); }

void rainStatsFlush()
{
    xSemaphoreTake(rainNvsMutex, portMAX_DELAY);
    if (rainReady) {
        xSemaphoreTake(wxMutex, portMAX_DELAY);
        rollDailyRain();   // clock known: pending rain into today first
        const RainRecord cur = rainRecordNow();
        xSemaphoreGive(wxMutex);
        if (rainFlushDue(rainSave, cur, uptimeSec(), gCfg.rainPersist)) {
            if (writeRainNvs(cur)) {
                dbgSerial.printf("WX: rain statistics saved to flash before restart (today %.2f mm).\n", cur.d.mm);
            } else {
                rainSaveFailed(rainSave);
                dbgSerial.println("WX: FAILED to save rain statistics to flash before restart.");
            }
        }
    }
    xSemaphoreGive(rainNvsMutex);
}

void systemRestart(const char* reason)
{
    dbgSerial.printf("Restart: %s\n", reason ? reason : "");
    rainStatsFlush();
    recordsFlush();
    historyStoreFlush();
    delay(100);   // let the log line out
    ESP.restart();
}

void wxUploadLock()   { xSemaphoreTake(wxMutex, portMAX_DELAY); }
void wxUploadUnlock() { xSemaphoreGive(wxMutex); }

void wxUploadStatus(WxTargetStatus out[WX_TARGET_COUNT])
{
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    for (size_t i = 0; i < WX_TARGET_COUNT; i++) {
        out[i] = targets[i].st;
        targetConfig((int)i, out[i].enabled, out[i].intervalSec);
    }
    xSemaphoreGive(wxMutex);
}

float wxDailyRainMm(bool& valid)
{
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    valid = rollDailyRain() && rtcRain.ymd != 0;
    float mm = valid ? rtcRain.mm : rtcRain.mm + rtcPending.mm;
    xSemaphoreGive(wxMutex);
    return mm;
}

void wxDailyStats(WxDailyStats& out)
{
    out = WxDailyStats{};
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    out.valid      = rollDailyRain() && rtcRain.ymd != 0;
    out.rainTodayMm = rtcRain.mm;
    out.sunTodaySec = rtcRain.sunSec;
    out.yesterdayValid   = prevIsYesterday();
    out.rainYesterdayMm  = rtcRain.prevMm;
    out.sunYesterdaySec  = rtcRain.prevSunSec;
    out.lastRainEpoch    = rtcRain.lastRainEpoch;
    out.rainWeekMm       = rtcPeriods.p.weekMm;
    out.rainMonthMm      = rtcPeriods.p.monthMm;
    out.rainYearMm       = rtcPeriods.p.yearMm;
    xSemaphoreGive(wxMutex);
}

void wxAddSunshine(float sec)
{
    if (sec <= 0.0f) return;
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    const bool clock = rollDailyRain();
    RainRecord r = rainRecordNow();
    rainRecordAddSun(r, sec, clock);
    rainRecordStore(r);
    xSemaphoreGive(wxMutex);
}

void wxUpdateAvailable(char* out, size_t len)
{
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    strlcpy(out, updateLatest, len);
    xSemaphoreGive(wxMutex);
}

void wxUpdateStatus(char* out, size_t len)
{
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    strlcpy(out, updateStatus, len);
    xSemaphoreGive(wxMutex);
}
