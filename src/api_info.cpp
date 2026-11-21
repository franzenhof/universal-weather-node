#include "api_info.h"
#include "out_status.h"
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif
#if __has_include(<nvs.h>) && defined(ESP_PLATFORM)
#include <nvs.h>
#endif

// alarms_run.cpp / custom_upload.cpp (weak defaults: host builds without them)
__attribute__((weak)) void alarmChannelsConfigured(bool* ch) { for (int k = 0; k < 4; k++) ch[k] = false; }
__attribute__((weak)) void customUploadsConfigured(bool* cu) { cu[0] = cu[1] = false; }
#include "globals.h"
#include "units.h"
#include "board_io.h"
#include "heltec_board.h"
#include "sources.h"
#include "rf_sensors.h"
#include "history.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "wind_window.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("generated_version.h")
#include "generated_version.h"
#endif
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

void hostnameFromName(const char* name, char* out, size_t len)
{
    if (!len) return;
    size_t n = 0;
    bool dash = false;
    for (const char* p = name; *p && n + 1 < len && n < HOSTNAME_MAX_LEN; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && n > 0) {
                // a '-' only with room for the character after it: never trailing
                if (n + 2 >= len || n + 2 > HOSTNAME_MAX_LEN) break;
                out[n++] = '-';
            }
            out[n++] = c;
            dash = false;
        } else {
            dash = true;   // collapsed; never leading or trailing
        }
    }
    out[n] = '\0';
}

