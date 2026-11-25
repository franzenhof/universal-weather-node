#include "json_payload.h"
#include "globals.h"
#include "units.h"
#include <Arduino.h>
#include <DallasTemperature.h>
#include "board_io.h"
#include "heltec_board.h"
#include "sources.h"
#include "rf_sensors.h"
#include "davis_diag.h"
#include "derived_now.h"
#include "sensors.h"
#include "api_info.h"
#include "records.h"
#include "astro.h"
#include "alarms.h"
#include "rf_stats.h"
#include "rf_names.h"
#include <esp_timer.h>
#include <time.h>

const char* gResetReason = "unknown";
unsigned long gBaseCycleMs = 0;   // base cycle length, set by loop() (0 = not yet known)

// Unit-neutral field names (temp, pressureAbs, ...) — the actual unit used
// depends on gCfg.unitSystem and is always given by a sibling "*Unit" field
// (same pattern as the pre-existing windUnit), so consumers don't need to
// hardcode an assumption about which system is active.
// Derived values (derived_now.h): dew point, feels-like, cloud base, trends,
// rolling and daily rain, sunshine, warnings, firmware update.
static void addDerivedJson(JsonDocument& doc, float windMs, int dirDeg)
{
    DerivedNow d;
    derivedNow(d, windMs, dirDeg);
    bool imperial = gCfg.unitSystem == 1;
    if (d.dewValid)   doc["dewPoint"]  = roundf(temperatureForOutput(d.dewPointC, gCfg) * 10.0f) / 10.0f;
    if (d.vpdValid) {
        doc["vpd"]     = roundf(d.vpdKpa * 100.0f) / 100.0f;
        doc["vpdUnit"] = "kPa";
    }
    if (d.feelsValid) doc["feelsLike"] = roundf(temperatureForOutput(d.feelsLikeC, gCfg) * 10.0f) / 10.0f;
    if (d.cloudValid) {
        doc["cloudBase"]     = (long)lroundf(imperial ? d.cloudBaseM * 3.28084f : d.cloudBaseM);
        doc["cloudBaseUnit"] = imperial ? "ft" : "m";
    }
    if (d.windDirText) doc["windDirText"] = d.windDirText;
    if (d.pressTrendValid) {
        doc["pressureTrend3h"]  = roundf(pressureForOutput(d.pressDelta3hHpa, gCfg) * 100.0f) / 100.0f;   // linear: fine for a difference
        doc["pressureTendency"] = pressureTendencyText(d.tendency);
        doc["weatherTrend"]     = d.weatherTrend;
    }
    if (d.tempTrendValid) doc["tempTrend1h"] = roundf(d.tempDelta1hC * (imperial ? 1.8f : 1.0f) * 10.0f) / 10.0f;
    if (d.rainValid) {
        doc["rain1h"]  = rainForOutput(d.rain1hMm, gCfg);
        doc["rain24h"] = rainForOutput(d.rain24hMm, gCfg);
        if (d.daily.valid) {
            doc["rainToday"] = rainForOutput(d.daily.rainTodayMm, gCfg);
            doc["rainWeek"]  = rainForOutput(d.daily.rainWeekMm, gCfg);    // ISO week, Monday start
            doc["rainMonth"] = rainForOutput(d.daily.rainMonthMm, gCfg);
            doc["rainYear"]  = rainForOutput(d.daily.rainYearMm, gCfg);
        }
        if (d.daily.yesterdayValid) doc["rainYesterday"] = rainForOutput(d.daily.rainYesterdayMm, gCfg);
        if (d.daily.lastRainEpoch > 0) {
            time_t t = (time_t)d.daily.lastRainEpoch;
            struct tm u; gmtime_r(&t, &u);
            char iso[24]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &u);
            doc["lastRain"] = iso;
        }
    }
    if (d.sunValid && d.daily.valid) {
        doc["sunshineTodayMin"] = (long)(d.daily.sunTodaySec / 60.0f);
        if (d.daily.yesterdayValid) doc["sunshineYesterdayMin"] = (long)(d.daily.sunYesterdaySec / 60.0f);
    }
    if (d.warnValid) {
        JsonObject w = doc["warnings"].to<JsonObject>();
        w["frost"]  = (d.warnings & WARN_FROST) != 0;
        w["fog"]    = (d.warnings & WARN_FOG) != 0;
        w["sultry"] = (d.warnings & WARN_SULTRY) != 0;
    }
    char upd[24];
    wxUpdateAvailable(upd, sizeof(upd));
    if (upd[0]) doc["firmwareUpdate"] = upd;
}

