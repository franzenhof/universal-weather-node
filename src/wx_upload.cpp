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
#include <ArduinoJson.h>

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <MD5Builder.h>
#include <esp_attr.h>
#include <stddef.h>

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
    int   dirDeg = 0;
    float rainRateMmH = 0, dailyRainMm = 0;
    float tempC = 0, humidity = 0, pressureHpa = 0;
    // filled in the upload task right before sending (outside wxMutex)
    float rain1hMm = 0;
    bool  hasUv = false, hasLux = false;
    float uvIndex = 0, lux = 0;
    float windMs() const { return windKmh / MS_TO_KMH; }
};

struct Target {
    WxTargetStatus st;
    Agg            agg;
    unsigned long  windowStartMs;   // 0 = window not started yet (static storage: zero-initialized)
};

enum TargetIdx { T_AWEKAS = 0, T_WU = 1, T_WCLOUD = 2, T_WNETPRO = 3, T_WINDY = 4, T_OSEM = 5 };

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
};

// Guards targets[] (aggregators + status), rtcRain and the upload-related
// Config fields — see wxUploadLock() in the header. pushCycle runs on the
// main loop task, the upload loop on its own task, the status page on the
// web server task.
SemaphoreHandle_t wxMutex = xSemaphoreCreateMutex();

// Rain since local midnight. RTC_NOINIT_ATTR: lives in RTC RAM, which
// survives a software restart/watchdog reset/OTA (unlike normal RAM), but
// not a power cycle — so a reboot mid-day doesn't drop the daily total
// back to 0 for the rest of the day, without writing anything to flash.
// The bridge solves the same problem by re-reading its database on start.
// Also yesterday's totals, the time of the last rain and the sunshine
// duration (wireless light sensor) - the same daily bookkeeping.
struct DailyRain {
    uint32_t magic;
    int32_t  ymd;          // local date (yyyymmdd) the totals belong to; 0 = not known yet
    float    mm;           // rain today
    int32_t  prevYmd;      // date of the "yesterday" values (only shown if it really was yesterday)
    float    prevMm;       // rain yesterday
    float    sunSec;       // sunshine today, s
    float    prevSunSec;   // sunshine yesterday, s
    int64_t  lastRainEpoch;   // UTC time of the last rain increment, 0 = unknown
    uint32_t check;
};
constexpr uint32_t DAILY_RAIN_MAGIC = 0x52414E32;  // "RAN2"
RTC_NOINIT_ATTR DailyRain rtcRain;

