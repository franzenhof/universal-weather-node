#include "web_api.h"
#include "system_restart.h"
#include "globals.h"
#include "params.h"
#include "board_io.h"
#include "heltec_board.h"
#include "sources.h"
#include "lora_payload.h"
#include "json_payload.h"
#include "history.h"
#include "wifi_mqtt.h"
#include "readme_md.h"
#include "help_page.h"
#include "log_page.h"
#include "chart_page.h"
#include "generated_version.h"
#include "serial_log.h"
#include "units.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "lora_transport.h"
#include "wind_window.h"
#include "rf_sensors.h"
#include "derived_now.h"

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "loxone.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

WebServer server(80);
bool      serverStarted = false;

// Guards `server` (begin/stop/handleClient) and `serverStarted` themselves —
// separate from history.cpp's own mutex, which guards different data (the
// ring buffer). server.handleClient() now runs continuously from its own
// FreeRTOS task (webApiTask, pinned to core 0, see webApiSetup() below)
// instead of once per main-loop() iteration, so it no longer sits behind
// sendLoRa()'s blocking airtime — but webApiStop() (main.cpp, called from
// the main loop task right before opening the WiFiManager config portal)
// can now run concurrently with that task's own begin/stop/handleClient
// cycle, and WiFiServer's underlying socket isn't documented as safe to
// stop() from one task while another is mid-accept()/handleClient() on it.
SemaphoreHandle_t serverMutex = xSemaphoreCreateMutex();
TaskHandle_t      webApiTaskHandle = nullptr;
// Set by webApiStop() under serverMutex. webApiTask checks it before every
// begin(): between webApiStop() releasing the mutex and suspending the task,
// a task already waiting on the mutex would otherwise restart the server.
bool              apiStopped = false;

// Streams a JsonDocument to the client as chunked transfer-encoding, one
// bounded buffer at a time. Used by handleApiHistory() below instead of
// measureJson()+serializeJson() (which walked the whole document twice —
// once just to precompute Content-Length — doubling ArduinoJson's traversal
// cost on every poll). Reuses WebServer's own chunk framing: passing
// CONTENT_LENGTH_UNKNOWN to setContentLength() before send() makes
// WebServer::_prepareHeader() switch to "Transfer-Encoding: chunked" and
// sendContent() add the size-prefixed framing per call; _finalizeResponse()
// appends the terminating zero-length chunk automatically once the handler
// returns. Same Print-subclass pattern as SerialLog (serial_log.h).
class ChunkedPrint : public Print
{
public:
    explicit ChunkedPrint(WebServer& srv) : server_(srv) {}

    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t* data, size_t len) override
    {
        size_t remaining = len;
        while (remaining) {
            size_t space = sizeof(buf_) - fill_;
            size_t n     = remaining < space ? remaining : space;
            memcpy(buf_ + fill_, data, n);
            fill_ += n;
            data  += n;
            remaining -= n;
            if (fill_ == sizeof(buf_)) flushChunk();
        }
        return len;
    }

    void flush() override { flushChunk(); }

private:
    void flushChunk()
    {
        if (fill_) {
            server_.sendContent(buf_, fill_);
            fill_ = 0;
        }
    }

    WebServer& server_;
    char       buf_[512];
    size_t     fill_ = 0;
};

