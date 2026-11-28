#include "api_info.h"
#include "lsensors.h"
#include "out_status.h"
#include "psram.h"
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif
#if __has_include(<nvs.h>) && defined(ESP_PLATFORM)
#include <nvs.h>
#endif

// alarms_run.cpp / custom_upload.cpp (weak defaults: host builds without them)
__attribute__((weak)) void alarmChannelsConfigured(bool* ch) { for (int k = 0; k < 4; k++) ch[k] = false; }
__attribute__((weak)) void customUploadsConfigured(bool* cu) { cu[0] = cu[1] = false; }
#include "wifi_ap.h"
// wifi_ap.cpp (weak default: host builds without it)
__attribute__((weak)) void apStatus(ApStatus& out) { out = ApStatus(); }
#include "https_cert.h"
// https_esp.cpp (weak default: host builds without it)
__attribute__((weak)) void httpsStatus(HttpsStatus& out) { out = HttpsStatus(); }
#include "gnss.h"
// gnss.cpp (weak defaults: host builds without it)
__attribute__((weak)) void gnssStatus(GnssStatus& out) { out = GnssStatus(); }
__attribute__((weak)) bool gnssActive() { return false; }
#include "sensors.h"
#include "globals.h"
#include "units.h"
#include "board_io.h"
#include "heltec_board.h"
#include "sources.h"
#include "rf_sensors.h"
#include "vsensor.h"
#include "vs_store.h"
#include "reset_history.h"
#include "history.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "wind_window.h"
#include "influx_queue.h"
// wx_upload.cpp (weak default: host builds without it)
__attribute__((weak)) void influxBacklogStatus(InfluxBacklogStatus& out) { out = InfluxBacklogStatus(); }
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

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

// Internal RAM: 0 free, 1 minimum free since start, 2 largest free block;
// PSRAM (0 without): 3 total, 4 free, 5 largest block. Weak: host tests
__attribute__((weak)) size_t hostOrDeviceFreeHeap(int what)
{
#if __has_include(<esp_heap_caps.h>) && defined(ESP_PLATFORM)
    switch (what) {
        case 0:  return internalFree();   // internal RAM: PSRAM separately (psram.h)
        case 1:  return internalMinFree();
        case 3:  return psramTotal();
        case 4:  return psramFree();
        case 5:  return psramLargest();
        default: return internalLargest();
    }
#else
    return what == 0 ? 150000 : what == 1 ? 120000 : what >= 3 ? 0 : 90000;
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

// Task stack never used so far (bytes, FreeRTOS high-water mark), -1 = no
// such task; weak: host tests. Tasks: loopTask (24 KB), webApi (8 KB),
// wxUpload (12 KB) - a value near 0 is the warning before a stack overflow.
__attribute__((weak)) int hostOrDeviceStackFree(const char* task)
{
#if defined(ESP_PLATFORM)
    TaskHandle_t h = xTaskGetHandle(task);
    return h ? (int)uxTaskGetStackHighWaterMark(h) : -1;
#else
    return !strcmp(task, "loopTask") ? 15000 : !strcmp(task, "httpd") ? 2800 : 6500;
#endif
}

// A core dump of the last crash is in flash (admin download); weak: host tests
__attribute__((weak)) bool hostOrDeviceCoreDumpStored()
{
#if defined(ESP_PLATFORM)
    size_t a, n;
    return resetHistoryCoreDump(a, n);
#else
    return true;
#endif
}

namespace {
void putUtc(JsonObject o, const char* key, uint32_t epoch)
{
    const time_t t = (time_t)epoch;
    struct tm u;
    gmtime_r(&t, &u);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &u);
    o[key] = (const char*)iso;   // const char*: copied (a char array would be kept by pointer)
}

struct Hex {   // "0x4200abcd"
    char s[12];
    explicit Hex(uint32_t x) { snprintf(s, sizeof(s), "0x%08lx", (unsigned long)x); }
    const char* str() const { return s; }   // const char*: ArduinoJson copies it
};

// Reset history and the last crash (reset_history.h)
void putResets(JsonObject sys)
{
    const ResetHistory& h = resetHistory();
    sys["starts"] = h.starts;
    JsonArray a = sys["resets"].to<JsonArray>();
    for (int i = 0; i < h.count && i < RESET_HISTORY_LEN; i++) {
        const ResetRecord& r = h.rec[i];
        JsonObject e = a.add<JsonObject>();
        e["cause"] = resetCauseKey(r.cause);
        if (r.flags & RR_UPTIME_KNOWN) e["uptimeSec"] = r.uptimeSec;
        if (r.flags & RR_TIME_KNOWN) putUtc(e, "time", r.epoch);
        if (r.detail[0]) e["detail"] = (const char*)r.detail;
        if (r.flags & RR_CRASH) e["crash"] = true;
    }
    const CrashSummary& c = h.lastCrash;
    if (!c.valid) return;
    JsonObject k = sys["lastCrash"].to<JsonObject>();
    if (c.epoch) putUtc(k, "time", c.epoch);
    k["task"] = (const char*)c.task;
    k["pc"] = Hex(c.pc).str();
    JsonArray bt = k["backtrace"].to<JsonArray>();
    for (int i = 0; i < c.depth && i < 8; i++) bt.add(Hex(c.backtrace[i]).str());
    if (c.backtraceCorrupted) k["backtraceCorrupted"] = true;
    if (c.reason[0]) k["reason"] = (const char*)c.reason;
    // an abort / assert / watchdog brings its own text; only a CPU exception has none - then its cause
    if (const char* ex = c.reason[0] ? nullptr : xtensaExceptionName(c.exceptionCause)) {
        k["exception"] = ex;
        k["exceptionAddress"] = Hex(c.exceptionAddress).str();
    }
    k["firmware"] = (const char*)c.firmware;
    k["elfSha"]   = (const char*)c.elfSha;
    k["coreDump"] = hostOrDeviceCoreDumpStored();
}
} // namespace