// One wireless sensor (rfSensors[] item, also the MQTT topic <prefix>/rf/<ID>).
void addRfSensorJson(JsonObject o, int i)
{
    const RfSensorReading& r = gRfSensors[i];
    char id[11];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)r.id);
    o["id"]        = id;
    uint32_t eco;
    if (r.proto == RF_PROTO_FINEOFFSET && rfEcowittId(r.id, eco)) {   // as the Ecowitt app / console shows it
        char e[8];
        snprintf(e, sizeof(e), "%lX", (unsigned long)eco);
        o["ecowittId"] = e;
    }
    const char* nm = rfNameOf(r.id);
    if (nm[0]) o["name"] = nm;   // user-defined (Settings → wireless sensor table)
    // wireless sensor number 1-32 (ID list order, else reception order); 1-4
    // are the value-source slots. Missing = not in the ID list (not possible
    // while the list filters the reception).
    int slot = rfSlotOfIndex(i);
    if (slot >= 0) o["slot"] = slot + 1;
    o["mine"] = rfStateOf(r.id) == RF_STATE_MINE;   // in the "Mine" list: numbered first, Home Assistant device
    o["type"]      = r.type;
    o["protocol"]  = rfProtoName(r.proto);
    if (r.model)   o["model"]   = r.model;      // Fine Offset: "WS90", "WH31", ...
    if (r.channel) o["channel"] = r.channel;    // WH31
    o["rssi"]      = (int)lroundf(r.rssi);
    // after a millis() wrap the raw difference is meaningless once the sensor is stale
    unsigned long age = (millis() - r.lastMs) / 1000;
    if ((r.staleMask & RF_VAL_ANY) && age < rfFreshMs(r) / 1000) age = rfFreshMs(r) / 1000;
    o["ageSec"]    = age;
    o["batteryOk"] = r.batteryOk;
    if (r.batteryMv) o["batteryV"] = roundf(r.batteryMv / 10.0f) / 100.0f;   // the voltage the sensor reports (diagnostic)
    if (r.hasTemp)     o["temp"]     = temperatureForOutput(r.tempC, gCfg);
    if (r.hasHumidity) o["humidityPct"] = r.humidity;
    if (r.hasWind) {
        o["windSpeed"]  = windSpeedForOutput(r.windAvgMs, gCfg);
        o["windGust"]   = windSpeedForOutput(r.windGustMs, gCfg);
        o["windDirDeg"] = (int)lroundf(r.windDirDeg);
    }
    if (r.hasRain)  o["rainTotal"] = rainForOutput(r.rainTotalMm, gCfg);   // the sensor's own counter (since battery change)
    RfStat* st = (r.hasRain || r.hasLightning) ? rfStatFor(r.id, false) : nullptr;
    uint32_t t = 0, ymd = 0;
    if (st) rfStatsClock(t, ymd);
    if (st && r.hasRain) {   // from the counter's increments since the node started (rf_stats.h)
        rfStatRoll(*st, t, ymd);
        const int dec = gCfg.unitSystem == 1 ? 2 : 1;
        const float k = dec == 2 ? 100.0f : 10.0f;
        if (st->ymd) o["rainToday"] = roundf(rainForOutput(st->rainTodayMm, gCfg) * k) / k;
        o["rain1h"] = roundf(rainForOutput(rfStatRain1h(*st, t), gCfg) * k) / k;
    }
    if (r.hasUv)    o["uvIndex"]   = r.uvIndex;
    if (r.hasLight) o["lightLux"]  = (long)lroundf(r.lightLux);
    if (r.hasMoisture) o["soilMoisturePct"] = r.moisturePct;
    if (r.hasMoisture && r.moistureAd != 0xFFFF) o["soilMoistureAd"] = r.moistureAd;   // WH51 raw AD (calibration)
    if (r.hasSolar) o["solarWm2"] = (long)lroundf(r.solarWm2);       // Davis: solar radiation
    if (r.hasSupercap) o["supercapV"] = roundf(r.supercapV * 10.0f) / 10.0f;   // only stations with one (WS85/WS90, Davis Vue)
    if (r.hasLightning) {
        o["lightningCount"] = r.lightningCount;   // the sensor's 8-bit counter (wraps): diagnostic
        if (r.lightningKm) o["lightningDistKm"] = r.lightningKm;
        if (st) {
            rfStatRoll(*st, t, ymd);
            if (st->ymd) o["lightningToday"] = st->strikesToday;
            o["lightning1h"] = rfStatStrikes1h(*st, t);
            if (st->lastStrike) {
                time_t tt = (time_t)st->lastStrike;
                struct tm u; gmtime_r(&tt, &u);
                char iso[24]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &u);
                o["lastLightning"] = iso;
            }
        }
    }
    // Fine Offset per-sensor values (latest packet; no value-source groups)
    if (r.hasPressure) {   // station pressure (not reduced to sea level), in the node's pressure unit
        float p = pressureForOutput(r.pressureHpa, gCfg);
        o["pressure"] = gCfg.unitSystem == 1 ? roundf(p * 100.0f) / 100.0f : roundf(p * 10.0f) / 10.0f;
    }
    if (r.hasPm25) o["pm25"] = roundf(r.pm25 * 10.0f) / 10.0f;                  // ug/m3
    if (r.hasPm10) {
        o["pm10"] = roundf(r.pm10 * 10.0f) / 10.0f;
        if (r.pm10Estimated) o["pm10Estimated"] = true;
    }
    if (r.hasPm1) o["pm1"] = roundf(r.pm1 * 10.0f) / 10.0f;
    if (r.hasPm4) o["pm4"] = roundf(r.pm4 * 10.0f) / 10.0f;
    if (r.hasCo2) o["co2Ppm"] = r.co2Ppm;
    if (r.extPower) o["externalPower"] = true;
    if (r.hasLeak) {
        o["leak"] = r.leak;
        o["leakHighSensitivity"] = r.leakHighSensitivity;
        o["leakRaw"] = r.leakRaw;
    }
    if (r.hasLeafWetness) o["leafWetnessPct"] = r.leafWetnessPct;
    if (r.hasConductivity) o["conductivityUsCm"] = (long)lroundf(r.conductivityUsCm);
    if (r.hasDistance) {
        o["distanceMm"] = r.distanceMm;          // LDS01 laser distance (air gap)
        if (r.heaterActive) o["heaterActive"] = true;
    }
    if (r.hasHeatCount) o["heatCount"] = r.heatCount;   // LDS01 heater-on count (low byte)
    // values not received for more than 5 min (the sensor still sends others)
    const bool has[7] = { r.hasTemp, r.hasHumidity, r.hasWind, r.hasRain, r.hasUv, r.hasLight, r.hasPressure };
    static const char* const keys[7] = { "temp", "humidity", "wind", "rain", "uv", "light", "pressure" };
    JsonArray stale;
    for (int k = 0; k < 7; k++)
        if (has[k] && !srcRfValueFresh(i, keys[k])) {
            if (stale.isNull()) stale = o["stale"].to<JsonArray>();
            stale.add(keys[k]);
        }
}

