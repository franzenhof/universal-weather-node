#include "web_api.h"
#include "globals.h"
#include "params.h"
#include "json_payload.h"
#include "history.h"
#include "wifi_mqtt.h"
#include "readme_md.h"
#include "help_page.h"
#include "log_page.h"
#include "chart_page.h"
#include "generated_version.h"
#include "serial_log.h"

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>

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
    char buf[1024];
    serializeJson(doc, buf, sizeof(buf));
    server.send(200, "application/json", buf);
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

// Status page: the last measured values (same data as /api/data, rendered
// as HTML) plus device/connectivity info. Used to live at "/" — /chart (the
// richer dashboard) took over the bare-IP landing spot, see handleStatusPage()
// below for why this stayed around at /status instead of being dropped.
void addRow(String& html, const String& label, const String& value)
{
    html += "<tr><td>" + label + "</td><td>" + value + "</td></tr>";
}

// Plain, JS-free status table — kept at /status (moved off "/" when /chart
// took over the root) precisely because it has no canvas/fetch dependency:
// a fast, dependency-free health check for curl/monitoring scripts or a
// JS-disabled browser, plus the WiFi/MQTT/LoRaWAN connectivity fields that
// don't fit the weather-dashboard framing of /chart.
void handleStatusPage()
{
    JsonDocument doc;
    buildJsonPayload(doc);

    const char* name = gCfg.deviceName[0] ? gCfg.deviceName : "WeatherNode";

    String html;
    html.reserve(2560);
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>"; html += name; html += " status</title></head><body>";
    html += "<h3>"; html += name; html += " status</h3>";
    html += "<p>Firmware: " FIRMWARE_VERSION "</p>";

    html += "<h4>Measurements</h4><table>";
    if (gCfg.windEn) {
        addRow(html, "Wind direction",     String(doc["windDirDeg"].as<int>()) + "&deg;");
        addRow(html, "Wind speed",         String(doc["windSpeed"].as<float>(), 1) + " " + doc["windUnit"].as<const char*>());
        addRow(html, "Wind gust",          String(doc["windGust"].as<float>(),  1) + " " + doc["windUnit"].as<const char*>());
    }
    if (gCfg.rainEn) {
        addRow(html, "Rain rate",          String(doc["rainRate"].as<float>(), 2)       + " " + doc["rainRateUnit"].as<const char*>());
        addRow(html, "Rain (this cycle)",  String(doc["rain"].as<float>(), 2)           + " " + doc["rainUnit"].as<const char*>());
        addRow(html, "Rain since start",   String(doc["rainSinceStart"].as<float>(), 2) + " " + doc["rainUnit"].as<const char*>());
    }
    if (bme280Present) {
        addRow(html, "Temperature",        String(doc["temp"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>());
        addRow(html, "Humidity",           String(doc["humidityPct"].as<float>(), 1) + " %");
        addRow(html, "Pressure (absolute)",String(doc["pressureAbs"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
        addRow(html, "Pressure (QNH)",     String(doc["pressureQnh"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
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
    html += "</table>";

    html += "<h4>Status</h4><table>";
    if (gCfg.debugMode) addRow(html, "Measurement cycle", String(doc["cycle"].as<int>()));
    addRow(html, "Next measurement in", String(doc["nextCycleInSec"].as<uint32_t>()) + " s");
    addRow(html, "BME280 sensor",      bme280Present ? "present" : "not detected");
    addRow(html, "DS18B20 sensors",    String(bus1Count));
    addRow(html, "Uplink send failures", String(sendFailCount));
    addRow(html, "Uptime",             String((float)millis() / 3600000.0f, 2) + " h");
    addRow(html, "Free heap",          String((float)ESP.getFreeHeap() / 1024.0f, 1) + " KiB");
    addRow(html, "LoRaWAN",            gCfg.loraEn ? "enabled" : "disabled");
    addRow(html, "WiFi",               wifiIsConnected() ? ("connected (" + WiFi.localIP().toString() + ")") : "disconnected");
    addRow(html, "MQTT",               !gCfg.mqttEn ? "disabled" :
                                        (mqttIsConnected() ? ("connected (" + String(gCfg.mqttHost) + ":" + String(gCfg.mqttPort) + ")") : "disconnected"));
    html += "</table>";

    html += "<p><a href='/'>Chart</a> &middot; <a href='/api/settings'>Settings</a> &middot; <a href='/api/log'>Log</a> &middot; "
            "<a href='/help'>Help / Documentation</a> &middot; <a href='/api/data'>Raw JSON</a></p>";
    html += "</body></html>";

    server.send(200, "text/html; charset=utf-8", html);
}

// Gates both viewing and saving the settings page behind HTTP Basic Auth
// (browser's built-in login prompt) using the admin password configured via
// the WiFi portal. No password set at all -> access is refused outright
// rather than prompting for one that can never match.
bool requireAuth()
{
    if (gCfg.webAdminPass[0] == '\0') {
        server.send(403, "text/plain", "Forbidden: no admin password configured (set one via the WiFi portal's \"WiFi + MQTT Parameter\" section).");
        return false;
    }
    if (!server.authenticate("admin", gCfg.webAdminPass)) {
        server.requestAuthentication(BASIC_AUTH, "WeatherNode settings");
        return false;
    }
    return true;
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
    if (!requireAuth()) return;
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

// Deliberately NOT a route into the full WiFiManager AP portal (which holds
// WiFi credentials, LoRaWAN AppKey, MQTT credentials/CA cert) — this exposes
// only the same operational parameters as applyParam() (interval,
// sensor enables, debug mode). Full portal stays reachable only via
// button-press / first-boot / LoRaWAN downlink 0x03.
void handleSettingsGet()
{
    if (!requireAuth()) return;

    String html;
    html.reserve(900);
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>WeatherNode settings</title></head><body>";
    html += "<h3>WeatherNode operational settings</h3>";
    html += "<p><a href='/status'>Status</a> &middot; <a href='/'>Chart</a> &middot; <a href='/api/log'>Log</a> &middot; <a href='/help'>Help / Documentation</a></p>";
    html += "<p><em>Only the operational settings below can be changed here. Everything "
            "else — WiFi credentials, static IP, LoRaWAN keys/region, MQTT broker/"
            "credentials, sensor calibration, etc. — requires the WiFi config portal "
            "(hold the button for 3&nbsp;s, first boot, or LoRaWAN downlink "
            "<code>0x03</code>).</em></p>";
    html += "<form method='POST' action='/api/settings'>";
    html += "Uplink interval (s): <input name='uplinksec' value='" + String(gCfg.uplinkIntervalSec) + "'><br>";
    html += "Sample interval (s): <input name='samplesec' value='" + String(gCfg.sampleSec) + "'><br>";
    html += "Wind sensor enabled: <input type='checkbox' name='winden'" + String(gCfg.windEn ? " checked" : "") + "><br>";
    html += "Rain sensor enabled: <input type='checkbox' name='rainen'" + String(gCfg.rainEn ? " checked" : "") + "><br>";
    html += "Debug mode: <input type='checkbox' name='debugmode'" + String(gCfg.debugMode ? " checked" : "") + "><br>";
    html += "Imperial units (°F, in, inHg, mph): <input type='checkbox' name='imperial'" + String(gCfg.unitSystem ? " checked" : "") + "><br><br>";
    html += "<input type='submit' value='Save'>";
    html += "</form></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

void handleSettingsPost()
{
    if (!requireAuth()) return;

    // Collect rejected fields instead of silently ignoring them.
    String rejected;
    auto apply = [&rejected](uint8_t id, long value, const char* label) {
        if (!applyParam(id, clampToParamValue(value))) {
            if (rejected.length()) rejected += ", ";
            rejected += label;
        }
    };
    if (server.hasArg("uplinksec")) apply(PARAM_UPLINK_INTERVAL_SEC, server.arg("uplinksec").toInt(), "uplink interval (5-3600 s)");
    if (server.hasArg("samplesec")) apply(PARAM_SAMPLE_SEC, server.arg("samplesec").toInt(), "sample interval (1 s - uplink interval)");
    apply(PARAM_WIND_EN,     server.hasArg("winden") ? 1 : 0,    "wind sensor");
    apply(PARAM_RAIN_EN,     server.hasArg("rainen") ? 1 : 0,    "rain sensor");
    apply(PARAM_UNIT_SYSTEM, server.hasArg("imperial") ? 1 : 0,  "unit system");
    apply(PARAM_DEBUG_MODE,  server.hasArg("debugmode") ? 1 : 0, "debug mode");
    saveConfig(gCfg);

    if (rejected.length()) {
        server.send(400, "text/html; charset=utf-8",
                    "<!DOCTYPE html><html><body><p>Invalid value(s), not changed: " + rejected +
                    "</p><p>All other settings were saved. <a href='/api/settings'>Back to settings</a></p></body></html>");
        return;
    }
    server.sendHeader("Location", "/api/settings");
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