uint32_t dailyRainCheck(const DailyRain& r)
{
    // FNV-1a over everything but the check word itself.
    const uint8_t* p = (const uint8_t*)&r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(DailyRain, check); i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void sealDailyRain() { rtcRain.check = dailyRainCheck(rtcRain); }

int32_t ymdOf(const struct tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

// Caller holds wxMutex. Moves today's totals to "yesterday" once the local
// date has moved on from the one they were collected on. Totals collected
// before NTP ever synced (ymd 0) are adopted as today's — at worst they're
// off by whatever fell between midnight and the first sync after a reboot.
void rollDailyRain()
{
    struct tm lt;
    if (!localTimeNow(lt)) return;
    int32_t ymd = ymdOf(lt);
    if (rtcRain.ymd == ymd) return;
    if (rtcRain.ymd != 0) {
        rtcRain.prevYmd    = rtcRain.ymd;
        rtcRain.prevMm     = rtcRain.mm;
        rtcRain.prevSunSec = rtcRain.sunSec;
        rtcRain.mm     = 0.0f;
        rtcRain.sunSec = 0.0f;
    }
    rtcRain.ymd = ymd;
    sealDailyRain();
}

// Caller holds wxMutex: true if the stored "yesterday" date is the day before today.
bool prevIsYesterday()
{
    struct tm lt;
    if (!localTimeNow(lt) || rtcRain.prevYmd == 0) return false;
    lt.tm_mday -= 1;
    lt.tm_hour = 12;   // away from DST edges
    mktime(&lt);
    return ymdOf(lt) == rtcRain.prevYmd;
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
        r.dirDeg  = wind10.dirValid ? wind10.dirDeg : 0;
    } else if (a.windCount) {
        r.hasWind = true;
        r.windKmh = a.windSum / a.windCount * MS_TO_KMH;
        r.gustKmh = a.gustMax * MS_TO_KMH;
        int deg   = (int)roundf(atan2f(a.dirSin / a.windCount, a.dirCos / a.windCount) * RAD_TO_DEG);
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
    else     localtime_r(&now, &t);
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

    String v;
    v.reserve(200);
    // Appends one field plus its separator; an absent value stays empty.
    auto field = [&v](bool present, const String& value) {
        if (present) v += value;
        v += ';';
    };
    field(true,      c.id);
    field(true,      md5.toString());
    field(true,      formatTime("%d.%m.%Y", false));
    field(true,      formatTime("%H:%M", false));
    field(r.hasBme,  num(r.tempC, 1));
    field(r.hasHum,  num(r.humidity, 0));
    field(r.hasPress, num(r.pressureHpa, 1));
    field(r.hasRain, num(r.dailyRainMm, 2));
    field(r.hasWind, num(r.windKmh, 1));
    field(r.hasWind, String(r.dirDeg));
    v += ";;;";                                // condition, warning text, snow depth
    field(true,      "en");                    // language
    v += ';';                                  // pressure tendency
    field(r.hasWind, num(r.gustKmh, 1));
    v += ";;;;;";                              // solar, UV, brightness, sunshine, soil temp
    field(r.hasRain, num(r.rainRateMmH, 1));
    field(true,      "weather-node-" FIRMWARE_VERSION);
    field(true,      c.lon);
    v += c.lat;
    return "http://data.awekas.at/eingabe_pruefung.php?val=" + urlEncode(v.c_str());
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
        u += "&winddir=";      u += String(r.dirDeg);
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
        u += "&wdir=";   u += String(r.dirDeg);
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
        u += "&winddir="; u += String(r.dirDeg);
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
        else if (k == "dir"     && r.hasWind)  v = String(r.dirDeg);
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

int httpPostJson(const String& url, const String& auth, const String& json, String& body)
{
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
    if (!http.begin(tls, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
    http.addHeader("Content-Type", "application/json");
    if (auth.length()) http.addHeader("Authorization", auth);
    int code = http.POST(json);
    if (code > 0) body = http.getString();
    http.end();
    return code;
}

// Plain or TLS GET depending on the scheme. TLS is unverified (setInsecure)
// — same tradeoff as MQTT_TLS_INSECURE: encrypted, not authenticated; no CA
// bundle to keep up to date on the device for a handful of upload hosts.
int httpGet(const String& url, String& body)
{
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);

    WiFiClient       plain;
    WiFiClientSecure tls;
    bool begun;
    if (url.startsWith("https://")) {
        tls.setInsecure();
        tls.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        begun = http.begin(tls, url);
    } else {
        begun = http.begin(plain, url);
    }
    if (!begun) return HTTPC_ERROR_CONNECTION_REFUSED;

    int code = http.GET();
    if (code > 0) body = http.getString();
    http.end();
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
        default:
            url = wuStyleUrl("http://api.wetternetzwerk.pro/weatherstation/updateweatherstation.php", r, c);
            expect = "success";
            break;
    }

    String body;
    int code;
    bool ok;
    if (idx == T_OSEM) {
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

    char url[sizeof(gCfg.updateUrl)];
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    strlcpy(url, gCfg.updateUrl, sizeof(url));
    xSemaphoreGive(wxMutex);

    String body;
    int code = httpGet(url, body);
    char best[24] = "", status[48];
    if (code == HTTP_CODE_OK) {
        JsonDocument filter;
        filter[0]["name"] = true;
        filter[0]["tag_name"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
            for (JsonObject o : doc.as<JsonArray>()) {
                const char* n = o["tag_name"].is<const char*>() ? o["tag_name"].as<const char*>() : o["name"].as<const char*>();
                if (!n || strlen(n) >= sizeof(best)) continue;
                if (compareVersions(n, FIRMWARE_VERSION) > 0 && (!best[0] || compareVersions(n, best) > 0)) strlcpy(best, n, sizeof(best));
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

void wxUploadTask(void*)
{
    for (;;) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if (!gCfg.wifiEn || !wifiIsConnected()) continue;
        updateCheckPoll();

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
            if (now - t.windowStartMs < (unsigned long)intervalSec * 1000UL) {
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

            sendOne(i, r, c);
        }
    }
}

} // namespace

void wxUploadSetup()
{
    if (rtcRain.magic != DAILY_RAIN_MAGIC || rtcRain.check != dailyRainCheck(rtcRain)) {
        rtcRain = DailyRain{};
        rtcRain.magic = DAILY_RAIN_MAGIC;
        sealDailyRain();
    } else {
        dbgSerial.printf("WX: daily rain total restored from RTC RAM: %.2f mm (date %ld)\n",
                         rtcRain.mm, (long)rtcRain.ymd);
    }

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
        // temperature/humidity from the selected source; pressure (QNH, like the
        // bridge) only from the BME280
        if (gSrc.tempValid) {
            a.tempSum += gSrc.tempC;
            a.bmeCount++;
            if (gSrc.humValid) { a.humSum += gSrc.humidity; a.humCount++; }
        }
        if (bme280Present) { a.pressSum += bme280PressSea; a.pressCount++; }
    }

    if (srcActive(SG_RAIN)) {
        rollDailyRain();
        rtcRain.mm += rainMM;
        if (rainMM > 0.0f && timeIsValid()) rtcRain.lastRainEpoch = (int64_t)time(nullptr);
        sealDailyRain();
    }

    xSemaphoreGive(wxMutex);
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
    rollDailyRain();
    valid = rtcRain.ymd != 0;
    float mm = rtcRain.mm;
    xSemaphoreGive(wxMutex);
    return mm;
}

void wxDailyStats(WxDailyStats& out)
{
    out = WxDailyStats{};
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    rollDailyRain();
    out.valid      = rtcRain.ymd != 0;
    out.rainTodayMm = rtcRain.mm;
    out.sunTodaySec = rtcRain.sunSec;
    out.yesterdayValid   = prevIsYesterday();
    out.rainYesterdayMm  = rtcRain.prevMm;
    out.sunYesterdaySec  = rtcRain.prevSunSec;
    out.lastRainEpoch    = rtcRain.lastRainEpoch;
    xSemaphoreGive(wxMutex);
}

void wxAddSunshine(float sec)
{
    if (sec <= 0.0f) return;
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    rollDailyRain();
    rtcRain.sunSec += sec;
    sealDailyRain();
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