void handleApiData()
{
    // static: retains its underlying buffer across requests instead of
    // growing it from scratch and freeing it every call — safe because
    // WebServer::handleClient() only ever processes one request at a time on
    // this one task (see ChunkedPrint's comment above); buildJsonPayload()
    // clears the content (not the buffer) at its own top (see buildJsonPayload()).
    static JsonDocument doc;
    buildJsonPayload(doc);
    if (doc.overflowed()) {
        dbgSerial.println("API telemetry: JSON allocation failed.");
        server.send(500, "text/plain", "Telemetry allocation failed");
        return;
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    ChunkedPrint out(server);
    serializeJson(doc, out);
    out.flush();
}

// Rendered README help page, works fully offline (no CDN dependency) — see
// include/help_page.h. Same route pair as the WiFiManager portal's /help.
void handleHelp()
{
    server.send_P(200, "text/html; charset=utf-8", HELP_PAGE_HTML);
}

void handleHelpMd()
{
    server.send_P(200, "text/plain; charset=utf-8", README_MD);
}

// ---------------------------------------------------------------------------
// Shared look of the server-rendered pages (/status, /api/settings): same
// CSS custom properties/palette as the dashboard (chart_page.h), so all
// pages read as one UI. The one-line script only applies the theme stored
// by the dashboard's toggle — both pages work unchanged without JS.
// ---------------------------------------------------------------------------
const char PAGE_CSS[] PROGMEM = R"CSS(<style>
:root{--bg:#f4f6f8;--panel:#fff;--border:#dde3e9;--text:#1a2028;--dim:#62707c;--accent:#0f9d94;--ok:#2e9d57;--warn:#c98a12;--bad:#d0443a;}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#12161b;--panel:#1a2028;--border:#2a323c;--text:#e7ecf1;--dim:#8996a3;--accent:#3fd0c9;--ok:#4cc27a;--warn:#e0a63e;--bad:#ef6a5f;}}
:root[data-theme="dark"]{--bg:#12161b;--panel:#1a2028;--border:#2a323c;--text:#e7ecf1;--dim:#8996a3;--accent:#3fd0c9;--ok:#4cc27a;--warn:#e0a63e;--bad:#ef6a5f;}
*{box-sizing:border-box;}
body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);max-width:1000px;margin:0 auto;padding:0 1rem 2rem;}
nav{display:flex;flex-wrap:wrap;gap:1.1rem;padding:0.9rem 0;border-bottom:1px solid var(--border);margin-bottom:1rem;}
nav a{color:var(--dim);text-decoration:none;font-size:0.9rem;}
nav a:hover,nav a:focus,nav a.on{color:var(--accent);}
h3{margin:0 0 0.25rem;}
.sub{color:var(--dim);font-size:0.85rem;margin:0 0 1rem;}
section{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:0.75rem 0.9rem;margin-bottom:0.9rem;}
h4{margin:0 0 0.5rem;font-size:0.85rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
table{width:100%;border-collapse:collapse;}
td{padding:0.4rem 0.25rem;border-top:1px solid var(--border);vertical-align:top;}
tr:first-child td{border-top:0;}
td:first-child{color:var(--dim);width:38%;}
.num{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;}
.dot{display:inline-block;width:0.6rem;height:0.6rem;border-radius:50%;margin-right:0.45rem;}
.ok{background:var(--ok);}.warn{background:var(--warn);}.bad{background:var(--bad);}.off{background:var(--border);}
.dim,small{color:var(--dim);font-size:0.8rem;}
.tag{font-size:0.7rem;border-radius:4px;padding:0.05rem 0.35rem;margin-left:0.4rem;text-transform:none;letter-spacing:0;border:1px solid currentColor;}
.tag.live{color:var(--ok);}.tag.restart{color:var(--warn);}
.banner{border-radius:8px;padding:0.6rem 0.8rem;margin-bottom:0.9rem;border:1px solid;}
.banner.okb{border-color:var(--ok);}.banner.badb{border-color:var(--bad);}
label{display:grid;grid-template-columns:minmax(150px,38%) 1fr;gap:0.6rem;align-items:center;padding:0.35rem 0;border-top:1px solid var(--border);}
section>label:first-of-type,section>h4+label{border-top:0;}
label small{display:block;}
input,select,textarea{font:inherit;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:0.35rem 0.5rem;width:100%;}
input[type=checkbox]{width:1.1rem;height:1.1rem;justify-self:start;accent-color:var(--accent);}
textarea{font-family:ui-monospace,Consolas,monospace;font-size:0.75rem;}
.clear{display:inline-flex;gap:0.3rem;align-items:center;margin-top:0.25rem;font-size:0.8rem;color:var(--dim);border:0;padding:0;}
.ro{font-family:ui-monospace,Consolas,monospace;}
button{font:inherit;background:var(--accent);color:#fff;border:0;border-radius:6px;padding:0.55rem 1.4rem;cursor:pointer;}
@media (max-width:560px){label{grid-template-columns:1fr;gap:0.25rem;}td:first-child{width:45%;}}
</style>)CSS";

// Appends `s` with the five HTML-special characters escaped — for config
// strings (device name, MQTT host, server replies) placed into page text or
// attribute values.
void appendEscaped(String& html, const char* s)
{
    for (; *s; s++) {
        switch (*s) {
            case '&':  html += "&amp;";  break;
            case '<':  html += "&lt;";   break;
            case '>':  html += "&gt;";   break;
            case '"':  html += "&quot;"; break;
            case '\'': html += "&#39;";  break;
            default:   html += *s;       break;
        }
    }
}

void pageStart(String& html, const char* title, const char* activeHref, int refreshSec = 0)
{
    const char* name = gCfg.deviceName[0] ? gCfg.deviceName : "WeatherNode";
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    if (refreshSec > 0) { html += "<meta http-equiv='refresh' content='"; html += refreshSec; html += "'>"; }
    html += "<title>"; appendEscaped(html, name); html += ' '; html += title; html += "</title>";
    html += FPSTR(PAGE_CSS);
    html += "<script>try{var t=localStorage.getItem('wnTheme');if(t)document.documentElement.setAttribute('data-theme',t);}catch(e){}</script>";
    html += "</head><body><nav>";
    static const char* const links[][2] = {
        { "/", "Dashboard" }, { "/status", "Status" }, { "/api/log", "Log" },
        { "/api/settings", "Settings" }, { "/help", "Help / Documentation" },
    };
    for (auto& l : links) {
        html += "<a href='"; html += l[0]; html += "'";
        if (!strcmp(l[0], activeHref)) html += " class='on'";
        html += ">"; html += l[1]; html += "</a>";
    }
    html += "</nav><h3>"; appendEscaped(html, name); html += ' '; html += title; html += "</h3>";
}

// "42 s ago" / "7 min ago" / "3.5 h ago" for a millis() timestamp.
String agoText(unsigned long atMs)
{
    unsigned long s = (millis() - atMs) / 1000;
    if (s < 60)   return String(s) + " s ago";
    if (s < 3600) return String(s / 60) + " min ago";
    return String(s / 3600.0f, 1) + " h ago";
}

// Wireless sensor types (gCfg.rfProto / parameter 50) for the settings
// select and the status page.
struct RfTypeEntry { int value; const char* text; };
const RfTypeEntry RF_TYPES[] = {
    { RF_PROTO_AUTO, "Auto (search, stores the first type received)" },
    { RF_PROTO_BRESSER, "Bresser (5/6/7-in-1 and others)" },
    { RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9, "TFA / LaCrosse IT+ (both data rates)" },
    { RF_PROTO_ITPLUS17, "TFA / LaCrosse IT+ 17.241 kbit/s only" },
    { RF_PROTO_ITPLUS9, "TFA / LaCrosse IT+ 9.579 kbit/s only" },
    { RF_PROTO_FINEOFFSET, "Fine Offset / Ecowitt / Froggit" },
    { RF_PROTO_ITPLUS17 | RF_PROTO_FINEOFFSET, "TFA IT+ 17.241 kbit/s + Fine Offset (same data rate, no alternating)" },
    { RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9 | RF_PROTO_FINEOFFSET, "TFA IT+ (both data rates) + Fine Offset" },
    { RF_PROTO_BRESSER | RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9, "Bresser + TFA IT+ alternating (not recommended)" },
    { RF_PROTO_BRESSER | RF_PROTO_FINEOFFSET, "Bresser + Fine Offset alternating (not recommended)" },
};
constexpr size_t RF_TYPE_COUNT = sizeof(RF_TYPES) / sizeof(RF_TYPES[0]);

String rfSensorTypeText(int v)
{
    for (const RfTypeEntry& t : RF_TYPES) if (t.value == v) return t.text;
    return "bit mask " + String(v);
}

void addRow(String& html, const String& label, const String& value)
{
    html += "<tr><td>" + label + "</td><td class='num'>" + value + "</td></tr>";
}

// One connection/service row: status dot (ok/warn/bad/off), name, details.
void addStateRow(String& html, const char* cls, const char* label, const String& detail)
{
    html += "<tr><td><span class='dot "; html += cls; html += "'></span>";
    html += label; html += "</td><td>"; html += detail; html += "</td></tr>";
}

// Status page, Connections: Loxone UDP output.
void addLoxoneRow(String& html)
{
    if (!gCfg.loxEn) { addStateRow(html, "off", "Loxone UDP", "<span class='dim'>disabled</span>"); return; }
    const LoxoneStatus& ls = loxoneStatus();
    String d = "to ";
    loxoneLock();
    appendEscaped(d, gCfg.loxHost[0] ? gCfg.loxHost : "(no host)");
    loxoneUnlock();
    d += ":" + String(gCfg.loxPort);
    if (ls.ipValid) d += " <span class='dim'>(" + ls.ip.toString() + ")</span>";
    d += gCfg.loxSec ? " &middot; every " + String(gCfg.loxSec) + " s" : String(" &middot; every cycle");
    const char* cls = "ok";
    if (!gCfg.wifiEn)       { cls = "bad"; d += " &middot; needs WiFi"; }
    if (ls.everSent) {
        unsigned long ago = (millis() - ls.lastSendMs) / 1000;
        d += " &middot; last set " + (ago < 120 ? String(ago) + " s" : String(ago / 60) + " min") + " ago, " +
             String(ls.lastValues) + " values";
        if (ls.lastFailed) { cls = "warn"; d += ", " + String(ls.lastFailed) + " failed"; }
    } else {
        cls = "warn";
        d += " &middot; nothing sent yet";
    }
    if (ls.errors) {
        d += " &middot; " + String(ls.errors) + " error(s)";
        if (ls.lastError) {
            unsigned long ago = (millis() - ls.lastErrorMs) / 1000;
            d += String(", last: ") + ls.lastError + " (" + (ago < 120 ? String(ago) + " s" : String(ago / 60) + " min") + " ago)";
        }
    } else if (ls.lastError) {
        d += String(" &middot; ") + ls.lastError;   // DNS problem before any set
    }
    addStateRow(html, cls, "Loxone UDP", d);
}

void addWeatherNetworkRows(String& html)
{
    WxTargetStatus st[WX_TARGET_COUNT];
    wxUploadStatus(st);
    for (const WxTargetStatus& s : st) {
        String d;
        const char* cls;
        if (!s.enabled) {
            cls = "off";
            d   = "<span class='dim'>not configured</span>";
        } else if (s.lastAttemptMs == 0) {
            cls = "warn";
            d   = "enabled, every " + String(s.intervalSec) + " s &middot; no upload yet";
            if (s.lastReply[0]) { d += " <span class='dim'>("; appendEscaped(d, s.lastReply); d += ")</span>"; }
        } else if (s.lastOkMs == s.lastAttemptMs) {
            cls = "ok";
            d   = "OK &middot; last upload " + agoText(s.lastOkMs);
        } else {
            cls = "bad";
            d   = "failed " + agoText(s.lastAttemptMs) + ": ";
            if (s.lastHttpCode > 0) d += "HTTP " + String(s.lastHttpCode) + ", ";
            d += "&ldquo;"; appendEscaped(d, s.lastReply); d += "&rdquo;";
            d += " &middot; last success " + (s.lastOkMs ? agoText(s.lastOkMs) : String("never"));
        }
        if (s.enabled && s.lastAttemptMs) {
            d += "<br><small>" + String(s.okCount) + " ok / " + String(s.failCount) +
                 " failed since boot &middot; every " + String(s.intervalSec) + " s</small>";
        }
        addStateRow(html, cls, s.name, d);
    }
}

// Health overview: latest readings, device state, and whether every
// transport/upload is actually working. Plain server-rendered HTML with no
// JS dependency (a fast check for curl/monitoring or a JS-disabled browser);
// reloads itself every 30 s.
void handleStatusPage()
{
    JsonDocument doc;
    buildJsonPayload(doc);

    String html;
    html.reserve(7000);
    pageStart(html, "status", "/status", 30);
    html += "<p class='sub'>Firmware " FIRMWARE_VERSION " &middot; refreshes every 30 s</p>";

    html += "<section><h4>Measurements</h4><table>";
    bool srcSet = gCfg.rfEn != 0;
    for (int g = 0; g < SG_COUNT; g++) srcSet |= gCfg.srcSel[g] != SRC_AUTO;
    if (srcSet) {   // which sensor feeds each value (settings "Value sources")
        static const char* const names[SG_COUNT] = { "temperature/humidity", "wind", "rain", "UV", "light" };
        String v;
        for (int g = 0; g < SG_COUNT; g++) {
            char d[24]; srcDescribe((SrcGroup)g, d, sizeof(d));
            if (g) v += " &middot; ";
            v += String(names[g]) + ": " + d;
        }
        if (gSrc.rfBatteryLow) v += " &middot; <b>wireless battery low</b>";
        addRow(html, "Sources", "<span class='dim'>" + v + "</span>");
    }
    if (srcActive(SG_WIND)) {
        addRow(html, "Wind direction",     String(doc["windDirDeg"].as<int>()) + "&deg;" +
                                           (doc["windDirText"].is<const char*>() ? String(" (") + doc["windDirText"].as<const char*>() + ")" : String()));
        addRow(html, "Wind speed",         String(doc["windSpeed"].as<float>(), 1) + " " + doc["windUnit"].as<const char*>());
        addRow(html, "Wind gust",          String(doc["windGust"].as<float>(),  1) + " " + doc["windUnit"].as<const char*>());
    }
    if (srcActive(SG_RAIN)) {
        bool dailyValid;
        float dailyMm = wxDailyRainMm(dailyValid);
        addRow(html, "Rain rate",          String(doc["rainRate"].as<float>(), 2)       + " " + doc["rainRateUnit"].as<const char*>());
        addRow(html, "Rain (this cycle)",  String(doc["rain"].as<float>(), 2)           + " " + doc["rainUnit"].as<const char*>());
        addRow(html, "Rain today",         String(rainForOutput(dailyMm, gCfg), 2) + " " + rainUnitLabel(gCfg) +
                                           (dailyValid ? "" : " <span class='dim'>(clock not synced yet)</span>"));
        addRow(html, "Rain last hour / 24 h", String(doc["rain1h"].as<float>(), 2) + " / " + String(doc["rain24h"].as<float>(), 2) +
                                           " " + doc["rainUnit"].as<const char*>());
        if (doc["rainYesterday"].is<float>())
            addRow(html, "Rain yesterday", String(doc["rainYesterday"].as<float>(), 2) + " " + doc["rainUnit"].as<const char*>());
        if (doc["rainWeek"].is<float>())
            addRow(html, "Rain this week / month / year", String(doc["rainWeek"].as<float>(), 2) + " / " +
                   String(doc["rainMonth"].as<float>(), 2) + " / " + String(doc["rainYear"].as<float>(), 2) + " " +
                   doc["rainUnit"].as<const char*>() + (gCfg.rainPersist ? "" : " <span class='dim'>(RAM only, lost on power loss)</span>"));
        else
            addRow(html, "Rain this week / month / year", "<span class='dim'>after the first clock sync</span>");
        addRow(html, "Rain since start",   String(doc["rainSinceStart"].as<float>(), 2) + " " + doc["rainUnit"].as<const char*>());
        if (doc["lastRain"].is<const char*>()) {
            String lr = doc["lastRain"].as<const char*>();
            time_t t = 0;
            struct tm u = {};
            if (sscanf(lr.c_str(), "%d-%d-%dT%d:%d", &u.tm_year, &u.tm_mon, &u.tm_mday, &u.tm_hour, &u.tm_min) == 5) {
                u.tm_year -= 1900; u.tm_mon -= 1;
                t = mktime(&u);   // UTC string, TZ-independent difference below
                struct tm nowU; time_t now = time(nullptr); gmtime_r(&now, &nowU);
                long ageMin = (long)difftime(mktime(&nowU), t) / 60;
                String age = ageMin < 60 ? String(ageMin) + " min" : ageMin < 2880 ? String(ageMin / 60) + " h" : String(ageMin / 1440) + " days";
                addRow(html, "Last rain", age + " ago");
            }
        }
    }
    if (gSrc.tempValid)
        addRow(html, "Temperature",        String(doc["temp"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>());
    if (gSrc.tempValid && gSrc.humValid)
        addRow(html, "Humidity",           String(doc["humidityPct"].as<float>(), 1) + " %");
    if (bme280Present) {
        addRow(html, "Pressure (absolute)",String(doc["pressureAbs"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
        addRow(html, "Pressure (QNH)",     String(doc["pressureQnh"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
        if (doc["pressureTrend3h"].is<float>()) {
            float d3 = doc["pressureTrend3h"].as<float>();
            addRow(html, "Pressure tendency (3 h)", String(d3 >= 0 ? "+" : "") + String(d3, gCfg.unitSystem == 1 ? 2 : 1) + " " +
                   doc["pressureUnit"].as<const char*>() + " &middot; " + doc["pressureTendency"].as<const char*>() +
                   " &middot; trend: <b>" + doc["weatherTrend"].as<const char*>() + "</b>");
        } else {
            addRow(html, "Pressure tendency (3 h)", "<span class='dim'>available 3 h after start</span>");
        }
    }
    if (doc["dewPoint"].is<float>())
        addRow(html, "Dew point",          String(doc["dewPoint"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>());
    if (doc["vpd"].is<float>())
        addRow(html, "Vapour pressure deficit", String(doc["vpd"].as<float>(), 2) + " kPa");
    if (doc["feelsLike"].is<float>()) {
        DerivedNow dn;
        derivedNow(dn, windSpeedMs, windDirection);
        addRow(html, "Feels like",         String(doc["feelsLike"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>() +
               (dn.windChill ? " <span class='dim'>(wind chill)</span>" : dn.heatIndex ? " <span class='dim'>(heat index)</span>" : ""));
    }
    if (doc["tempTrend1h"].is<float>()) {
        float t1 = doc["tempTrend1h"].as<float>();
        addRow(html, "Temperature trend (1 h)", String(t1 >= 0 ? "+" : "") + String(t1, 1) + " K" +
               (gCfg.unitSystem == 1 ? " <span class='dim'>(&deg;F)</span>" : ""));
    }
    if (doc["cloudBase"].is<long>())
        addRow(html, "Cloud base",         String(doc["cloudBase"].as<long>()) + " " + doc["cloudBaseUnit"].as<const char*>() +
               " <span class='dim'>(above the station, estimated)</span>");
    if (doc["sunshineTodayMin"].is<long>()) {
        long m = doc["sunshineTodayMin"].as<long>();
        String v = String(m / 60) + " h " + String(m % 60) + " min";
        if (doc["sunshineYesterdayMin"].is<long>()) {
            long y = doc["sunshineYesterdayMin"].as<long>();
            v += " <span class='dim'>&middot; yesterday " + String(y / 60) + " h " + String(y % 60) + " min</span>";
        }
        addRow(html, "Sunshine today", v);
    }
    if (doc["warnings"].is<JsonObject>()) {
        JsonObject w = doc["warnings"];
        String v;
        if (w["frost"])  v += "frost ";
        if (w["fog"])    v += "fog/dew ";
        if (w["sultry"]) v += "sultry ";
        if (v.length()) addRow(html, "Warnings", "<b>" + v + "</b>");
    }
    if (bus1Count > 0) {
        JsonArray ds18b20 = doc["ds18b20"].as<JsonArray>();
        int i = 0;
        for (JsonVariant v : ds18b20) {
            if (!v.isNull())
                addRow(html, "DS18B20 #" + String(i), String(v.as<float>(), 1) + "&deg;" + doc["ds18b20Unit"].as<const char*>());
            i++;
        }
    }
    if (gCfg.lipoEn) addRow(html, "Battery", String(doc["batteryVolt"].as<float>(), 3) + " V");
    if (gSrc.uvRadio) addRow(html, "UV index", String(gSrc.uvIndexRadio, 1));
    if (gSrc.lightValid) addRow(html, "Illuminance", String((long)lroundf(gSrc.lightLux)) + " lx");
    if (ioV2() && gSrc.uvBoard) {
        if (doc["uvSensorVolts"].is<float>())
            addRow(html, "UV sensor voltage", String(doc["uvSensorVolts"].as<float>(), 3) + " V");
        addRow(html, "UV index calibration", gIo.uvIndexCalibrated ? "calibrated" : "not calibrated");
        if (doc["uvIndex"].is<float>())
            addRow(html, "UV index", String(doc["uvIndex"].as<float>(), 2));
    }
    if (ioV2() && gCfg.wind2Mode) {
        const char* wu = windSpeedUnitLabel(gCfg);
        if (ioWind2DirActive()) addRow(html, "Wind 2 direction", String(gIo.wind2DirDeg) + "&deg;");
        addRow(html, "Wind 2 speed", String(windSpeedForOutput(gIo.wind2SpeedMs, gCfg), 1) + " " + wu);
        addRow(html, "Wind 2 gust",  String(windSpeedForOutput(gIo.wind2GustMs,  gCfg), 1) + " " + wu);
    }
    if (ioV2() && ioAuxAinActive()) addRow(html, "AUX analog input", String(gIo.auxAinV, 3) + " V");
    if (ioV2() && gCfg.auxPulseEn && !gCfg.wind2Mode) addRow(html, "AUX pulses", String(gIo.auxPulsesCycle) + " (cycle) &middot; " +
                                          String(gIo.auxPulsesTotal) + " (total)");
    html += "</table></section>";

    if (ioV2() && (gCfg.heaterMode || gCfg.fanMode || gCfg.tachoEn)) {
        static const char* const modeText[] = { "off", "on", "auto" };
        html += "<section><h4>Outputs</h4><table>";
        if (gCfg.heaterMode)
            addStateRow(html, gIo.heaterOn ? "ok" : "off", "Heater (J12)",
                        String(gIo.heaterOn ? "on, " + String(gIo.heaterDuty) + " %" : String("off")) +
                        " &middot; mode " + modeText[constrain((int)gCfg.heaterMode, 0, 2)]);
        if (gCfg.fanMode || gCfg.tachoEn) {
            String d = gIo.fanOn ? "on, " + String(gIo.fanDuty) + " %" : String("off");
            d += String(" &middot; mode ") + modeText[constrain((int)gCfg.fanMode, 0, 2)];
            if (gCfg.tachoEn) d += " &middot; " + String((int)lroundf(gIo.fanRpm)) + " rpm";
            addStateRow(html, gIo.fanStalled ? "bad" : (gIo.fanOn ? "ok" : "off"), "Fan (J13)",
                        gIo.fanStalled ? d + " &middot; <b>no tacho pulses</b>" : d);
        }
        if (gIo.battLockout)
            addStateRow(html, "warn", "Battery lockout", "outputs held off below " + String(gCfg.outMinBattDv / 10.0f, 1) + " V");
        html += "</table></section>";
    }

    if (ioV2() && gCfg.veDirectEn) {
        static const struct { int cs; const char* text; } csText[] = {
            { 0, "off" }, { 2, "fault" }, { 3, "bulk" }, { 4, "absorption" }, { 5, "float" }, { 7, "equalize" },
            { 245, "starting up" }, { 247, "auto equalize" }, { 252, "external control" } };
        const VeDirectValues& ve = gIo.ve;
        html += "<section><h4>Victron VE.Direct</h4><table>";
        if (!gIo.veValid) {
            addStateRow(html, "bad", "Connection", "no valid data &mdash; check the cable (J14 / J15)");
        } else {
            if (!isnan(ve.battV)) addRow(html, "Battery voltage", String(ve.battV, 2) + " V");
            if (!isnan(ve.battA)) addRow(html, "Battery current", String(ve.battA, 2) + " A");
            if (!isnan(ve.socPct)) addRow(html, "State of charge", String(ve.socPct, 1) + " %");
            if (!isnan(ve.loadA))  addRow(html, "Load output current", String(ve.loadA, 2) + " A");
            if (!isnan(ve.pvV))   addRow(html, "PV voltage", String(ve.pvV, 2) + " V");
            if (!isnan(ve.pvW))   addRow(html, "PV power", String(ve.pvW, 0) + " W");
            if (!isnan(ve.yieldTodayKwh)) addRow(html, "Yield today", String(ve.yieldTodayKwh, 2) + " kWh");
            if (ve.chargeState >= 0) {
                String t = String(ve.chargeState);
                for (const auto& c : csText) if (c.cs == ve.chargeState) { t = c.text; break; }
                addRow(html, "Charge state", t);
            }
            if (ve.error >= 0)
                addStateRow(html, ve.error ? "bad" : "ok", "Error", ve.error ? "code " + String(ve.error) : String("none"));
        }
        html += "</table></section>";
    }

    html += "<section><h4>Connections</h4><table>";
    if (!gCfg.loraEn)            addStateRow(html, "off", "LoRaWAN", "<span class='dim'>disabled</span>");
    else if (!loraReady())      addStateRow(html, "warn", "LoRaWAN", "not joined &middot; automatic retries; measurements/WiFi continue");
    else if (sendFailCount == 0 && lastPayloadDropped[0])
        addStateRow(html, "warn", "LoRaWAN", String("uplinks OK &middot; ") + (gCfg.payloadFormat ? "binary" : "CayenneLPP") +
                    " &middot; last uplink left out (data rate too slow): " + lastPayloadDropped);
    else if (sendFailCount == 0) addStateRow(html, "ok",  "LoRaWAN", String("uplinks OK &middot; ") + (gCfg.payloadFormat ? "binary" : "CayenneLPP"));
    else                         addStateRow(html, "bad", "LoRaWAN", String(sendFailCount) + " send failure(s)");

    if (!gCfg.wifiEn) {
        addStateRow(html, "off", "WiFi", "<span class='dim'>disabled</span>");
    } else if (wifiIsConnected()) {
        String d;
        appendEscaped(d, WiFi.SSID().c_str());
        d += " &middot; " + WiFi.localIP().toString() + " &middot; " + String(WiFi.RSSI()) + " dBm";
        addStateRow(html, "ok", "WiFi", d);
    } else {
        addStateRow(html, "bad", "WiFi", "disconnected");
    }

    if (!gCfg.mqttEn) {
        addStateRow(html, "off", "MQTT", "<span class='dim'>disabled</span>");
    } else {
        String d;
        appendEscaped(d, gCfg.mqttHost);
        d += ":" + String(gCfg.mqttPort);
        addStateRow(html, mqttIsConnected() ? "ok" : "bad", "MQTT",
                    (mqttIsConnected() ? "connected &middot; " : "disconnected &middot; ") + d);
    }

    addLoxoneRow(html);

    struct tm lt;
    if (!gCfg.wifiEn) {
        addStateRow(html, "off", "Clock (NTP)", "<span class='dim'>needs WiFi</span>");
    } else if (localTimeNow(lt)) {
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &lt);
        addStateRow(html, "ok", "Clock (NTP)", String("synced &middot; ") + buf);
    } else {
        addStateRow(html, "warn", "Clock (NTP)", "waiting for first sync");
    }
    html += "</table></section>";

    html += "<section><h4>Weather networks</h4><table>";
    addWeatherNetworkRows(html);
    html += "</table></section>";

    if (gCfg.loraEn) {
        html += "<section><h4>LoRaWAN</h4><table>";
        const LoraDiag& ld = gLoraDiag;
        if (!ld.haveUplink) {
            addRow(html, "Uplinks", "<span class='dim'>none since start</span>");
        } else {
            // DR0-5 = SF12-SF7 at 125 kHz in EU868 and the other 1-channel-plan
            // regions; US915/AU915 use a different table, show the DR only there.
            String dr = "DR" + String(ld.dataRate);
            bool euLike = strncmp(gCfg.region, "US", 2) != 0 && strncmp(gCfg.region, "AU", 2) != 0;
            if (euLike && ld.dataRate <= 5) dr += " (SF" + String(12 - ld.dataRate) + ")";
            dr += " &middot; " + String(ld.freqMHz, 1) + " MHz &middot; " + String(ld.txPowerDbm) + " dBm";
            if (gBoard.fem != FEM_NONE)
                dr += " <span class='dim'>(SX1262 " + String(femChipPowerDbm(ld.txPowerDbm, gBoard.fem)) + " dBm + " +
                      femName(gBoard.fem) + " PA)</span>";
            addRow(html, "Last uplink", dr);
            unsigned long cfgMs = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
            if (ld.intervalMs > cfgMs)
                addRow(html, "Uplink interval", String(ld.intervalMs / 1000) + " s <span class='dim'>(set " +
                       String(gCfg.uplinkIntervalSec) + " s, stretched to keep 1 % airtime at this data rate)</span>");
            char addr[12];
            snprintf(addr, sizeof(addr), "%08lX", (unsigned long)ld.devAddr);
            addRow(html, "Device address / FCnt up", String(addr) + " &middot; " + String(ld.fCntUp));
        }
        if (ld.haveDownlink) {
            unsigned long ago = (millis() - ld.lastDownlinkMs) / 1000;
            String when = ago < 120 ? String(ago) + " s ago" : ago < 7200 ? String(ago / 60) + " min ago"
                                                            : String(ago / 3600.0f, 1) + " h ago";
            addRow(html, "Last downlink", when + " &middot; RSSI " + String(ld.lastDlRssi, 0) + " dBm &middot; SNR " +
                                          String(ld.lastDlSnr, 1) + " dB");
        } else {
            addRow(html, "Last downlink", "<span class='dim'>none since start</span>");
        }
        if (ld.rejoinThreshold)
            addRow(html, "Uplinks without downlink", String(ld.uplinksWithoutDownlink) + " / " + String(ld.rejoinThreshold) +
                   " <span class='dim'>(rejoin at the limit)</span>");
        addRow(html, "Rejoins since start", String(ld.rejoins));
        html += "</table></section>";
    }

    if (gCfg.rfEn) {
        html += "<section><h4>Wireless sensors (868 MHz)</h4><table>";
        if (rfSensorsReceiving())
            addStateRow(html, "ok", "Receiver", String("listening (") + rfSensorsListening() + ") between LoRaWAN uplinks &middot; " +
                        String(rfSensorsPacketsTotal()) + " packets since start");
        else if (rfSensorsLastError() == 0)   // LoRaWAN uses the chip right now (uplink + receive windows)
            addStateRow(html, "ok", "Receiver", "paused for a LoRaWAN uplink &middot; " +
                        String(rfSensorsPacketsTotal()) + " packets since start");
        else
            addStateRow(html, "bad", "Receiver", "not receiving (radio error " + String(rfSensorsLastError()) + ", retrying)");
        if (gCfg.rfProto == RF_PROTO_AUTO)
            addStateRow(html, "warn", "Sensor type", "Auto: searching &mdash; Bresser, IT+ 17.241 kbit/s + Fine Offset and "
                        "IT+ 9.579 kbit/s in turn until the first valid packet" +
                        String(gCfg.rfIds[0] ? " from a listed sensor ID" : ""));
        else if (rfSensorsDetectedProto())
            addStateRow(html, "ok", "Sensor type", String("detected: ") + rfSensorTypeText(rfSensorsDetectedProto()) +
                        " <span class='dim'>(auto search, saved)</span>");
        else
            addRow(html, "Sensor type", rfSensorTypeText(gCfg.rfProto));
        bool any = false;
        for (int i = 0; i < RF_MAX_SENSORS; i++) {
            const RfSensorReading& r = gRfSensors[i];
            if (!r.used) continue;
            any = true;
            char id[12];
            snprintf(id, sizeof(id), "%08lX", (unsigned long)r.id);
            unsigned long ago = (millis() - r.lastMs) / 1000;
            bool gone = (r.staleMask & RF_VAL_ANY) && ago < SRC_RF_FRESH_MS / 1000;   // millis() wrapped
            String v = (gone ? String("&gt;") + String(SRC_RF_FRESH_MS / 1000) : String(ago)) + " s ago &middot; RSSI " + String(r.rssi, 0) + " dBm";
            if (!r.batteryOk) v += " &middot; <b>battery low</b>";
            auto stale = [i](const char* key) { return srcRfValueFresh(i, key) ? String() : String(" <span class='dim'>(stale)</span>"); };
            if (r.hasTemp)     v += " &middot; " + String(temperatureForOutput(r.tempC, gCfg), 1) + "&deg;" + temperatureUnitLabel(gCfg) + stale("temp");
            if (r.hasHumidity) v += " &middot; " + String(r.humidity) + " %" + stale("humidity");
            if (r.hasWind)     v += " &middot; wind " + String(windSpeedForOutput(r.windAvgMs, gCfg), 1) + " / gust " +
                                    String(windSpeedForOutput(r.windGustMs, gCfg), 1) + " " + windSpeedUnitLabel(gCfg) +
                                    " &middot; " + String((int)lroundf(r.windDirDeg)) + "&deg;" + stale("wind");
            if (r.hasRain)     v += " &middot; rain total " + String(rainForOutput(r.rainTotalMm, gCfg), 1) + " " + rainUnitLabel(gCfg) + stale("rain");
            if (r.hasUv)       v += " &middot; UV " + String(r.uvIndex, 1) + stale("uv");
            if (r.hasLight)    v += " &middot; " + String((long)lroundf(r.lightLux)) + " lx" + stale("light");
            if (r.hasMoisture) v += " &middot; soil moisture " + String(r.moisturePct) + " %";
            if (r.hasSupercap) v += " &middot; supercap " + String(r.supercapV, 1) + " V";
            if (r.hasLightning) v += " &middot; lightning count " + String(r.lightningCount) +
                                     (r.lightningKm ? " &middot; storm " + String(r.lightningKm) + " km" : String());
            String kind = r.proto == RF_PROTO_BRESSER ? "type " + String(r.type) : String(rfProtoName(r.proto));
            if (r.model)   kind += String(" ") + r.model;
            if (r.channel) kind += " ch " + String(r.channel);
            addRow(html, String("ID ") + id + " (" + kind + ")", v);
        }
        if (!any) addRow(html, "Sensors", "<span class='dim'>none received yet (Bresser sensors send every 12&ndash;16 s, IT+ about every 4 s, Fine Offset every 5&ndash;80 s)</span>");
        html += "</table></section>";
    }

    html += "<section><h4>Device</h4><table>";
    if (gCfg.debugMode) addRow(html, "Measurement cycle", String(doc["cycle"].as<int>()));
    addRow(html, "Next measurement in", String(doc["nextCycleInSec"].as<uint32_t>()) + " s");
    {
        String b = String("Heltec WiFi LoRa 32 ") + boardName(gBoard.kind);
        if (boardIsV4()) b += gBoard.fem == FEM_KCT8103L ? ".3 (KCT8103L front end)" : ".2 (GC1109 front end)";
        b += gBoard.autoSel ? " <span class='dim'>(detected, " : " <span class='dim'>(set manually, detected ";
        if (!gBoard.autoSel) b += String(boardName(gBoard.detected)) + ", ";
        b += String(gBoard.flashBytes >> 20) + " MB flash)</span>";
        if (boardIsR8()) b += " <b>&ndash; V4-R8 (ESP32-S3R8) is not supported</b>";
        addRow(html, "Board", b);
    }
    if (boardPcbAllowed())
        addRow(html, "PCB revision",   gCfg.pcbVersion == PCB_REV_2_0 ? (gIo.hwIdDetected ? "v2.0 (detected)" : "v2.0 (set manually)")
                                       : (gCfg.pcbVersion == PCB_REV_1_2 ? "Rev 1.2" : "Rev 1.3"));
    else
        addRow(html, "PCB",            "<b>Heltec V4: wireless build only &ndash; PCB inputs disabled</b> "
                                       "<span class='dim'>(wind/rain/vane, 1-Wire and the v2.0 I/O block use GPIO2/5/7, "
                                       "taken by the V4's RF front end)</span>");
    addRow(html, "BME280 sensor",      bme280Present ? "present" : "not detected");
    addRow(html, "DS18B20 sensors",    String(bus1Count));
    addRow(html, "CPU temperature",    String(doc["cpuTemp"].as<float>(), 1) + "&deg;" + doc["cpuTempUnit"].as<const char*>() +
                                       " <span class='dim'>(chip, above ambient)</span>");
    addRow(html, "Uptime",             String((float)millis() / 3600000.0f, 2) + " h");
    if (gCfg.updateCheckEn) {
        char st[48]; wxUpdateStatus(st, sizeof(st));
        char up[24]; wxUpdateAvailable(up, sizeof(up));
        String v;
        if (up[0]) { v = "<b>"; appendEscaped(v, up); v += " available</b>"; }
        else       appendEscaped(v, st);
        addRow(html, "Firmware update", v);
    }
    addRow(html, "Last restart",       gResetReason);
    addRow(html, "Free heap",          String((float)ESP.getFreeHeap() / 1024.0f, 1) + " KiB &middot; minimum " +
                                       String((float)ESP.getMinFreeHeap() / 1024.0f, 1) + " KiB");
    html += "</table></section>";

    html += "<p class='dim'><a href='/api/data'>Raw JSON</a></p></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

// Gates both viewing and saving the settings page behind HTTP Digest Auth
// (browser's built-in login prompt) using the admin password configured via
// the WiFi portal. Digest rather than Basic: the settings page can change
// LoRaWAN keys and MQTT credentials, and Basic would put the password on
// the (unencrypted) network in plain text with every request. No password
// set at all -> access is refused outright rather than prompting for one
// that can never match.
bool requireAuth()
{
    if (gCfg.webAdminPass[0] == '\0') {
        server.send(403, "text/plain", "Forbidden: no admin password configured (set one via the WiFi portal's \"WiFi + MQTT Parameter\" section).");
        return false;
    }
    if (!server.authenticate("admin", gCfg.webAdminPass)) {
        server.requestAuthentication(DIGEST_AUTH, "WeatherNode settings");
        return false;
    }
    return true;
}

// CSRF guard for state-changing POSTs. The browser re-sends cached Digest
// credentials with any request to the node, so a foreign web page could
// otherwise submit the settings form for a logged-in user. Browsers mark
// the page a form was sent from in Origin (or at least Referer): its host
// must be the one the request is addressed to (Host). Requests carrying
// neither header (curl, scripts) are allowed - they carry no browser-cached
// credentials. "Origin: null" (sandboxed/privacy contexts) is refused.
bool sameOriginRequest()
{
    String src = server.header("Origin");
    if (src.isEmpty()) src = server.header("Referer");
    if (src.isEmpty()) return true;
    int start = src.indexOf("://");
    if (start < 0) return false;
    start += 3;
    int end = src.indexOf('/', start);
    String host = end < 0 ? src.substring(start) : src.substring(start, end);
    if (host.isEmpty() || !host.equalsIgnoreCase(server.hostHeader())) {
        dbgSerial.printf("Web: cross-site POST refused (from '%s', host '%s')\n", host.c_str(), server.hostHeader().c_str());
        return false;
    }
    return true;
}

bool requireSameOrigin()
{
    if (sameOriginRequest()) return true;
    server.send(403, "text/plain", "Forbidden: cross-site request (open the settings page on the node itself and submit it there).");
    return false;
}


void handleLogPage()
{
    if (!requireAuth()) return;
    server.send_P(200, "text/html; charset=utf-8", LOG_PAGE_HTML);
}

void handleLogRaw()
{
    if (!requireAuth()) return;
    static char buf[8192];
    dbgSerial.snapshot(buf, sizeof(buf));
    server.send(200, "text/plain; charset=utf-8", buf);
}

void handleLogClear()
{
    if (!requireAuth() || !requireSameOrigin()) return;
    dbgSerial.clear();
    server.send(200, "text/plain", "cleared");
}

// Rolling history JSON, served by handleApiHistory() below. history.cpp
// keeps its own two-tier in-RAM ring buffer (fine/24h + coarse/7-day); this
// only serves whichever tier ?range=7d selects (default: the fine tier).
void handleApiHistory()
{
    bool coarse = server.hasArg("range") && server.arg("range") == "7d";
    // static: see handleApiData()'s comment above — buildHistoryJson()
    // clears the content (not the buffer) at its own top (see buildHistoryJson()).
    static JsonDocument doc;
    buildHistoryJson(doc, coarse);
    // Full history (up to HISTORY_CAPACITY samples) is much bigger than the
    // fixed 1024-byte buf handleApiData() uses — stream it straight to the
    // client via ChunkedPrint instead of holding the whole serialized text
    // in RAM at once, and in a single serialize pass (see ChunkedPrint's
    // comment above).
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    ChunkedPrint out(server);
    serializeJson(doc, out);
    out.flush();
}


void handleChartPage()
{
    server.send_P(200, "text/html; charset=utf-8", CHART_PAGE_HTML);
}

// /chart itself moved to "/" (it's the page most visitors actually want) —
// redirect rather than 404 for anything that bookmarked/linked the old path.
void handleChartRedirect()
{
    server.sendHeader("Location", "/");
    server.send(302);
}

// ---------------------------------------------------------------------------
// Settings page: every setting except the WiFi network itself (SSID/
// password live in ESP-IDF's own storage and are changed via the config
// portal) and the guided wind-direction calibration (needs the portal's
// live ADC wizard). Gated by the admin password (requireAuth).
//
// Two kinds of fields, marked per section:
//  - live:    applied immediately, no restart (operational parameters via
//             applyParam() — same validation as LoRaWAN downlink 0x10 / the
//             MQTT "set" command — plus calibration, weather networks, time,
//             admin password)
//  - restart: LoRaWAN keys/region, network/MQTT, PCB revision — only read at
//             boot, so the node saves and restarts (losing the in-RAM chart
//             history) when any of them actually changed
//
// Passwords/keys are never sent back to the browser: secret fields render
// empty, an empty submission keeps the stored value, and optional secrets
// get an explicit "remove" checkbox. wifiEn/webApiEn are shown read-only —
// switching them off from this page would lock the page itself out.
// ---------------------------------------------------------------------------

// Same list as isSupportedRegion() (config.cpp), for the <select>.
const char* const LORA_REGIONS[] = { "EU868", "US915", "AU915", "AS923", "IN865", "KR920", "CN470", "RU864" };

// Set by handleSettingsPost() after a restart-requiring save; webApiTask
// restarts once the response has gone out.
volatile unsigned long restartAtMs = 0;

void sectionStart(String& html, const char* title, bool restart, const char* note = nullptr)
{
    html += "<section><h4>"; html += title;
    html += restart ? "<span class='tag restart'>restart</span>" : "<span class='tag live'>live</span>";
    html += "</h4>";
    if (note) { html += "<p class='dim'>"; html += note; html += "</p>"; }
}

void labelStart(String& html, const char* label, const char* hint)
{
    html += "<label><span>"; html += label;
    if (hint) { html += "<small>"; html += hint; html += "</small>"; }
    html += "</span>";
}

void fieldText(String& html, const char* name, const char* label, const String& value,
               size_t maxLen, const char* hint = nullptr, const char* type = "text")
{
    labelStart(html, label, hint);
    html += "<input type='"; html += type; html += "' name='"; html += name; html += "' value='";
    appendEscaped(html, value.c_str());
    html += "' maxlength='"; html += (unsigned)maxLen; html += "'";
    if (!strcmp(type, "number")) html += " step='any'";
    html += "></label>";
}

void fieldCheck(String& html, const char* name, const char* label, bool checked, const char* hint = nullptr)
{
    labelStart(html, label, hint);
    html += "<input type='checkbox' name='"; html += name; html += "'";
    if (checked) html += " checked";
    // sent after the checkbox: arg() returns the first value ("on" if
    // checked), so a missing field means "not in this form", not "off"
    html += "><input type='hidden' name='"; html += name; html += "' value='0'></label>";
}

struct Option { int value; const char* text; };

void fieldSelect(String& html, const char* name, const char* label, int current,
                 const Option* opts, size_t count, const char* hint = nullptr)
{
    labelStart(html, label, hint);
    html += "<select name='"; html += name; html += "'>";
    for (size_t i = 0; i < count; i++) {
        html += "<option value='"; html += opts[i].value; html += "'";
        if (opts[i].value == current) html += " selected";
        html += ">"; html += opts[i].text; html += "</option>";
    }
    html += "</select></label>";
}

void fieldSecret(String& html, const char* name, const char* label, bool isSet, bool clearable,
                 size_t maxLen, const char* hint = nullptr)
{
    labelStart(html, label, hint);
    html += "<span><input type='password' autocomplete='new-password' name='"; html += name;
    html += "' maxlength='"; html += (unsigned)maxLen;
    html += isSet ? "' placeholder='set &mdash; leave empty to keep'>" : "' placeholder='not set'>";
    if (clearable && isSet) {
        html += "<span class='clear'><input type='checkbox' name='"; html += name;
        html += "_clear'>remove</span>";
    }
    html += "</span></label>";
}

void fieldReadonly(String& html, const char* label, const String& value, const char* hint = nullptr)
{
    labelStart(html, label, hint);
    html += "<span class='ro'>"; html += value; html += "</span></label>";
}

String hexOf(uint64_t v)
{
    char buf[17];
    uint64ToHex(v, buf, sizeof(buf));
    return buf;
}

void renderSettings(const String& message, bool isError, int httpCode)
{
    static const Option unitSystems[] = { { 0, "Metric (&deg;C, mm, hPa)" }, { 1, "Imperial (&deg;F, in, inHg, mph)" } };
    static const Option windUnits[]  = { { 1, "m/s" }, { 0, "km/h" } };
    static const Option windSamples[] = { { 1, "1 s" }, { 3, "3 s (less wake-ups, low-power mode)" } };
    static const Option windAggs[]   = { { WIND_AGG_INTERVAL, "Interval (since the last send) &ndash; for Grafana etc." },
                                         { WIND_AGG_10MIN, "10 min (WMO) &ndash; ready-to-use values" } };
    static const Option cpuFreqs[]   = { { 80, "80 MHz (low power)" }, { 240, "240 MHz" } };
    static const Option dirTypes[]   = { { 0, "Potentiometer (Davis)" }, { 1, "Calibrated table (reed/resistor vane)" } };
    static const Option payFmts[]    = { { 1, "Compact binary (FPort 3, recommended)" }, { 0, "CayenneLPP (FPort 1, about twice the size)" } };
    static const Option tlsModes[]   = { { MQTT_TLS_NONE, "Plain (no TLS)" }, { MQTT_TLS_INSECURE, "TLS, trust any certificate" },
                                         { MQTT_TLS_VERIFY, "TLS, verify against CA below" } };
    static const Option boards[]     = { { BOARD_SEL_AUTO, "Auto (by flash size: 8 MB = V3, 16 MB = V4)" },
                                         { BOARD_V3, "Heltec WiFi LoRa 32 V3" }, { BOARD_V4, "Heltec WiFi LoRa 32 V4 (wireless build only)" } };
    static const Option pcbRevs[]    = { { PCB_REV_1_2, "Rev 1.2" }, { PCB_REV_1_3, "Rev 1.3" }, { PCB_REV_2_0, "v2.0" } };
    static const Option heatModes[]  = { { 0, "Off" }, { 1, "On" }, { 2, "Auto (below the threshold)" } };
    static const Option fanModes[]   = { { 0, "Off" }, { 1, "On" }, { 2, "Auto (above the threshold)" } };
    static const Option wind2Modes[] = { { 0, "Off (AUX inputs free)" }, { 1, "Speed (anemometer at AUX PULSE)" },
                                         { 2, "Speed + direction (vane at AUX AIN)" } };
    #define OPTS(a) a, sizeof(a) / sizeof(a[0])

    // One contiguous ~26 KB block. If the heap can't provide it (or a later
    // grow fails), String silently drops the appends - answer 503 instead of
    // serving a truncated form that would save half the settings.
    String html;
    if (!html.reserve(26000)) {
        server.send(503, "text/plain", "Not enough free memory for the settings page right now - please retry.");
        return;
    }
    pageStart(html, "settings", "/api/settings");
    html += "<p class='sub'><span class='tag live'>live</span> fields apply immediately. "
            "<span class='tag restart'>restart</span> fields save and restart the node (the in-RAM chart history is lost). "
            "Passwords and keys are never displayed &mdash; leave empty to keep. Switching WiFi or the web API off and the "
            "wind-direction calibration wizard stay in the config portal (hold the button 3&nbsp;s, or downlink <code>0x03</code>).</p>";
    if (message.length()) {
        html += "<div class='banner "; html += isError ? "badb" : "okb"; html += "'>"; html += message; html += "</div>";
    }
    if (wifiChangeBusy()) {
        html += "<div class='banner okb'>Testing the new WiFi network &hellip;</div>";
    } else {
        String wr = wifiChangeResult();
        if (wr.length()) {
            html += "<div class='banner "; html += wr.startsWith("Could not") ? "badb" : "okb"; html += "'>";
            appendEscaped(html, wr.c_str()); html += "</div>";
        }
    }
    html += "<form method='POST' action='/api/settings'><input type='hidden' name='form' value='1'>";

    sectionStart(html, "Measurement", false);
    fieldCheck (html, "rfen",      "Wireless sensors (868 MHz)", gCfg.rfEn,
                "Bresser, TFA / LaCrosse IT+ or Fine Offset / Ecowitt / Froggit sensors (see Wireless sensor type), "
                "received with the LoRa chip between uplinks; disables low-power mode");
    {
        // the last entry shows a bit mask set by downlink / MQTT that has no own
        // entry, so saving the form keeps it
        static char customText[48];
        static Option rfProtos[RF_TYPE_COUNT + 1];
        for (size_t i = 0; i < RF_TYPE_COUNT; i++) rfProtos[i] = { RF_TYPES[i].value, RF_TYPES[i].text };
        int cur = gCfg.rfProto;
        bool listed = false;
        for (size_t i = 0; i < RF_TYPE_COUNT; i++) listed |= rfProtos[i].value == cur;
        size_t count = RF_TYPE_COUNT;
        if (!listed && rfProtoValid(cur)) {
            snprintf(customText, sizeof(customText), "Other combination (bit mask %d, set by parameter 50)", cur);
            rfProtos[RF_TYPE_COUNT] = { cur, customText };
            count++;
        } else if (!listed) {
            cur = RF_PROTO_AUTO;
        }
        fieldSelect(html, "rfproto", "Wireless sensor type", cur, rfProtos, count,
                    "Auto: listens to Bresser, IT+ 17.241 kbit/s + Fine Offset and IT+ 9.579 kbit/s in turn and stores "
                    "the type of the first valid packet (from a listed ID, if IDs are set); the status page shows what "
                    "was found. "
                    "IT+: e.g. TFA 30.3143/3144/3147/3157/3159.IT (17.241 kbit/s), 30.3155/3156.WD (9.579 kbit/s); "
                    "with both rates the receiver alternates every 5 s. Fine Offset: Ecowitt / Froggit / Ambient "
                    "WH65/WS69 7-in-1, WS80, WS85, WS90, WS68, WH31/DP50, WN34, WH40, WN20, WH51, WH57 (868 MHz "
                    "versions); same data rate as IT+ 17.241 kbit/s, so both are received together (with IT+ 9.579 "
                    "kbit/s: 17 s / 5 s alternating). Bresser + another family: the receiver alternates (17 s "
                    "Bresser, 5 s per IT+ data rate, 17 s Fine Offset) and misses packets of both &mdash; e.g. "
                    "about every second Bresser wind packet; not recommended, choose one data rate where possible");
    }
    {
        // received sensors as a copy-and-paste hint ("learning" without extra buttons)
        static char seen[160];
        int n = snprintf(seen, sizeof(seen), "hex, comma-separated, max 4, order = wireless 1&ndash;4; empty = any sensor");
        bool first = true;
        for (int i = 0; i < RF_MAX_SENSORS && n < (int)sizeof(seen) - 24; i++) {
            if (!gRfSensors[i].used) continue;
            if (gRfSensors[i].model)
                n += snprintf(seen + n, sizeof(seen) - n, "%s%08lX (%s)", first ? ". Received: " : ", ",
                              (unsigned long)gRfSensors[i].id, gRfSensors[i].model);
            else
                n += snprintf(seen + n, sizeof(seen) - n, "%s%08lX (type %u)", first ? ". Received: " : ", ",
                              (unsigned long)gRfSensors[i].id, (unsigned)gRfSensors[i].type);
            first = false;
        }
        fieldText  (html, "rfids",     "Wireless sensor IDs", gCfg.rfIds, sizeof(gCfg.rfIds) - 1, seen);
    }
    {
        // value sources (sources.h): one select per group, wireless slots labelled with their ID
        static char rfLabel[4][48];
        for (int k = 0; k < 4; k++) {
            int idx = rfSensorIndexForSlot(k);
            std::vector<uint32_t> ids = rfParseIds(gCfg.rfIds);
            if (idx >= 0)
                snprintf(rfLabel[k], sizeof(rfLabel[k]), "Wireless %d (%08lX)", k + 1, (unsigned long)gRfSensors[idx].id);
            else if (k < (int)ids.size())
                snprintf(rfLabel[k], sizeof(rfLabel[k]), "Wireless %d (%08lX, not received)", k + 1, (unsigned long)ids[k]);
            else
                snprintf(rfLabel[k], sizeof(rfLabel[k]), "Wireless %d (not received)", k + 1);
        }
        const Option srcOpts[] = { { SRC_AUTO, "Auto (board first, then wireless 1&ndash;4)" }, { SRC_BOARD, "Board sensor" },
                                   { SRC_RF1, rfLabel[0] }, { SRC_RF1 + 1, rfLabel[1] }, { SRC_RF1 + 2, rfLabel[2] },
                                   { SRC_RF4, rfLabel[3] }, { SRC_OFF, "Off (not reported)" } };
        static const char* const names[SG_COUNT]  = { "srcair", "srcwind", "srcrain", "srcuv", "srclight" };
        static const char* const labels[SG_COUNT] = { "Source: temperature / humidity", "Source: wind",
                                                      "Source: rain", "Source: UV", "Source: light" };
        for (int g = 0; g < SG_COUNT; g++)
            fieldSelect(html, names[g], labels[g], gCfg.srcSel[g], OPTS(srcOpts),
                        g == SG_AIR   ? "used for LoRaWAN, MQTT, weather networks and history; a wireless sensor counts while "
                                        "it sent within 5 min. Pressure always comes from the BME280" :
                        g == SG_LIGHT ? "only wireless sensors (e.g. Bresser 7-in-1) measure light" :
                        g == SG_RAIN  ? "switching keeps the rain totals continuous (only increases of the selected source count)" : nullptr);
    }
    fieldText  (html, "uplinksec", "LoRaWAN / measurement interval (s)", String(gCfg.uplinkIntervalSec), 5,
                "5&ndash;3600; stretched automatically if the data rate would exceed 1 % airtime", "number");
    fieldSelect(html, "samplesec", "Wind sample interval", windSampleSecNormalized(gCfg.sampleSec), OPTS(windSamples),
                "gust = highest 3-s mean; other sensors every 10 s or each send");
    fieldSelect(html, "lorawagg",  "LoRaWAN wind values", gCfg.loraWindAgg, OPTS(windAggs),
                "Interval: mean, 3-s gust and direction since the last uplink. 10 min: the WMO values of the last "
                "10 minutes &mdash; do not average those again downstream");
    fieldText  (html, "mqttsec",   "MQTT interval (s)", String(gCfg.mqttIntervalSec), 5, "0 = like the LoRaWAN interval, else 2&ndash;3600", "number");
    fieldSelect(html, "mqttwagg",  "MQTT wind values", gCfg.mqttWindAgg, OPTS(windAggs));
    if (boardPcbAllowed()) {
        fieldCheck (html, "winden",    "Wind sensor (board)", gCfg.windEn, "Davis anemometer/vane on the PCB; off without a PCB (floating inputs)");
        fieldCheck (html, "rainen",    "Rain sensor (board)", gCfg.rainEn, "Davis rain gauge on the PCB");
    } else {
        fieldReadonly(html, "Wind / rain sensor (board)", "not available on Heltec V4",
                      "wireless build only &ndash; PCB inputs disabled (GPIO2/GPIO7 belong to the V4's RF front end)");
    }
    fieldCheck (html, "rainpersist", "Keep rain statistics over a power cut", gCfg.rainPersist,
                "saves rain/sunshine today and yesterday, last rain and rain week/month/year to flash at most "
                "once per hour, at midnight and before every restart/update, so they survive a power cut; "
                "off = kept in RTC RAM only, lost on power loss (and possibly on a firmware update)");
    fieldCheck (html, "lipoen",    "LiPo battery measurement", gCfg.lipoEn);
    fieldText  (html, "altitude",  "Altitude above sea level (m)", String(gCfg.altitudeM), 5, "0&ndash;4000, for sea-level pressure (QNH)", "number");
    fieldSelect(html, "cpufreq",   "CPU frequency", gCfg.cpuFreqMhz, OPTS(cpuFreqs));
    fieldCheck (html, "debugmode", "Debug mode", gCfg.debugMode, "adds diagnostic fields to every payload");
    html += "</section>";

    if (ioV2()) {
        sectionStart(html, "Inputs &amp; outputs (PCB v2.0)", false,
                     "Heater (max 3 A) and fan (max 1 A) run from the VIN terminal. Auto modes use the BME280 temperature "
                     "(else DS18B20 #0), thresholds in &deg;C with 1&nbsp;K hysteresis.");
        fieldCheck (html, "auxainen",    "AUX analog input (GPIO4)", gCfg.auxAinEn);
        fieldText  (html, "auxainscale", "AUX analog scale", String(gCfg.auxAinScale, 3), 8,
                    "V at terminal per V at pin: 1 = direct (ADC useful to ~3.1&nbsp;V; 3.3&nbsp;V limit), 1.66667 with R25 15k (0&ndash;5&nbsp;V); older boards: retain their calibration", "number");
        fieldCheck (html, "auxpulseen",  "AUX pulse counter (GPIO48)", gCfg.auxPulseEn);
        fieldSelect(html, "wind2",       "2nd wind sensor (AUX)", gCfg.wind2Mode, OPTS(wind2Modes),
                    "uses the AUX PULSE (and AIN) terminals instead of the AUX counter/analog input "
                    "same calibration as the main anemometer, vane 0&ndash;3.3&nbsp;V from AUX 3V3");
        fieldText  (html, "wind2off",    "2nd wind vane offset (&deg;)", String(gCfg.wind2DirOffsetDeg), 4, "&minus;180 to +180", "number");
        fieldSelect(html, "heatmode",    "Heater (J12)", gCfg.heaterMode, OPTS(heatModes));
        fieldText  (html, "heatduty",    "Heater power (%)", String(gCfg.heaterDutyPct), 3, "1&ndash;100, 10 s heating cycle (0.1 Hz); auto: power at the full-power temperature", "number");
        fieldText  (html, "heatbelow",   "Heater auto: on below (&deg;C)", String(gCfg.heaterOnBelowDc / 10.0f, 1), 6, "off again 1&nbsp;K above", "number");
        fieldText  (html, "heatminduty", "Heater auto: power at that temperature (%)", String(gCfg.heaterMinDutyPct), 3, "rises linearly towards the full-power temperature", "number");
        fieldText  (html, "heatfull",    "Heater auto: full power at (&deg;C)", String(gCfg.heaterFullBelowDc / 10.0f, 1), 6, "and colder", "number");
        fieldSelect(html, "fanmode",     "Fan (J13)", gCfg.fanMode, OPTS(fanModes));
        fieldText  (html, "fanduty",     "Fan speed (%)", String(gCfg.fanDutyPct), 3, "1&ndash;100, PWM; tacho and stall check only at 100", "number");
        fieldText  (html, "fanabove",    "Fan auto: on from (&deg;C)", String(gCfg.fanOnAboveDc / 10.0f, 1), 6, "off again 1&nbsp;K below", "number");
        fieldText  (html, "fanminpv",    "Fan auto: minimum PV power (W)", String(gCfg.fanMinPvW), 5, "0 = ignore; otherwise missing/disabled VE.Direct PV data keeps fan off", "number");
        fieldCheck (html, "tachoen",     "Fan tacho (GPIO40)", gCfg.tachoEn);
        fieldText  (html, "tachoppr",    "Tacho pulses per revolution", String(gCfg.tachoPpr), 1, "PC fans: 2", "number");
        fieldCheck (html, "vedirecten",  "Victron VE.Direct", gCfg.veDirectEn, "J14 / J15, isolated");
        fieldText  (html, "outminbatt",  "Heater/fan: minimum battery voltage (V)", String(gCfg.outMinBattDv / 10.0f, 1), 5,
                    "0 = no limit; otherwise missing/disabled VE.Direct keeps outputs off (4s LiFePO4: e.g. 12.4)", "number");
        fieldCheck (html, "uven",        "UV sensor (GPIO3)", gCfg.uvEn,
                    "raw sensor voltage is reported whenever enabled; GPIO3 is never driven as an output");
        fieldText  (html, "uvscale",     "UV divider scale", String(gCfg.uvAinScale, 5), 10,
                    "terminal V per ADC V: 1.1 for JP10 1-2 (0-3.3 V), 1.66534 for JP10 2-3 (0-5 V)", "number");
        fieldCheck (html, "uvindexen",   "Publish calculated UV index", gCfg.uvIndexEn);
        fieldText  (html, "uvslope",     "UV index slope (index/V)", String(gCfg.uvIndexSlope, 3), 10,
                    "linear calibration; valid range 0.001-30", "number");
        fieldText  (html, "uvoffset",    "UV index offset", String(gCfg.uvIndexOffset, 3), 10,
                    "linear calibration; valid range -10 to +10", "number");
        fieldCheck (html, "uvcal",       "Calibration explicitly verified", gCfg.uvCalibrated,
                    "index remains omitted until this is enabled and slope/offset are valid");
        html += "</section>";
    }

    sectionStart(html, "Units", false, "Device-wide: also affects MQTT, Home Assistant and the LoRaWAN payload.");
    fieldSelect(html, "unitsys",  "Unit system", gCfg.unitSystem, OPTS(unitSystems));
    fieldSelect(html, "windunit", "Wind speed unit (metric only)", gCfg.windUnitMs, OPTS(windUnits));
    html += "</section>";

    sectionStart(html, "Sensor calibration", false, "Defaults match the Davis 6410 (wind) and 6466M (rain).");
    fieldText  (html, "rainmmtip",  "Rain per tip (mm)", String(gCfg.rainMmPerTip, 4), 10, "Davis: 0.2", "number");
    fieldText  (html, "windmspp",   "Wind speed per pulse/s (m/s)", String(gCfg.windMsPerPps, 5), 10, "Davis: 1.00584", "number");
    fieldText  (html, "windoff",    "Wind direction offset (&deg;)", String(gCfg.windDirOffsetDeg), 4, "&minus;180 to +180, added to the reading", "number");
    fieldSelect(html, "winddirtyp", "Wind direction sensor", gCfg.windDirSensorType, OPTS(dirTypes));
    fieldReadonly(html, "Calibration table", gCfg.windDirCalCount ? String(gCfg.windDirCalCount) + " positions" : String("not calibrated"),
                  "recorded with the portal's wizard");
    html += "</section>";

    // Read under the upload lock: these are the fields the upload task reads.
    wxUploadLock();
    sectionStart(html, "Weather networks", false,
                 "Direct uploads over WiFi, each enabled by filling in its station ID. Enable a network either here "
                 "or in weather-data-bridge, never both &mdash; it would receive every reading twice.");
    fieldText  (html, "lat", "Latitude", gCfg.latitude, sizeof(gCfg.latitude) - 1, "decimal degrees, e.g. 47.2692 (sent to AWEKAS)");
    fieldText  (html, "lon", "Longitude", gCfg.longitude, sizeof(gCfg.longitude) - 1, "decimal degrees, e.g. 11.4041");
    fieldText  (html, "awkuser", "AWEKAS user name", gCfg.awekasUser, sizeof(gCfg.awekasUser) - 1);
    fieldSecret(html, "awkpass", "AWEKAS password", gCfg.awekasPass[0], true, sizeof(gCfg.awekasPass) - 1);
    fieldText  (html, "awkint",  "AWEKAS interval (s)", String(gCfg.awekasIntervalSec), 4, "300&ndash;3600", "number");
    fieldText  (html, "wuid",    "Weather Underground station ID", gCfg.wuStationId, sizeof(gCfg.wuStationId) - 1);
    fieldSecret(html, "wukey",   "Weather Underground station key", gCfg.wuStationKey[0], true, sizeof(gCfg.wuStationKey) - 1);
    fieldText  (html, "wuint",   "Weather Underground interval (s)", String(gCfg.wuIntervalSec), 4, "60&ndash;3600", "number");
    fieldText  (html, "wcid",    "Weathercloud ID", gCfg.wcloudId, sizeof(gCfg.wcloudId) - 1);
    fieldSecret(html, "wckey",   "Weathercloud key", gCfg.wcloudKey[0], true, sizeof(gCfg.wcloudKey) - 1);
    fieldText  (html, "wcint",   "Weathercloud interval (s)", String(gCfg.wcloudIntervalSec), 4, "60&ndash;3600 (free accounts: 600)", "number");
    fieldText  (html, "wnpid",   "Wetternetzwerk.pro station ID", gCfg.wnetproId, sizeof(gCfg.wnetproId) - 1);
    fieldSecret(html, "wnpkey",  "Wetternetzwerk.pro station key", gCfg.wnetproKey[0], true, sizeof(gCfg.wnetproKey) - 1);
    fieldText  (html, "wnpint",  "Wetternetzwerk.pro interval (s)", String(gCfg.wnetproIntervalSec), 4, "60&ndash;3600", "number");
    fieldText  (html, "windyid", "Windy station ID", gCfg.windyId, sizeof(gCfg.windyId) - 1, "stations.windy.com &rarr; My stations");
    fieldSecret(html, "windypw", "Windy station password", gCfg.windyPass[0], true, sizeof(gCfg.windyPass) - 1);
    fieldText  (html, "windyint","Windy interval (s)", String(gCfg.windyIntervalSec), 4, "300&ndash;3600", "number");
    fieldText  (html, "osemid",  "openSenseMap senseBox ID", gCfg.osemBoxId, sizeof(gCfg.osemBoxId) - 1);
    fieldSecret(html, "osemtok", "openSenseMap access token", gCfg.osemToken[0], true, sizeof(gCfg.osemToken) - 1,
                "only for boxes with authentication (default for new boxes)");
    fieldText  (html, "osemmap", "openSenseMap sensor IDs", gCfg.osemSensors, sizeof(gCfg.osemSensors) - 1,
                "key=sensorId, comma-separated; keys: temp, hum, press, wind, gust, dir, rain (last hour), rainday, dew, uv, lux "
                "&mdash; units &deg;C, %, hPa, m/s, &deg;, mm, lx");
    fieldText  (html, "osemint", "openSenseMap interval (s)", String(gCfg.osemIntervalSec), 4, "60&ndash;3600", "number");
    html += "</section>";

    sectionStart(html, "Time", false);
    fieldText(html, "tz",  "Time zone (POSIX TZ)", gCfg.timeZone, sizeof(gCfg.timeZone) - 1,
              "Central Europe: CET-1CEST,M3.5.0,M10.5.0/3 &middot; UK: GMT0BST,M3.5.0/1,M10.5.0");
    fieldText(html, "ntp", "NTP server", gCfg.ntpServer, sizeof(gCfg.ntpServer) - 1);
    html += "</section>";
    wxUploadUnlock();

    sectionStart(html, "Firmware update check", false);
    fieldCheck (html, "updchk", "Check daily for newer firmware", gCfg.updateCheckEn,
                "shown on the status page and as <code>firmwareUpdate</code> in JSON; installing stays manual (OTA)");
    fieldText  (html, "updurl", "Version list URL", gCfg.updateUrl, sizeof(gCfg.updateUrl) - 1,
                "tags (Gitea) or releases (GitHub) API returning names like 2.0.0-beta.3");
    html += "</section>";

    sectionStart(html, "Web access", false);
    fieldSecret  (html, "webadminpw", "Admin password", true, false, sizeof(gCfg.webAdminPass) - 1, "user name: admin");
    fieldReadonly(html, "WiFi / Web API", "enabled", "can only be switched off in the config portal");
    html += "</section>";

    sectionStart(html, "LoRaWAN", true);
    fieldCheck (html, "loraen", "LoRaWAN enabled", gCfg.loraEn);
    fieldCheck (html, "lowpower", "Low-power mode", gCfg.lowPowerEn, "light sleep between samples; only active while Wi-Fi is off");
    fieldCheck (html, "vanesave", "Low-power: switch wind-vane supply off", gCfg.vanePowerSave, "PCB v2.0; off for vanes that need continuous power");
    fieldText  (html, "deveui",  "DevEUI", hexOf(gCfg.devEui), 16, "16 hex characters");
    fieldText  (html, "joineui", "JoinEUI", hexOf(gCfg.joinEui), 16, "16 hex characters");
    fieldSecret(html, "appkey",  "AppKey", true, false, 32, "32 hex characters");
    labelStart (html, "Region", nullptr);
    html += "<select name='region'>";
    for (const char* r : LORA_REGIONS) {
        html += "<option"; if (!strcmp(r, gCfg.region)) html += " selected"; html += ">"; html += r; html += "</option>";
    }
    html += "</select></label>";
    fieldSelect(html, "payfmt", "Payload format", gCfg.payloadFormat, OPTS(payFmts),
                "applies immediately. Slow data rates allow only 51 bytes (SF10&ndash;SF12): the node then leaves out the least "
                "important values (debug, AUX, outputs, Victron, LiPo, DS18B20, then BME280, rain, wind) until the uplink fits "
                "&mdash; binary usually fits completely, CayenneLPP often does not. The status page shows what was left out.");
    html += "</section>";

    sectionStart(html, "Network &amp; MQTT", true);
    fieldText  (html, "wifissid",   "WiFi network (SSID)", WiFi.SSID(), 32,
                "a change is tested for 60&nbsp;s without restart; if the node cannot connect or neither the gateway nor DNS "
                "answers there, it returns to the current network. Another network needs DHCP: switch a static IP off "
                "in the same step");
    fieldSecret(html, "wifipass",   "WiFi password", true, true, 63,
                "leave empty to keep the current one; &ldquo;remove&rdquo; = open network");
    fieldCheck (html, "wifinotest", "Store WiFi without test (pre-provisioning)", false,
                "for a network that is not in range yet: stored right away, the node leaves the current network "
                "immediately and keeps trying the new one &mdash; reach it again via the new network, LoRaWAN "
                "(downlink <code>0x03</code> opens the config portal) or the button");
    fieldText  (html, "devname",    "Device name", gCfg.deviceName, sizeof(gCfg.deviceName) - 1, "hostname, MQTT client ID, default topic prefix");
    fieldCheck (html, "staticipen", "Static IP", gCfg.staticIpEn, "off = DHCP");
    fieldText  (html, "staticip",   "IP address", gCfg.staticIp, 15);
    fieldText  (html, "staticgw",   "Gateway", gCfg.staticGateway, 15);
    fieldText  (html, "staticsn",   "Subnet mask", gCfg.staticSubnet, 15);
    fieldText  (html, "staticdns",  "DNS server", gCfg.staticDns, 15, "optional, defaults to the gateway");
    fieldCheck (html, "mqtten",     "MQTT enabled", gCfg.mqttEn);
    fieldCheck (html, "mqttctrlen", "MQTT command topic", gCfg.mqttCtrlEn, "accept commands on &lt;prefix&gt;/cmd");
    fieldText  (html, "mqtthost",   "Broker host", gCfg.mqttHost, sizeof(gCfg.mqttHost) - 1);
    fieldText  (html, "mqttport",   "Broker port", String(gCfg.mqttPort), 5, "1883 plain, 8883 TLS", "number");
    fieldText  (html, "mqttuser",   "User name", gCfg.mqttUser, sizeof(gCfg.mqttUser) - 1, "optional");
    fieldSecret(html, "mqttpass",   "Password", gCfg.mqttPass[0], true, sizeof(gCfg.mqttPass) - 1, "optional");
    fieldText  (html, "mqtttopic",  "Topic prefix", gCfg.mqttTopic, sizeof(gCfg.mqttTopic) - 1, "optional, defaults to the device name");
    fieldSelect(html, "mqtttls",    "Security", gCfg.mqttTlsMode, OPTS(tlsModes));
    labelStart (html, "CA certificate (PEM)", "only for &ldquo;verify&rdquo;");
    html += "<textarea name='mqttcacert' rows='6' maxlength='"; html += (unsigned)(sizeof(gCfg.mqttCaCert) - 1);
    html += "' placeholder='-----BEGIN CERTIFICATE-----'>";
    appendEscaped(html, gCfg.mqttCaCert);
    html += "</textarea></label></section>";

    sectionStart(html, "Loxone (UDP)", false,
                 "Sends every value as its own UDP datagram <code>name=value</code> (e.g. <code>temp=6.6</code>) to a "
                 "Loxone Miniserver: create a Virtual UDP Input with this port and one command per value, "
                 "recognition e.g. <code>temp=\\v</code>. Names = the keys of <code>/api/data</code>. Values that are "
                 "currently not valid are not sent; Loxone keeps the last one. Needs WiFi.");
    fieldCheck (html, "loxen",     "Loxone UDP output", gCfg.loxEn);
    loxoneLock();
    fieldText  (html, "loxhost",   "Miniserver IP or host name", gCfg.loxHost, sizeof(gCfg.loxHost) - 1,
                "a host name is resolved without blocking and refreshed every 10 min");
    fieldText  (html, "loxprefix", "Name prefix", gCfg.loxPrefix, sizeof(gCfg.loxPrefix) - 1,
                "optional, A&ndash;Z a&ndash;z 0&ndash;9 _ . - (max 16), e.g. <code>g75_</code> to tell several nodes apart on one port");
    loxoneUnlock();
    fieldText  (html, "loxport",   "UDP port", String(gCfg.loxPort), 5, "1&ndash;65535, default 7000", "number");
    fieldText  (html, "loxsec",    "Loxone interval (s)", String(gCfg.loxSec), 5,
                "0 = every measurement cycle, else 2&ndash;3600; wind mean/gust and rain cover the time since the last send", "number");
    html += "</section>";

    sectionStart(html, "Hardware", true);
    {
        static char boardHint[120];
        snprintf(boardHint, sizeof(boardHint), "detected: %s (%lu MB flash)%s; V4: wireless build only, no PCB",
                 boardName(gBoard.detected), (unsigned long)(gBoard.flashBytes >> 20),
                 boardIsR8() ? " &ndash; V4-R8 not supported" : "");
        fieldSelect(html, "board", "Heltec board", gCfg.boardSel, OPTS(boards), boardHint);
    }
    if (boardPcbAllowed())
        fieldSelect(html, "pcbver", "PCB revision", gCfg.pcbVersion, OPTS(pcbRevs));
    else
        fieldReadonly(html, "PCB revision", "ignored on Heltec V4", "wireless build only &ndash; PCB inputs disabled");
    html += "</section>";
    #undef OPTS

    html += "<button type='submit'>Save</button></form></body></html>";
    if (!html.endsWith("</form></body></html>")) {
        server.send(503, "text/plain", "Not enough free memory for the settings page right now - please retry.");
        return;
    }
    server.send(httpCode, "text/html; charset=utf-8", html);
}

void handleSettingsGet()
{
    if (!requireAuth()) return;
    renderSettings(server.hasArg("saved") ? "Saved." : "", false, 200);
}

// --- POST helpers. `rejected` collects the labels of invalid fields: they
// keep their stored value, everything else is still applied. ---

void addRejected(String& rejected, const char* label)
{
    if (rejected.length()) rejected += ", ";
    rejected += label;
}

String formArg(const char* name)
{
    String v = server.arg(name);
    v.trim();
    return v;
}

bool isHex(const String& s, size_t len)
{
    if (s.length() != len) return false;
    for (size_t i = 0; i < len; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

// Plain text field; sets `changed` if the stored value differs.
// Only if the field is part of the request (a partial POST keeps the rest).
template <size_t N>
void storeText(char (&dst)[N], const char* name, bool& changed)
{
    if (!server.hasArg(name)) return;
    String v = formArg(name);
    if (v.length() >= N) v.remove(N - 1);
    if (strcmp(dst, v.c_str()) != 0) { strlcpy(dst, v.c_str(), N); changed = true; }
}

// Write-only secret: empty keeps, "<name>_clear" (optional secrets) wipes.
template <size_t N>
void storeSecret(char (&dst)[N], const char* name, bool& changed)
{
    String clearName = String(name) + "_clear";
    if (server.hasArg(clearName)) {
        if (dst[0]) { dst[0] = '\0'; changed = true; }
        return;
    }
    String v = server.arg(name);   // not trimmed: spaces may be part of a password
    if (v.isEmpty()) return;
    if (v.length() >= N) v.remove(N - 1);
    if (strcmp(dst, v.c_str()) != 0) { strlcpy(dst, v.c_str(), N); changed = true; }
}

void storeInt(int& dst, const char* name, long minV, long maxV, const char* label, String& rejected, bool& changed)
{
    if (!server.hasArg(name)) return;
    String v = formArg(name);
    long n = v.toInt();
    if (v.isEmpty() || n < minV || n > maxV) { addRejected(rejected, label); return; }
    if (dst != n) { dst = (int)n; changed = true; }
}

// Decimal-degree coordinate or empty (= not set).
template <size_t N>
void storeCoord(char (&dst)[N], const char* name, float limit, const char* label, String& rejected)
{
    if (!server.hasArg(name)) return;
    String v = formArg(name);
    if (v.length()) {
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (*end != '\0' || f < -limit || f > limit || v.length() >= N) { addRejected(rejected, label); return; }
    }
    strlcpy(dst, v.c_str(), N);
}

bool validIp(const String& s)
{
    IPAddress ip;
    return ip.fromString(s);
}

void handleSettingsPost()
{
    if (!requireAuth() || !requireSameOrigin()) return;
    if (!server.hasArg("form")) {
        renderSettings("Incomplete form submission &mdash; nothing was saved.", true, 400);
        return;
    }

    // Only fields present in the request are changed, so a partial POST
    // (curl, scripts) leaves everything else alone. Checkboxes: the page
    // sends a hidden "0" after each one (fieldCheck), so "unchecked" is
    // distinguishable from "not part of the request".
    String rejected;
    auto present = [](const char* name) { return server.hasArg(name); };
    auto checked = [](const char* name) -> long { String v = server.arg(name); return (v.length() && v != "0") ? 1 : 0; };
    auto apply = [&rejected](uint8_t id, long value, const char* label) {
        if (!applyParam(id, clampToParamValue(value))) addRejected(rejected, label);
    };
    auto applyNum = [&](uint8_t id, const char* name, const char* label) {
        if (present(name)) apply(id, formArg(name).toInt(), label);
    };
    auto applyCheck = [&](uint8_t id, const char* name, const char* label) {
        if (present(name)) apply(id, checked(name), label);
    };
    auto scaled1000 = [](const char* name) -> long {
        String text = formArg(name);
        char* end = nullptr;
        float value = strtof(text.c_str(), &end);
        if (text.isEmpty() || *end != '\0' || !isfinite(value) ||
            value < -32.768f || value > 32.767f) {
            return -32768;
        }
        return lroundf(value * 1000.0f);
    };

    // ---- live: operational parameters (uplink before sample interval:
    // the sample interval is validated against the uplink interval) ----
    applyNum(PARAM_UPLINK_INTERVAL_SEC,      "uplinksec",   "measurement interval (5-3600 s)");
    applyNum(PARAM_SAMPLE_SEC,               "samplesec",   "wind sample interval (1 or 3 s)");
    applyNum(PARAM_MQTT_INTERVAL_SEC,        "mqttsec",     "MQTT interval (0 or 2-3600 s)");
    applyNum(PARAM_LORA_WIND_AGG,            "lorawagg",    "LoRaWAN wind values");
    applyNum(PARAM_MQTT_WIND_AGG,            "mqttwagg",    "MQTT wind values");
    applyCheck(PARAM_LOX_EN,                 "loxen",       "Loxone UDP output");
    applyNum(PARAM_LOX_INTERVAL_SEC,         "loxsec",      "Loxone interval (0 or 2-3600 s)");
    applyCheck(PARAM_RF_EN,                  "rfen",        "wireless sensors");
    applyNum(PARAM_RF_PROTO,                 "rfproto",     "wireless sensor type");
    {
        static const char* const names[SG_COUNT] = { "srcair", "srcwind", "srcrain", "srcuv", "srclight" };
        for (int g = 0; g < SG_COUNT; g++)
            if (formArg(names[g]).length())
                applyNum(PARAM_SRC_TEMP_HUM + g, names[g], "value source");
    }
    applyCheck(PARAM_WIND_EN,                "winden",      "wind sensor");
    applyCheck(PARAM_RAIN_EN,                "rainen",      "rain sensor");
    applyCheck(PARAM_RAIN_PERSIST,           "rainpersist", "rain statistics in flash");
    applyCheck(PARAM_LIPO_EN,                "lipoen",      "LiPo measurement");
    applyCheck(PARAM_DEBUG_MODE,             "debugmode",   "debug mode");
    // Only when changed: applyParam() reprograms the CPU clock unconditionally.
    if (present("cpufreq") && formArg("cpufreq").toInt() != gCfg.cpuFreqMhz)
        applyNum(PARAM_CPU_FREQ_MHZ,             "cpufreq",     "CPU frequency");
    applyNum(PARAM_UNIT_SYSTEM,              "unitsys",     "unit system");
    applyNum(PARAM_WIND_UNIT_MS,             "windunit",    "wind speed unit");
    applyNum(PARAM_WIND_DIR_OFFSET_DEG,      "windoff",     "wind direction offset (-180 to 180)");
    applyNum(PARAM_WIND_DIR_SENSOR_TYPE,     "winddirtyp",  "wind direction sensor");
    applyNum(PARAM_PAYLOAD_FORMAT,           "payfmt",      "payload format");
    if (ioV2()) {   // the section is only rendered on v2.0
        auto x10 = [](const char* name) -> long { return lroundf(formArg(name).toFloat() * 10.0f); };
        // 2nd wind sensor first: it takes over the AUX inputs it uses, the
        // AUX checkboxes then only apply to the inputs that are still free.
        applyNum(PARAM_WIND2_MODE,               "wind2",       "2nd wind sensor");
        applyNum(PARAM_WIND2_DIR_OFFSET_DEG,     "wind2off",    "2nd wind vane offset (-180 to 180)");
        if (gCfg.wind2Mode != 2)
            applyCheck(PARAM_AUX_AIN_EN,             "auxainen",    "AUX analog input");
        if (present("auxainscale")) apply(PARAM_AUX_AIN_SCALE_X1000, lroundf(formArg("auxainscale").toFloat() * 1000.0f), "AUX analog scale (0.001-20)");
        if (!gCfg.wind2Mode)
            applyCheck(PARAM_AUX_PULSE_EN,           "auxpulseen",  "AUX pulse counter");
        applyNum(PARAM_HEATER_MODE,              "heatmode",    "heater mode");
        applyNum(PARAM_HEATER_DUTY_PCT,          "heatduty",    "heater power (1-100 %)");
        if (present("heatbelow")) apply(PARAM_HEATER_ON_BELOW_DC,  x10("heatbelow"),             "heater threshold (-40 to 40 &deg;C)");
        applyNum(PARAM_HEATER_MIN_DUTY_PCT,      "heatminduty", "heater power at the threshold (1-100 %)");
        if (present("heatfull")) apply(PARAM_HEATER_FULL_BELOW_DC, x10("heatfull"),             "heater full-power temperature (-40 to 40 &deg;C)");
        applyNum(PARAM_FAN_MODE,                 "fanmode",     "fan mode");
        applyNum(PARAM_FAN_DUTY_PCT,             "fanduty",     "fan speed (1-100 %)");
        if (present("fanabove")) apply(PARAM_FAN_ON_ABOVE_DC,     x10("fanabove"),              "fan threshold (-40 to 60 &deg;C)");
        applyNum(PARAM_FAN_MIN_PV_W,             "fanminpv",    "fan minimum PV power (0-5000 W)");
        applyCheck(PARAM_TACHO_EN,               "tachoen",     "fan tacho");
        applyNum(PARAM_TACHO_PPR,                "tachoppr",    "tacho pulses per revolution (1-8)");
        applyCheck(PARAM_VEDIRECT_EN,            "vedirecten",  "VE.Direct");
        if (present("outminbatt")) apply(PARAM_OUT_MIN_BATT_DV,     x10("outminbatt"),            "minimum battery voltage (0-60 V)");
        applyCheck(PARAM_UV_EN,                  "uven",        "UV sensor");
        if (present("uvscale")) apply(PARAM_UV_AIN_SCALE_X1000,  scaled1000("uvscale"),
                                                                        "UV divider scale (1-5)");
        applyCheck(PARAM_UV_INDEX_EN,            "uvindexen",   "UV index enable");
        if (present("uvslope")) apply(PARAM_UV_INDEX_SLOPE_X1000,scaled1000("uvslope"),
                                                                        "UV index slope (0-30)");
        if (present("uvoffset")) apply(PARAM_UV_INDEX_OFFSET_X1000,scaled1000("uvoffset"),
                                                                        "UV index offset (-10 to +10)");
        applyCheck(PARAM_UV_CALIBRATED,          "uvcal",       "UV calibration flag");
    }

    // Calibration factors directly as float — applyParam()'s x1000 integer
    // encoding (made for the 2-byte downlink value) would round 1.00584 to 1.006.
    if (present("rainmmtip")) {
        float rainTip = formArg("rainmmtip").toFloat();
        if (rainTip > 0.0f && rainTip <= 5.0f) gCfg.rainMmPerTip = rainTip;
        else addRejected(rejected, "rain per tip (0-5 mm)");
    }
    if (present("windmspp")) {
        float windPps = formArg("windmspp").toFloat();
        if (windPps > 0.0f && windPps <= 10.0f) gCfg.windMsPerPps = windPps;
        else addRejected(rejected, "wind speed per pulse/s (0-10)");
    }

    bool ignored = false;   // "changed" sink for live fields (no restart needed)
    int altitude = gCfg.altitudeM;
    storeInt(altitude, "altitude", 0, 4000, "altitude (0-4000 m)", rejected, ignored);
    gCfg.altitudeM = altitude;

    // ---- live: weather networks, time, admin password ----
    bool timeChanged = false;
    wxUploadLock();
    storeCoord(gCfg.latitude,  "lat", 90.0f,  "latitude", rejected);
    storeCoord(gCfg.longitude, "lon", 180.0f, "longitude", rejected);
    storeText  (gCfg.awekasUser,   "awkuser", ignored);
    storeSecret(gCfg.awekasPass,   "awkpass", ignored);
    storeInt   (gCfg.awekasIntervalSec, "awkint", 300, 3600, "AWEKAS interval (300-3600 s)", rejected, ignored);
    storeText  (gCfg.wuStationId,  "wuid",    ignored);
    storeSecret(gCfg.wuStationKey, "wukey",   ignored);
    storeInt   (gCfg.wuIntervalSec, "wuint", 60, 3600, "Weather Underground interval (60-3600 s)", rejected, ignored);
    storeText  (gCfg.wcloudId,     "wcid",    ignored);
    storeSecret(gCfg.wcloudKey,    "wckey",   ignored);
    storeInt   (gCfg.wcloudIntervalSec, "wcint", 60, 3600, "Weathercloud interval (60-3600 s)", rejected, ignored);
    storeText  (gCfg.wnetproId,    "wnpid",   ignored);
    storeSecret(gCfg.wnetproKey,   "wnpkey",  ignored);
    storeInt   (gCfg.wnetproIntervalSec, "wnpint", 60, 3600, "Wetternetzwerk.pro interval (60-3600 s)", rejected, ignored);
    storeText  (gCfg.windyId,      "windyid", ignored);
    storeSecret(gCfg.windyPass,    "windypw", ignored);
    storeInt   (gCfg.windyIntervalSec, "windyint", 300, 3600, "Windy interval (300-3600 s)", rejected, ignored);
    storeText  (gCfg.osemBoxId,    "osemid",  ignored);
    storeSecret(gCfg.osemToken,    "osemtok", ignored);
    storeText  (gCfg.osemSensors,  "osemmap", ignored);
    storeInt   (gCfg.osemIntervalSec, "osemint", 60, 3600, "openSenseMap interval (60-3600 s)", rejected, ignored);
    if (present("updchk")) gCfg.updateCheckEn = checked("updchk");
    if (formArg("updurl").startsWith("http")) storeText(gCfg.updateUrl, "updurl", ignored);
    else if (present("updurl") && gCfg.updateCheckEn) addRejected(rejected, "version list URL (http/https)");
    if (present("tz")) {
        if (formArg("tz").isEmpty()) addRejected(rejected, "time zone");
        else                         storeText(gCfg.timeZone, "tz", timeChanged);
    }
    if (present("ntp")) {
        if (formArg("ntp").isEmpty()) addRejected(rejected, "NTP server");
        else                          storeText(gCfg.ntpServer, "ntp", timeChanged);
    }
    wxUploadUnlock();
    if (timeChanged) timeSyncReconfigure();
    // Not clearable: an empty admin password would lock this page out.
    storeSecret(gCfg.webAdminPass, "webadminpw", ignored);
    storeText(gCfg.rfIds, "rfids", ignored);   // rf_sensors.cpp picks up changes on its next loop
    {   // Loxone: live, read by the main loop under the same lock
        int loxPort = gCfg.loxPort;
        storeInt(loxPort, "loxport", 1, 65535, "Loxone UDP port (1-65535)", rejected, ignored);
        gCfg.loxPort = loxPort;
        loxoneLock();
        storeText(gCfg.loxHost, "loxhost", ignored);
        if (present("loxprefix")) {
            String pre = formArg("loxprefix");
            if (loxPrefixValid(pre.c_str())) storeText(gCfg.loxPrefix, "loxprefix", ignored);
            else addRejected(rejected, "Loxone name prefix (max 16 of A-Z a-z 0-9 _ . -)");
        }
        loxoneUnlock();
    }

    // ---- restart: only read at boot. Collected into a copy (static: Config
    // is several KB, too big for this task's stack), so the running
    // transports never see a half-applied change before the restart. ----
    applyCheck(PARAM_LOW_POWER_EN,           "lowpower",    "low-power mode");
    applyCheck(PARAM_VANE_POWER_SAVE,        "vanesave",    "wind-vane power saving");
    static Config next;
    next = gCfg;
    bool restart = false;
    if (present("loraen")) {
        int loraEn = (int)checked("loraen");
        if (next.loraEn != loraEn) { next.loraEn = loraEn; restart = true; }
    }
    String eui = formArg("deveui");
    if (isHex(eui, 16)) { uint64_t v = hexToUint64(eui.c_str()); if (v != next.devEui) { next.devEui = v; restart = true; } }
    else if (present("deveui")) addRejected(rejected, "DevEUI (16 hex)");
    eui = formArg("joineui");
    if (isHex(eui, 16)) { uint64_t v = hexToUint64(eui.c_str()); if (v != next.joinEui) { next.joinEui = v; restart = true; } }
    else if (present("joineui")) addRejected(rejected, "JoinEUI (16 hex)");
    String key = formArg("appkey");
    if (key.length()) {
        uint8_t k[16];
        if (isHex(key, 32)) {
            hexToBytes(key.c_str(), k, sizeof(k));
            if (memcmp(k, next.appKey, sizeof(k)) != 0) { memcpy(next.appKey, k, sizeof(k)); restart = true; }
        } else {
            addRejected(rejected, "AppKey (32 hex)");
        }
    }
    String region = formArg("region");
    if (isSupportedRegion(region)) storeText(next.region, "region", restart);
    else if (present("region")) addRejected(rejected, "region");

    storeText(next.deviceName, "devname", restart);
    int staticIpEn = present("staticipen") ? (int)checked("staticipen") : next.staticIpEn;
    auto argOr = [](const char* name, const char* stored) { return server.hasArg(name) ? formArg(name) : String(stored); };
    String ip = argOr("staticip", next.staticIp), gw = argOr("staticgw", next.staticGateway),
           sn = argOr("staticsn", next.staticSubnet), dns = argOr("staticdns", next.staticDns);
    if (staticIpEn && !(validIp(ip) && validIp(gw) && validIp(sn) && (dns.isEmpty() || validIp(dns)))) {
        addRejected(rejected, "static IP (invalid address)");
    } else {
        if (next.staticIpEn != staticIpEn) { next.staticIpEn = staticIpEn; restart = true; }
        storeText(next.staticIp,      "staticip",  restart);
        storeText(next.staticGateway, "staticgw",  restart);
        storeText(next.staticSubnet,  "staticsn",  restart);
        storeText(next.staticDns,     "staticdns", restart);
    }
    int mqttEn     = present("mqtten")     ? (int)checked("mqtten")     : next.mqttEn;
    int mqttCtrlEn = present("mqttctrlen") ? (int)checked("mqttctrlen") : next.mqttCtrlEn;
    if (next.mqttEn != mqttEn)         { next.mqttEn = mqttEn; restart = true; }
    if (next.mqttCtrlEn != mqttCtrlEn) { next.mqttCtrlEn = mqttCtrlEn; restart = true; }
    storeText  (next.mqttHost,  "mqtthost",  restart);
    storeInt   (next.mqttPort,  "mqttport", 1, 65535, "MQTT port", rejected, restart);
    storeText  (next.mqttUser,  "mqttuser",  restart);
    storeSecret(next.mqttPass,  "mqttpass",  restart);
    storeText  (next.mqttTopic, "mqtttopic", restart);
    storeInt   (next.mqttTlsMode, "mqtttls", MQTT_TLS_NONE, MQTT_TLS_VERIFY, "MQTT security", rejected, restart);
    if (server.arg("mqttcacert").length() >= sizeof(next.mqttCaCert)) addRejected(rejected, "CA certificate (too long)");
    else storeText(next.mqttCaCert, "mqttcacert", restart);
    int board = present("board") ? formArg("board").toInt() : next.boardSel;
    if (!boardSelValid(board)) addRejected(rejected, "Heltec board");
    else if (next.boardSel != board) { next.boardSel = board; restart = true; }
    int pcb = present("pcbver") ? formArg("pcbver").toInt() : next.pcbVersion;
    if (gIo.hwIdDetected && pcb != PCB_REV_2_0) addRejected(rejected, "PCB revision (the board identifies itself as v2.0)");
    else if (pcb == PCB_REV_1_2 || pcb == PCB_REV_1_3 || pcb == PCB_REV_2_0) { if (next.pcbVersion != pcb) { next.pcbVersion = pcb; restart = true; } }
    else addRejected(rejected, "PCB revision");

    String rejectedMsg = rejected.length() ? "Invalid, kept the stored value: " + rejected + "." : String();

    // ---- WiFi network: tested with rollback (wifi_mqtt.cpp), not a restart field ----
    // Another network (SSID) only together with DHCP: a static IP belongs to
    // the current network - kept for the new one it would put the node into
    // the wrong subnet. Switching the static IP off in the same submission
    // is fine (the test then runs with DHCP, the restart applies it).
    bool wifiChange = false;
    bool noTest     = checked("wifinotest");
    if (present("wifissid")) {
        String ssid    = server.arg("wifissid");   // not trimmed: spaces may be part of an SSID
        String pass    = server.arg("wifipass");
        bool   clear   = server.hasArg("wifipass_clear");
        String curSsid = WiFi.SSID(), curPass = wifiCurrentPassword();
        String newPass = clear ? String() : (pass.length() ? pass : curPass);
        bool otherNetwork = ssid != curSsid;
        if (ssid.length() && gCfg.wifiEn && !wifiChangeBusy() && (otherNetwork || newPass != curPass)) {
            if (otherNetwork && next.staticIpEn)
                addRejected(rejected, "WiFi network (a static IP is set for the current network &mdash; switch the static IP "
                                      "off in the same step or before, and set it again on the new network)");
            else if (wifiRequestChange(ssid.c_str(), newPass.c_str(), !noTest, next.staticIpEn == 0)) wifiChange = true;
            else addRejected(rejected, "WiFi (name 1-32 characters, password 8-63 characters or none)");
        } else if (ssid.isEmpty()) {
            addRejected(rejected, "WiFi network (empty)");
        }
    }
    if (rejected.length()) rejectedMsg = "Invalid, kept the stored value: " + rejected + ".";

    if (wifiChange) {
        if (restart) { saveConfig(next); restartAtMs = millis() + 1000; if (restartAtMs == 0) restartAtMs = 1; }
        else         saveConfig(gCfg);
        String html;
        pageStart(html, "settings", "/api/settings", 75);
        if (noTest) {
            html += "<div class='banner okb'>Saved. The new WiFi network is stored without a test; the node leaves "
                    "this network now";
            html += restart ? " and restarts to apply the other network/LoRaWAN settings" : "";
            html += " and connects as soon as the new network is in range.</div>";
        } else {
            html += "<div class='banner okb'>Saved. The node now tries the new WiFi network for up to 60&nbsp;s";
            html += restart ? " and then restarts to apply the other network/LoRaWAN settings" : "";
            html += ". If it connects, it stays there &mdash; reach it again by its name or the IP on the display. "
                    "Otherwise it returns to the current network and this page shows a notice. It reloads in 75&nbsp;s.</div>";
        }
        // rejectedMsg is built from fixed labels only (already HTML, e.g. &deg;)
        if (rejectedMsg.length()) { html += "<div class='banner badb'>"; html += rejectedMsg; html += "</div>"; }
        html += "</body></html>";
        server.send(200, "text/html; charset=utf-8", html);
        dbgSerial.println("Web settings: WiFi network change requested");
        return;
    }

    if (restart) {
        saveConfig(next);
        String html;
        pageStart(html, "settings", "/api/settings", 20);
        html += "<div class='banner okb'>Saved. Restarting to apply the new network/LoRaWAN settings &mdash; "
                "this page reloads in 20&nbsp;s.</div>";
        if (rejectedMsg.length()) { html += "<div class='banner badb'>"; html += rejectedMsg; html += "</div>"; }
        html += "</body></html>";
        server.send(200, "text/html; charset=utf-8", html);
        dbgSerial.println("Web settings: restart-requiring change saved, restarting...");
        restartAtMs = millis() + 1000;
        if (restartAtMs == 0) restartAtMs = 1;
        return;
    }

    saveConfig(gCfg);
    if (rejectedMsg.length()) {
        renderSettings("Saved, except &mdash; " + rejectedMsg, true, 400);
        return;
    }
    server.sendHeader("Location", "/api/settings?saved=1");
    server.send(303);
}

// Runs continuously on its own FreeRTOS task (core 0) once webApiSetup()
// spawns it — this is what used to be webApiPoll(), called once per
// main-loop() iteration on core 1 alongside sensor sampling and the
// (blocking) sendLoRa(). Moving it here means an incoming HTTP request no
// longer has to wait behind LoRa airtime; the tradeoff (cross-task access
// to `server`/history.cpp's ring buffer/Config's volatile int fields) is
// handled by serverMutex here, historyMutex in history.cpp, and `volatile`
// on the relevant Config fields, respectively — see the comments at each.
void webApiTask(void*)
{
    for (;;) {
        xSemaphoreTake(serverMutex, portMAX_DELAY);
        if (!gCfg.wifiEn || !gCfg.webApiEn) {
            if (serverStarted) {
                server.stop();
                serverStarted = false;
            }
        } else if (!serverStarted && !apiStopped && wifiIsConnected()) {
            server.begin();
            serverStarted = true;
            dbgSerial.println("Web API started on port 80 (/, /status, /api/data, /api/settings, /help).");
        }
        if (serverStarted) server.handleClient();
        xSemaphoreGive(serverMutex);

        // Deferred restart after a restart-requiring settings save (see
        // handleSettingsPost()) — the response has been sent by now.
        // A WiFi change test runs first (it would be cut short otherwise).
        if (restartAtMs && (long)(millis() - restartAtMs) >= 0 && !wifiChangeBusy()) systemRestart("settings saved");

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

} // namespace

void webApiSetup()
{
    server.on("/",             HTTP_GET,  handleChartPage);
    server.on("/status",       HTTP_GET,  handleStatusPage);
    server.on("/chart",        HTTP_GET,  handleChartRedirect);
    server.on("/api/data",     HTTP_GET,  handleApiData);
    server.on("/api/settings", HTTP_GET,  handleSettingsGet);
    server.on("/api/settings", HTTP_POST, handleSettingsPost);
    server.on("/api/log",       HTTP_GET,  handleLogPage);
    // Origin/Referer for the CSRF check (Authorization is always collected)
    static const char* headerKeys[] = { "Origin", "Referer" };
    server.collectHeaders(headerKeys, 2);
    server.on("/api/log/raw",   HTTP_GET,  handleLogRaw);
    server.on("/api/log/clear", HTTP_POST, handleLogClear);
    server.on("/api/history",        HTTP_GET,  handleApiHistory);
    server.on("/help",         HTTP_GET,  handleHelp);
    server.on("/help.md",      HTTP_GET,  handleHelpMd);

    // Priority 1 == the main loop task's own priority (Arduino's default
    // "loopTask") — neither task should be able to starve the other for
    // this workload. 8 KB stack covers WebServer's own request parsing plus
    // our handlers' JSON/String building (matches Arduino's default loop
    // task stack size).
    xTaskCreatePinnedToCore(webApiTask, "webApi", 8192, nullptr, 1, &webApiTaskHandle, 0);
}

void webApiStop()
{
    // serverMutex's initializer runs as part of this file's static
    // construction, before any function in it — including this one — can be
    // called from elsewhere, so it's always valid here even though
    // webApiSetup() (which spawns webApiTask) may not have run yet. That
    // matters because main.cpp documents this exact function as being
    // called on the first-boot portal path before webApiSetup() is reached.
    xSemaphoreTake(serverMutex, portMAX_DELAY);
    apiStopped = true;
    if (serverStarted) {
        server.stop();
        serverStarted = false;
        dbgSerial.println("Web API stopped (port 80 freed for the config portal).");
    }
    xSemaphoreGive(serverMutex);

    // Stopping the server once isn't enough on its own anymore: webApiTask
    // keeps polling every ~5 ms on its own core and would just call
    // server.begin() again the next time it sees wifiEn/webApiEn still set
    // and WiFi still connected (plausible during the portal in AP+STA mode)
    // — reopening exactly the port-80 conflict with WiFiManager's own
    // captive-portal server that this function exists to prevent. Every
    // caller of webApiStop() (button long-press, first boot, downlink 0x03
    // — all via main.cpp's openPortalSaveAndRestart()) heads into
    // ESP.restart() regardless of outcome, so a one-way suspend (never
    // resumed) is correct here, not just a "pause". Guard against a null
    // handle: on the first-boot path this can run before webApiSetup() has
    // created the task at all, and vTaskSuspend(NULL) means "suspend the
    // *calling* task", not "no-op" — passing it unguarded here would freeze
    // the main task instead of the (nonexistent) web task.
    if (webApiTaskHandle) vTaskSuspend(webApiTaskHandle);
}