void deviceHostname(char* out, size_t len)
{
    hostnameFromName(gCfg.deviceName, out, len);
    if (len && out[0] == '\0') {
        // Same MAC byte order as mqttClientId() (wifi_mqtt.cpp): the last
        // three bytes of the printed MAC.
        uint64_t mac = ESP.getEfuseMac();
        snprintf(out, len, "weathernode-%02x%02x%02x", (unsigned)((mac >> 24) & 0xFF),
                 (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
    }
}

// 0 free, 1 minimum free since start, 2 largest free block (8-bit capable heap); weak: host tests
__attribute__((weak)) size_t hostOrDeviceFreeHeap(int what)
{
#if __has_include(<esp_heap_caps.h>) && defined(ESP_PLATFORM)
    switch (what) {
        case 0:  return heap_caps_get_free_size(MALLOC_CAP_8BIT);
        case 1:  return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        default: return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }
#else
    return what == 0 ? 150000 : what == 1 ? 120000 : 90000;
#endif
}

// Settings partition "cfgdata" (NVS): bytes used / free (32-byte entries); weak: host tests
__attribute__((weak)) size_t hostOrDeviceSettingsStore(bool freePart)
{
#if __has_include(<nvs.h>) && defined(ESP_PLATFORM)
    nvs_stats_t st;
    if (nvs_get_stats("cfgdata", &st) != ESP_OK) return 0;
    return (freePart ? st.free_entries : st.used_entries) * 32;
#else
    return freePart ? 45000 : 14000;
#endif
}

void buildInfoJson(JsonDocument& doc)
{
    doc.clear();
    doc["apiVersion"] = API_VERSION;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["name"] = gCfg.deviceName;   // "" = none set
    char host[HOSTNAME_MAX_LEN + 1];
    deviceHostname(host, sizeof(host));
    dev["hostname"] = host;
    dev["uptimeSec"] = (unsigned long)(esp_timer_get_time() / 1000000LL);
    // memory (bytes): watch heapMin / heapLargest over days for a leak or fragmentation
    JsonObject sys = doc["system"].to<JsonObject>();
    sys["uptimeSec"]   = (unsigned long)(esp_timer_get_time() / 1000000LL);
    sys["heapFree"]    = (unsigned long)hostOrDeviceFreeHeap(0);
    sys["heapMin"]     = (unsigned long)hostOrDeviceFreeHeap(1);
    sys["heapLargest"] = (unsigned long)hostOrDeviceFreeHeap(2);
    sys["settingsStoreUsed"] = (unsigned long)hostOrDeviceSettingsStore(false);
    sys["settingsStoreFree"] = (unsigned long)hostOrDeviceSettingsStore(true);
    // Language setting of the web interface (a UI may follow it); "auto" = browser
    static const char* const langs[] = { "auto", "en", "de", "fr", "es", "pt" };
    dev["language"] = langs[gCfg.uiLang >= 0 && gCfg.uiLang <= 5 ? gCfg.uiLang : 0];

    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"] = FIRMWARE_VERSION;
    char upd[24];
    wxUpdateAvailable(upd, sizeof(upd));
    if (upd[0]) fw["updateAvailable"] = upd;

    JsonObject hw = doc["hardware"].to<JsonObject>();
    hw["board"]  = boardIsR8() ? "V4-R8" : boardName(gBoard.kind);
    if (gBoard.fem != FEM_NONE) hw["boardFem"] = femName(gBoard.fem);
    hw["pcbRev"] = boardPcbAllowed() ? gCfg.pcbVersion : 0;   // same as status.pcbRev in /api/data

    const bool imperial = gCfg.unitSystem == 1;
    JsonObject u = doc["units"].to<JsonObject>();
    u["system"]    = imperial ? "imperial" : "metric";
    u["temp"]      = temperatureUnitLabel(gCfg);
    u["pressure"]  = pressureUnitLabel(gCfg);
    u["wind"]      = windSpeedUnitLabel(gCfg);
    u["rain"]      = rainUnitLabel(gCfg);
    u["rainRate"]  = rainRateUnitLabel(gCfg);
    u["cloudBase"] = imperial ? "ft" : "m";
    u["tempTrend"] = imperial ? "F" : "K";

    JsonObject m = doc["measurement"].to<JsonObject>();
    m["cycleSec"] = gBaseCycleMs ? (unsigned long)(gBaseCycleMs / 1000) : (unsigned long)gCfg.uplinkIntervalSec;
    m["windSampleSec"] = windSampleSecNormalized(gCfg.sampleSec);
    m["mqttWindAggregation"] = gCfg.mqttWindAgg == 1 ? "10min" : "interval";
    m["altitudeM"] = gCfg.altitudeM;
    m["clockSynced"] = timeIsValid();
    m["timeZone"] = gCfg.timeZone;

    JsonArray ranges = doc["history"]["ranges"].to<JsonArray>();
    JsonObject r24 = ranges.add<JsonObject>();
    r24["range"] = "24h"; r24["bucketSec"] = HISTORY_FINE_BUCKET_SEC; r24["maxPoints"] = HISTORY_FINE_CAPACITY;
    JsonObject r7 = ranges.add<JsonObject>();
    r7["range"] = "7d"; r7["bucketSec"] = HISTORY_COARSE_BUCKET_SEC; r7["maxPoints"] = HISTORY_COARSE_CAPACITY;

    // Outputs/integrations: on/off only - never hosts, IDs or keys.
    const bool wifi = gCfg.wifiEn;
    JsonObject out = doc["outputs"].to<JsonObject>();
    out["wifi"]         = wifi;
    out["webApi"]       = wifi && gCfg.webApiEn;
    out["mqtt"]         = wifi && gCfg.mqttEn;
    out["mqttCommands"] = wifi && gCfg.mqttEn && gCfg.mqttCtrlEn;
    out["lorawan"]      = (bool)gCfg.loraEn;
    out["loxone"]       = wifi && gCfg.loxEn;
    out["updateCheck"]  = wifi && gCfg.updateCheckEn;
    JsonArray wx = out["weatherNetworks"].to<JsonArray>();
    if (wifi) {   // same enable rule as wx_upload.cpp
        if (gCfg.awekasUser[0])                        wx.add("awekas");
        if (gCfg.wuStationId[0])                       wx.add("wunderground");
        if (gCfg.wcloudId[0])                          wx.add("weathercloud");
        if (gCfg.wnetproId[0])                         wx.add("wetternetzwerk");
        if (gCfg.windyId[0])                           wx.add("windy");
        if (gCfg.osemBoxId[0] && gCfg.osemSensors[0])  wx.add("opensensemap");
        if (gCfg.wowSiteId[0] && gCfg.wowPin[0])       wx.add("wow");
        if (gCfg.cwopCall[0] && gCfg.latitude[0] && gCfg.longitude[0]) wx.add("cwop");
    }
    // Send status of every enabled output (out_status.h): times as age, short
    // local reason / trimmed server reply, never URLs or keys.
    {
        static OutStatus os[OUT_COUNT];   // static: ~600 bytes, not on the web task's stack
        outSnapshot(os);
        bool on[OUT_COUNT] = { (bool)gCfg.loraEn, wifi && gCfg.mqttEn, wifi && gCfg.mqttEn, wifi && gCfg.loxEn,
            wifi && gCfg.awekasUser[0], wifi && gCfg.wuStationId[0], wifi && gCfg.wcloudId[0], wifi && gCfg.wnetproId[0],
            wifi && gCfg.windyId[0], wifi && gCfg.osemBoxId[0] && gCfg.osemSensors[0],
            wifi && gCfg.wowSiteId[0] && gCfg.wowPin[0], wifi && gCfg.cwopCall[0] && gCfg.latitude[0] && gCfg.longitude[0],
            false, false, false, false, false, false };
        {   // alarm channels and custom uploads: configured = listed
            bool ch[4] = {}, cu[2] = {};
            alarmChannelsConfigured(ch);
            customUploadsConfigured(cu);
            for (int k = 0; k < 4; k++) on[OUT_NOTIFY_FIRST + k] = wifi && ch[k];
            for (int k = 0; k < 2; k++) on[OUT_CUSTOM_FIRST + k] = wifi && cu[k];
        }
        JsonObject stat = out["status"].to<JsonObject>();
        const uint32_t now = millis();
        for (uint8_t i = 0; i < OUT_COUNT; i++)
            if (on[i]) outStatusJson(stat[outStatusKey(i)].to<JsonObject>(), os[i], now);
    }

    // Value groups /api/data currently carries (same conditions as
    // buildJsonPayload()) - a UI builds its tiles from this.
    const bool v2 = ioV2();
    JsonObject a = doc["available"].to<JsonObject>();
    a["temp"]            = gSrc.tempValid;
    a["humidity"]        = gSrc.tempValid && gSrc.humValid;
    a["pressure"]        = gSrc.pressValid;
    a["wind"]            = srcActive(SG_WIND);
    a["rain"]            = srcActive(SG_RAIN);
    a["uvIndex"]         = gSrc.uvRadio || (v2 && gSrc.uvBoard && gIo.uvIndexCalibrated);
    a["light"]           = gSrc.lightValid;
    a["ds18b20Probes"]   = bus1Count;
    a["battery"]         = (bool)gCfg.lipoEn;
    a["wirelessSensors"] = (bool)gCfg.rfEn;
    a["uvSensorVolts"]   = v2 && gSrc.uvBoard;
    a["wind2"]           = v2 && gCfg.wind2Mode != 0;
    a["auxAnalog"]       = v2 && ioAuxAinActive();
    a["auxPulses"]       = v2 && gCfg.auxPulseEn && !gCfg.wind2Mode;
    a["heater"]          = v2 && gCfg.heaterMode != 0;
    a["fan"]             = v2 && (gCfg.fanMode || gCfg.tachoEn);
    a["victron"]         = v2 && gCfg.veDirectEn;

    // Wireless sensor states (counts only; the IDs are in /api/data rfSensors)
    JsonObject ws = doc["wireless"].to<JsonObject>();
    ws["onlyMine"]      = rfOnlyMineActive();
    ws["mine"]          = (int)rfParseIds(gCfg.rfIds).size();
    ws["hidden"]        = (int)rfParseIds(gCfg.rfIgnore).size();
    ws["hiddenInRange"] = rfHiddenInRange(millis());

    JsonObject src = doc["sources"].to<JsonObject>();
    char d[24];
    for (int g = 0; g < SG_COUNT; g++) { srcDescribe((SrcGroup)g, d, sizeof(d)); src[srcGroupKey((SrcGroup)g)] = d; }
}
