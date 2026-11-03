#include "wx_upload.h"
#include "globals.h"
#include "time_sync.h"
#include "wifi_mqtt.h"
#include "generated_version.h"
#include "serial_log.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <MD5Builder.h>
#include <esp_attr.h>

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
    uint16_t windCount = 0, rainCount = 0, bmeCount = 0;
    float    windSum = 0, gustMax = 0, dirSin = 0, dirCos = 0;
    float    rainRateSum = 0;
    float    tempSum = 0, humSum = 0, pressSum = 0;
};

// Aggregated values in the units the networks are built around (metric).
// A field is only valid if its group's count was non-zero.
struct Reading {
    bool  hasWind = false, hasRain = false, hasBme = false;
    float windKmh = 0, gustKmh = 0;
    int   dirDeg = 0;
    float rainRateMmH = 0, dailyRainMm = 0;
    float tempC = 0, humidity = 0, pressureHpa = 0;
};

struct Target {
    WxTargetStatus st;
    Agg            agg;
    unsigned long  windowStartMs;   // 0 = window not started yet (static storage: zero-initialized)
};

enum TargetIdx { T_AWEKAS = 0, T_WU = 1, T_WCLOUD = 2, T_WNETPRO = 3 };

Target targets[WX_TARGET_COUNT] = {
    { { "AWEKAS" } },
    { { "Weather Underground" } },
    { { "Weathercloud" } },
    { { "Wetternetzwerk.pro" } },
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
struct DailyRain {
    uint32_t magic;
    int32_t  ymd;     // local date (yyyymmdd) the total belongs to; 0 = not known yet
    float    mm;
    uint32_t check;
};
constexpr uint32_t DAILY_RAIN_MAGIC = 0x52414E31;  // "RAN1"
RTC_NOINIT_ATTR DailyRain rtcRain;

uint32_t dailyRainCheck(const DailyRain& r)
{
    uint32_t mmBits;
    memcpy(&mmBits, &r.mm, sizeof(mmBits));
    return r.magic ^ (uint32_t)r.ymd ^ mmBits ^ 0xA5A5A5A5u;
}

void sealDailyRain() { rtcRain.check = dailyRainCheck(rtcRain); }

// Caller holds wxMutex. Resets the total once the local date has moved on
// from the one it was collected on. A total collected before NTP ever
// synced (ymd 0) is adopted as today's — at worst it's off by whatever fell
// between midnight and the first sync after a reboot.
void rollDailyRain()
{
    struct tm lt;
    if (!localTimeNow(lt)) return;
    int32_t ymd = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
    if (rtcRain.ymd == ymd) return;
    if (rtcRain.ymd != 0) rtcRain.mm = 0.0f;
    rtcRain.ymd = ymd;
    sealDailyRain();
}

// Caller holds wxMutex.
void targetConfig(int idx, bool& enabled, int& intervalSec)
{
    switch (idx) {
        case T_AWEKAS:  enabled = gCfg.awekasUser[0];  intervalSec = gCfg.awekasIntervalSec;  break;
        case T_WU:      enabled = gCfg.wuStationId[0]; intervalSec = gCfg.wuIntervalSec;      break;
        case T_WCLOUD:  enabled = gCfg.wcloudId[0];    intervalSec = gCfg.wcloudIntervalSec;  break;
        default:        enabled = gCfg.wnetproId[0];   intervalSec = gCfg.wnetproIntervalSec; break;
    }
    if (!gCfg.wifiEn) enabled = false;
}

bool aggEmpty(const Agg& a) { return !(a.windCount || a.rainCount || a.bmeCount); }

Reading readingFrom(const Agg& a, float dailyRainMm, bool rainEn)
{
    Reading r;
    if (a.windCount) {
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
    if (a.bmeCount) {
        r.hasBme      = true;
        r.tempC       = a.tempSum / a.bmeCount;
        r.humidity    = a.humSum / a.bmeCount;
        r.pressureHpa = a.pressSum / a.bmeCount;
    }
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
    char key[33];
    char lat[12];
    char lon[12];
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
    field(r.hasBme,  num(r.humidity, 0));
    field(r.hasBme,  num(r.pressureHpa, 1));
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
    if (r.hasBme) {
        u += "&tempf=";    u += num(r.tempC * 1.8f + 32.0f, 1);
        u += "&humidity="; u += num(r.humidity, 0);
        u += "&baromin=";  u += num(r.pressureHpa / INHG_TO_HPA, 2);
    }
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
    if (r.hasBme) {
        u += "&temp="; u += x10(r.tempC);
        u += "&hum=";  u += num(r.humidity, 0);
        u += "&bar=";  u += x10(r.pressureHpa);
    }
    if (r.hasWind) {
        u += "&wspd=";   u += x10(r.windKmh / MS_TO_KMH);
        u += "&wspdhi="; u += x10(r.gustKmh / MS_TO_KMH);
        u += "&wdir=";   u += String(r.dirDeg);
    }
    return u;
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
        default:
            url = wuStyleUrl("http://api.wetternetzwerk.pro/weatherstation/updateweatherstation.php", r, c);
            expect = "success";
            break;
    }

    String body;
    int code = httpGet(url, body);
    body.trim();
    bool ok = code == HTTP_CODE_OK && body.indexOf(expect) >= 0;

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
        default:       id = gCfg.wnetproId;   key = gCfg.wnetproKey;   break;
    }
    strlcpy(c.id,  id,  sizeof(c.id));
    strlcpy(c.key, key, sizeof(c.key));
    strlcpy(c.lat, gCfg.latitude,  sizeof(c.lat));
    strlcpy(c.lon, gCfg.longitude, sizeof(c.lon));
}

void wxUploadTask(void*)
{
    for (;;) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if (!gCfg.wifiEn || !wifiIsConnected()) continue;

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
            Reading r = readingFrom(t.agg, rtcRain.mm, gCfg.rainEn);
            t.agg = Agg{};
            t.windowStartMs = now;
            Creds c;
            copyCreds(i, c);
            xSemaphoreGive(wxMutex);

            sendOne(i, r, c);
        }
    }
}

} // namespace

void wxUploadSetup()
{
    if (rtcRain.magic != DAILY_RAIN_MAGIC || rtcRain.check != dailyRainCheck(rtcRain)) {
        rtcRain = DailyRain{ DAILY_RAIN_MAGIC, 0, 0.0f, 0 };
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

void wxUploadPushCycle()
{
    xSemaphoreTake(wxMutex, portMAX_DELAY);

    for (size_t i = 0; i < WX_TARGET_COUNT; i++) {
        Target& t = targets[i];
        // Only aggregate while a window is actually running, so a network
        // enabled later doesn't send hours of stale data in its first upload.
        if (t.windowStartMs == 0) continue;
        Agg& a = t.agg;
        if (gCfg.windEn) {
            a.gustMax  = a.windCount ? max(a.gustMax, windGustMs) : windGustMs;
            a.windSum += windSpeedMs;
            float rad  = windDirection * DEG_TO_RAD;
            a.dirSin  += sinf(rad);
            a.dirCos  += cosf(rad);
            a.windCount++;
        }
        if (gCfg.rainEn) {
            a.rainRateSum += rainRateMmH;
            a.rainCount++;
        }
        if (bme280Present) {
            a.tempSum  += bme280TempC;
            a.humSum   += bme280Humidity;
            a.pressSum += bme280PressSea;   // QNH, like the bridge
            a.bmeCount++;
        }
    }

    if (gCfg.rainEn) {
        rollDailyRain();
        rtcRain.mm += rainMM;
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