// astro_store.cpp: weak default (host builds without it) = no location
__attribute__((weak)) bool astroJson(JsonObject) { return false; }
// alarms_run.cpp: weak defaults (host builds without it) = no alarms
__attribute__((weak)) int alarmsActiveCount() { return 0; }
__attribute__((weak)) void alarmsActiveJson(JsonArray) {}
// Today's records (records_store.cpp; this weak default = none, for host builds without it)
__attribute__((weak)) bool recordsToday(Records&) { return false; }

void buildJsonPayload(JsonDocument& doc, const PathValues* pv)
{
    doc.clear();
    PathValues g;
    if (!pv) {   // REST/status page: the base cycle's values (globals)
        g.windValid = windCycleValid;
        g.windSpeedMs = windSpeedMs; g.windGustMs = windGustMs; g.windDirDeg = windDirection;
        g.rainMm = rainMM; g.rainRateMmH = rainRateMmH;
        pv = &g;
    }
    doc["apiVersion"] = API_VERSION;   // api_info.h: additive changes only within one version

    if (srcActive(SG_WIND) && pv->windValid) {   // no valid sample in the window: left out
        doc["windDirDeg"] = pv->windDirDeg;
        doc["windSpeed"]  = windSpeedForOutput(pv->windSpeedMs, gCfg);
        doc["windGust"]   = windSpeedForOutput(pv->windGustMs,  gCfg);
        doc["windUnit"]   = windSpeedUnitLabel(gCfg);
    }
    if (srcActive(SG_RAIN)) {
        doc["rainRate"]        = rainRateForOutput(pv->rainRateMmH, gCfg);
        doc["rainRateUnit"]    = rainRateUnitLabel(gCfg);
        doc["rain"]            = rainForOutput(pv->rainMm, gCfg);
        doc["rainSinceStart"]  = rainForOutput(rainMMSinceStart, gCfg);
        doc["rainUnit"]        = rainUnitLabel(gCfg);
    }
    if (gSrc.tempValid) {   // selected source (BME280 or wireless sensor)
        doc["temp"]         = temperatureForOutput(gSrc.tempC, gCfg);
        doc["tempUnit"]     = temperatureUnitLabel(gCfg);
        if (gSrc.humValid) doc["humidityPct"] = gSrc.humidity;
    }
    if (alarmsActiveCount()) alarmsActiveJson(doc["alarms"].to<JsonArray>());   // active alarm rules (alarms.h)
    {   // sun and moon (astro.h): only with latitude / longitude and the clock
        JsonObject a = doc["astro"].to<JsonObject>();
        if (!astroJson(a)) doc.remove("astro");
    }
    {   // today's extremes (records.h) - the complete set: GET /api/records
        static Records today;   // static: ~430 bytes, not on the loop task's stack
        if (recordsToday(today)) {
            const RecVal& tn = today.mn[RQ_TEMP][RP_TODAY];
            const RecVal& tx = today.mx[RQ_TEMP][RP_TODAY];
            const RecVal& gx = today.mx[RQ_GUST][RP_TODAY];
            if (tn.t) doc["tempMinToday"] = roundf(temperatureForOutput(tn.v, gCfg) * 10.0f) / 10.0f;
            if (tx.t) doc["tempMaxToday"] = roundf(temperatureForOutput(tx.v, gCfg) * 10.0f) / 10.0f;
            if (gx.t) doc["gustMaxToday"] = roundf(windSpeedForOutput(gx.v, gCfg) * 10.0f) / 10.0f;
        }
    }
    if (gSrc.pressValid) {   // selected source: BME280 or a wireless sensor's station pressure (sources.pressure)
        doc["pressureAbs"]  = pressureForOutput(gSrc.pressAbsHpa, gCfg);
        doc["pressureQnh"]  = pressureForOutput(gSrc.pressSeaHpa, gCfg);
        doc["pressureUnit"] = pressureUnitLabel(gCfg);
    }
    if (bus1Count > 0) {
        // One entry per scanned slot, null for a slot that's since gone
        // disconnected — not skipped — so index i always means "probe i"
        // to every consumer (wifi_mqtt.cpp's Home Assistant discovery
        // templates hardcode ds18b20[i] per slot; a skipping array would
        // silently shift a later probe's reading into an earlier probe's
        // entity instead of going "unavailable" as intended).
        JsonArray ds18b20 = doc["ds18b20"].to<JsonArray>();
        for (int i = 0; i < bus1Count; i++)
            if (bus1Temps[i] != DEVICE_DISCONNECTED_C)
                ds18b20.add(temperatureForOutput(bus1Temps[i], gCfg));
            else
                ds18b20.add(nullptr);
        doc["ds18b20Unit"] = temperatureUnitLabel(gCfg);
    }
    if (gCfg.lipoEn) {
        doc["batteryVolt"] = batteryVolt;
    }

    // PCB v2.0 I/O block - each object only while its function is enabled.
    if (ioV2()) {
        if (gSrc.uvBoard) {
            doc["uvEnabled"] = true;
            doc["uvIndexCalibrated"] = gIo.uvIndexCalibrated;
            if (isfinite(gIo.uvSensorVolts)) {
                doc["uvSensorVolts"] = gIo.uvSensorVolts;
                doc["uvSensorVoltsUnit"] = "V";
            }
            if (gIo.uvIndexCalibrated && isfinite(gIo.uvIndex)) {
                doc["uvIndex"] = gIo.uvIndex;
                doc["uvIndexUnit"] = "index";
            }
        }
        if (ioAuxAinActive()) doc["auxAnalogV"] = gIo.auxAinV;
        if (gCfg.wind2Mode) {
            JsonObject w2 = doc["wind2"].to<JsonObject>();
            w2["speed"] = windSpeedForOutput(gIo.wind2SpeedMs, gCfg);
            w2["gust"]  = windSpeedForOutput(gIo.wind2GustMs,  gCfg);
            if (ioWind2DirActive()) w2["dirDeg"] = gIo.wind2DirDeg;
        }
        if (gCfg.auxPulseEn && !gCfg.wind2Mode) {
            doc["auxPulses"]      = gIo.auxPulsesCycle;
            doc["auxPulsesTotal"] = gIo.auxPulsesTotal;
        }
        if (gCfg.heaterMode) {
            JsonObject h = doc["heater"].to<JsonObject>();
            h["mode"]    = gCfg.heaterMode == 2 ? "auto" : "on";
            h["on"]      = gIo.heaterOn;
            h["dutyPct"] = gIo.heaterDuty;
        }
        if (gCfg.fanMode || gCfg.tachoEn) {
            JsonObject f = doc["fan"].to<JsonObject>();
            f["mode"]    = gCfg.fanMode == 2 ? "auto" : (gCfg.fanMode == 1 ? "on" : "off");
            f["on"]      = gIo.fanOn;
            f["dutyPct"] = gIo.fanDuty;
            if (gCfg.tachoEn) {
                f["rpm"]     = (int)lroundf(gIo.fanRpm);
                f["stalled"] = gIo.fanStalled;
            }
        }
        if (gCfg.veDirectEn) {
            JsonObject v = doc["victron"].to<JsonObject>();
            v["valid"] = gIo.veValid;
            if (gIo.veValid) {
                const VeDirectValues& ve = gIo.ve;
                if (!isnan(ve.battV))         v["batteryV"]      = ve.battV;
                if (!isnan(ve.battA))         v["batteryA"]      = ve.battA;
                if (!isnan(ve.pvV))           v["pvV"]           = ve.pvV;
                if (!isnan(ve.pvW))           v["pvW"]           = ve.pvW;
                if (!isnan(ve.yieldTodayKwh)) v["yieldTodayKWh"] = ve.yieldTodayKwh;
                if (!isnan(ve.socPct))        v["socPct"]        = ve.socPct;
                if (!isnan(ve.loadA))         v["loadA"]         = ve.loadA;
                if (ve.chargeState >= 0)      v["chargeState"]   = ve.chargeState;
                if (ve.error >= 0)            v["error"]         = ve.error;
            }
        }
    }

    if (gSrc.uvRadio) {
        doc["uvIndex"]     = gSrc.uvIndexRadio;
        doc["uvIndexUnit"] = "index";
    }
    if (gSrc.lightValid) doc["lightLux"] = gSrc.lightLux;
    if (gCfg.rfEn) {
        JsonObject src = doc["sources"].to<JsonObject>();
        char d[24];
        for (int g = 0; g < SG_COUNT; g++) { srcDescribe((SrcGroup)g, d, sizeof(d)); src[srcGroupKey((SrcGroup)g)] = d; }
        doc["radioBatteryLow"] = gSrc.rfBatteryLow;   // always (also false): a consumer that keeps the last value (Loxone) must see it clear
    }

    addDerivedJson(doc, pv->windSpeedMs, pv->windDirDeg);

    JsonObject status       = doc["status"].to<JsonObject>();
    status["bme280Present"] = bme280Present;
    status["bus1Present"]   = bus1Count > 0;
    status["sendFailCount"] = sendFailCount;
    status["pcbRev"]        = boardPcbAllowed() ? gCfg.pcbVersion : 0;   // 0 = no PCB (Heltec V4)
    status["board"]         = boardIsR8() ? "V4-R8" : boardName(gBoard.kind);
    if (gBoard.fem != FEM_NONE) status["boardFem"] = femName(gBoard.fem);
    status["uvEnabled"]     = ioV2() && ioUvActive();
    status["uvIndexCalibrated"] = ioV2() && ioUvActive() && gIo.uvIndexCalibrated;
    if (gIo.battLockout) status["batteryLowLockout"] = true;

    // Time left until the current measurement cycle completes and the next
    // transmit/publish happens (clamped to 0 — a cycle boundary can be
    // crossed between millis() here and the next loop() iteration).
    unsigned long sendTargetMs = gBaseCycleMs ? gBaseCycleMs : (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
    unsigned long elapsedMs    = millis() - lastSendMs;
    doc["nextCycleInSec"] = (elapsedMs < sendTargetMs) ? (sendTargetMs - elapsedMs) / 1000 : 0;

    // ESP32-S3 die temperature (typically 10-20 K above ambient) - for
    // spotting an overheating enclosure, not an air temperature.
    doc["cpuTemp"]     = temperatureForOutput(cpuTemperature, gCfg);
    doc["cpuTempUnit"] = temperatureUnitLabel(gCfg);
    doc["resetReason"] = gResetReason;

    // Wireless sensors (rf_sensors.cpp): latest values per sensor, units as configured.
    if (gCfg.rfEn) {
        JsonArray rf = doc["rfSensors"].to<JsonArray>();
        for (int i = 0; i < RF_MAX_SENSORS; i++)
            if (gRfSensors[i].used) addRfSensorJson(rf.add<JsonObject>(), i);
    }

    // Davis (exclusive) receiver diagnostics (davis_rx.cpp)
    if (rfDavisMode(gCfg.rfEn, gCfg.rfProto)) {
        const DavisDiag& d = gDavisDiag;
        JsonObject dv = doc["davis"].to<JsonObject>();
        static const char* const states[4] = { "acquire", "track", "lost", "raw" };
        dv["running"]      = d.running;
        dv["state"]        = states[d.state & 3];
        if (d.txId) dv["txId"] = d.txId;
        dv["model"]        = gCfg.davisModel ? "vue" : "vp2";
        dv["channelTable"] = gCfg.davisTable ? "classic" : "measured";
        dv["channel"]      = d.channel;
        if (d.freqMHz > 0.0f) dv["freqMHz"] = (double)lround((double)d.freqMHz * 10000.0) / 10000.0;
        if (d.periodMs > 0.0f) dv["periodMs"] = roundf(d.periodMs * 100.0f) / 100.0f;
        dv["offsetKhz"]        = roundf(d.offsetHz / 100.0f) / 10.0f;
        dv["offsetLearnedKhz"] = roundf(d.learnedOffsetHz / 100.0f) / 10.0f;
        float sync = davisSyncPct(d);
        if (sync >= 0.0f) dv["syncPct"] = roundf(sync * 10.0f) / 10.0f;
        dv["packets"]      = d.packets;
        dv["crcErrors"]    = d.crcErrors;
        dv["otherPackets"] = d.otherPackets;
        dv["misses"]       = d.misses;
        dv["resyncs"]      = d.resyncs;
        if (d.havePacket) {
            dv["lastPacketAgeSec"] = (millis() - d.lastPacketMs) / 1000;
            dv["lastRssi"]         = (int)lroundf(d.lastRssi);
        }
        if (d.haveTiming)  dv["lastTimingErrMs"] = roundf(d.lastTimingErrUs / 100.0f) / 10.0f;
        if (d.hasRainRate) dv["rainRate"]  = rainRateForOutput(d.rainRateMmH, gCfg);
        if (d.hasGust)     dv["gust10Min"] = windSpeedForOutput(d.gust10MinMs, gCfg);
        dv["sniffer"]    = gCfg.davisSniffer != 0;
        dv["rawCapture"] = gCfg.davisRaw != 0;
        if (d.radioError) dv["radioError"] = d.radioError;
        JsonArray msgs = dv["messages"].to<JsonArray>();
        for (int t = 0; t < 16; t++) {
            const DavisTypeDiag& m = d.types[t];
            if (!m.count) continue;
            JsonObject o = msgs.add<JsonObject>();
            o["type"]   = t;
            o["name"]   = davisDiagTypeName((uint8_t)t);
            o["count"]  = m.count;
            o["ageSec"] = (millis() - m.lastMs) / 1000;
            char hex[13];
            for (int i = 0; i < 6; i++) snprintf(hex + 2 * i, 3, "%02X", m.bytes[i]);
            o["bytes"]  = hex;
            o["text"]   = m.text;
        }
    }

    if (gCfg.debugMode) {
        doc["cycle"]       = loopCount;
        doc["freeHeapKiB"] = (float)ESP.getFreeHeap() / 1024.0f;
        // esp_timer (64 bit) rather than millis(), which wraps after 49.7 days.
        doc["uptimeH"]     = (float)(esp_timer_get_time() / 1000000LL) / 3600.0f;
    }
}