void buildInfoJson(JsonDocument& doc, bool withPosition)
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
    if (const size_t ps = hostOrDeviceFreeHeap(3)) {   // only a board whose PSRAM runs (V4)
        JsonObject p = sys["psram"].to<JsonObject>();
        p["total"]   = (unsigned long)ps;
        p["free"]    = (unsigned long)hostOrDeviceFreeHeap(4);
        p["largest"] = (unsigned long)hostOrDeviceFreeHeap(5);
    }
    sys["settingsStoreUsed"] = (unsigned long)hostOrDeviceSettingsStore(false);
    sys["settingsStoreFree"] = (unsigned long)hostOrDeviceSettingsStore(true);
    {   // stack never used per task (bytes)
        JsonObject sf = sys["stackFree"].to<JsonObject>();
        static const char* const tasks[][2] = { { "loop", "loopTask" }, { "web", "httpd" }, { "wxUpload", "wxUpload" } };
        for (const auto& t : tasks) {
            const int v = hostOrDeviceStackFree(t[1]);
            if (v >= 0) sf[t[0]] = v;
        }
    }
    putResets(sys);
    // Language setting of the web interface (a UI may follow it); "auto" = browser
    static const char* const langs[] = { "auto", "en", "de", "fr", "es", "pt" };
    dev["language"] = langs[gCfg.uiLang >= 0 && gCfg.uiLang <= 5 ? gCfg.uiLang : 0];

    // Web UI options (Settings -> System -> Web UI): the dashboard reads them once
    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["flashOnChange"] = gCfg.uiFlashEn;
    ui["showAge"]       = gCfg.uiAgeEn;

    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"] = FIRMWARE_VERSION;
    char upd[24];
    wxUpdateAvailable(upd, sizeof(upd));
    if (upd[0]) fw["updateAvailable"] = upd;

    JsonObject hw = doc["hardware"].to<JsonObject>();
    hw["board"]  = "V4";
    if (gBoard.fem != FEM_NONE) hw["boardFem"] = femName(gBoard.fem);
    hw["pcbRev"] = boardPcbAllowed() ? gCfg.pcbVersion : 0;   // same as status.pcbRev in /api/data
    {   // BME280 pins (heltec_board.h bmePinSelect)
        int sda, scl;
        bool oldWiring;
        uint8_t addr;
        sensorsBmePins(sda, scl, oldWiring, addr);
        JsonArray bp = hw["bme280Pins"].to<JsonArray>();
        bp.add(sda);
        bp.add(scl);
        hw["bme280OldPins"] = oldWiring;
    }
    if (gnssActive()) {   // GNSS module (gnss.h)
        GnssStatus g;
        gnssStatus(g);
        JsonObject gn = hw["gnss"].to<JsonObject>();
        gn["seen"] = g.seen;
        gn["baud"] = g.baud;
        gn["fix"] = g.fix;
        gn["fixQuality"] = g.quality;
        gn["satellites"] = g.sats;
        if (g.hdop > 0) gn["hdop"] = roundf(g.hdop * 10.0f) / 10.0f;
        if (g.posOk && withPosition) {   // the position: admin only (api_info.h)
            gn["lat"] = round(g.lat * 1e5) / 1e5;
            gn["lon"] = round(g.lon * 1e5) / 1e5;
        }
        if (g.altOk) gn["altitudeM"] = roundf(g.altM * 10.0f) / 10.0f;
        gn["pps"] = g.pps;
        if (g.timeUsed) {
            gn["timeAgeSec"] = g.timeAgeSec;
            gn["timePps"] = g.timePps;
        }
        gn["takeTime"] = gCfg.gnssTime != 0;
        gn["takePosition"] = gCfg.gnssPos != 0;
        gn["takeAltitude"] = gCfg.gnssAlt != 0;
    }

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
    {   // time source (time_sync.h)
        TimeSyncStatus ts;
        timeSyncStatus(ts);
        m["clockSource"] = timeSourceName(ts.source);
        if (ts.source != TSRC_NONE) {
            m["clockSourceAgeSec"] = ts.ageSec;
            m["clockSourceFresh"] = ts.fresh;
        }
        if (ts.loraNextSec >= 0) m["loraTimeReqInSec"] = ts.loraNextSec;
    }

    JsonArray ranges = doc["history"]["ranges"].to<JsonArray>();
    for (const HistoryRangeDef& rd : HISTORY_RANGES) {
        int tier;
        uint32_t group;
        historyPlan(rd.sec, rd.sec, -1, tier, group);
        JsonObject r = ranges.add<JsonObject>();
        r["range"] = rd.name;
        r["bucketSec"] = historyTierBucketSec(tier) * group;
        r["maxPoints"] = historyRangeMaxPoints(rd.sec);
        r["res"] = HISTORY_TIER_NAMES[tier];
    }
    JsonArray tiers = doc["history"]["tiers"].to<JsonArray>();
    for (int i = 0; i < HT_COUNT; i++) {
        JsonObject r = tiers.add<JsonObject>();
        r["res"] = HISTORY_TIER_NAMES[i];
        r["bucketSec"] = historyTierBucketSec(i);
        r["capacity"] = historyTierCapacity(i);
    }

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
    out["influxdb"]     = wifi && gCfg.influxEn && gCfg.influxUrl[0];
    {   // own access point (wifi_ap.h): mode, running, devices on it
        ApStatus ap;
        apStatus(ap);
        out["accessPoint"]       = apModeName(ap.mode);
        out["accessPointActive"] = ap.active;
        if (ap.active) out["accessPointClients"] = ap.clients;
    }
    {   // HTTPS (https_cert.h): off / on / why it did not start; running: whose certificate
        HttpsStatus hs;
        httpsStatus(hs);
        out["https"] = httpsStateName((HttpsState)hs.state);
        if (hs.state == HTTPS_RUNNING) out["httpsCertificate"] = httpsCertSourceName((HttpsCertSource)hs.source);
    }
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
        const OutStatus* os = outSnapshotForWebTask();   // send status per output (out_status.h)
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
        on[OUT_INFLUX] = wifi && gCfg.influxEn && gCfg.influxUrl[0];
        JsonObject stat = out["status"].to<JsonObject>();
        const uint32_t now = millis();
        for (uint8_t i = 0; i < OUT_COUNT; i++)
            if (on[i]) outStatusJson(stat[outStatusKey(i)].to<JsonObject>(), os[i], now);
        if (on[OUT_INFLUX]) {   // backfill queue (influx_queue.h)
            InfluxBacklogStatus ib;
            influxBacklogStatus(ib);
            if (ib.active) {
                JsonObject b = stat["influxdb"]["backlog"].to<JsonObject>();
                b["pushes"]          = ib.pushes;
                b["bytes"]           = ib.bytes;
                b["capacity"]        = ib.capacity;
                int64_t e;
                if (ib.oldestEpoch && epochNow(e) && e >= (int64_t)ib.oldestEpoch) b["oldestSecAgo"] = (uint32_t)(e - ib.oldestEpoch);
                b["droppedFull"]     = ib.droppedFull;
                b["droppedRejected"] = ib.droppedRejected;
            }
        }
    }

    // Value groups /api/data currently carries (same conditions as
    // buildJsonPayload()) - a UI builds its tiles from this.
    const bool v2 = ioFull();
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
    ws["mine"]          = lsMineCount();
    ws["hidden"]        = (int)rfParseIds(gCfg.rfIgnore).size();
    ws["hiddenInRange"] = rfHiddenInRange(millis());

    // Virtual sensors (vsensor.h), only while switched on: per configured
    // slot its type, derived ID, value names and counters; band airtime.
    VsSettingsCopy vsCfg = vsSwitchedOn() ? vsSettingsCopy() : VsSettingsCopy();
    if (vsCfg) {
        JsonObject vs = doc["virtualSensors"].to<JsonObject>();
        vs["txPowerDbm"]        = vsCfg->txPowerDbm;
        vs["frequencyMHz"]      = VS_FREQ_MHZ;
        vs["airtimeLastHourMs"] = (uint32_t)gVsBandLastHourMs;
        vs["airtimeBudgetMs"]   = VS_BAND_BUDGET_MS;
        if (rfDavisMode(gCfg.rfEn, gCfg.rfProto)) {   // frames go in the gaps after Davis packets (vs_run.h)
            vs["davisGapFrames"] = (uint32_t)gVsDavisGapFrames;
            vs["davisGapMaxMs"]  = (uint32_t)((gVsDavisGapMaxUs + 999) / 1000);
        }
        uint8_t mac[6];
        const uint64_t m = ESP.getEfuseMac();   // byte 0 in the lowest bits, as vsNodeMac()
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(m >> (8 * i));
        JsonArray sl = vs["slots"].to<JsonArray>();
        const uint32_t now = millis();
        for (int i = 0; i < VS_SLOTS; i++) {
            const VsSlotCfg& c = vsCfg->slot[i];
            if (c.type == VS_OFF || c.type >= VS_TYPE_COUNT) continue;
            const VsSlotStatus& st = gVsStatus[i];
            JsonObject e = sl.add<JsonObject>();
            e["slot"]    = i + 1;
            e["type"]    = VS_TYPES[c.type].key;
            e["enabled"] = (bool)c.enabled;
            char id[12];
            snprintf(id, sizeof(id), "%08lX", (unsigned long)vsSlotTableId(c, i, mac));
            e["id"] = id;
            // a received sensor has the same ID (vs_run.h vsIdReceived(); this file is host-built without vs_run)
            for (const RfSensorReading& r : gRfSensors) if (r.used && vsIdCollides(c, i, mac, r.id)) { e["idReceived"] = true; break; }
            if (c.type == VS_WH31) e["channel"] = c.channel;
            JsonObject f = e["fields"].to<JsonObject>();
            // (const char*): copied - a char array would be kept by pointer (ArduinoJson 7), and vsCfg is freed before serializing
            for (int k = 0; k < VF_COUNT; k++) if (c.src[k][0]) f[vsFieldKey((uint8_t)k)] = (const char*)c.src[k];
            e["sent"] = st.sent;
            if (st.sent) e["lastTxSecAgo"] = (now - st.lastTxMs) / 1000;
            JsonObject k = e["skipped"].to<JsonObject>();
            k["airtime"]      = st.skipBudget;
            k["radioBusy"]    = st.skipBusy;
            k["valueMissing"] = st.skipMissing;
            k["deferredDavis"] = st.deferDavis;
            if (st.lastError) e["lastError"] = st.lastError;
        }
    }

    JsonObject src = doc["sources"].to<JsonObject>();
    char d[24];
    for (int g = 0; g < SG_COUNT; g++) { srcDescribe((SrcGroup)g, d, sizeof(d)); src[srcGroupKey((SrcGroup)g)] = d; }
}
