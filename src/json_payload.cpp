#include "json_payload.h"
#include "globals.h"
#include "units.h"
#include <Arduino.h>
#include <DallasTemperature.h>
#include "board_io.h"
#include "sources.h"
#include "rf_sensors.h"
#include "derived_now.h"
#include "sensors.h"
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
        if (d.daily.valid) doc["rainToday"] = rainForOutput(d.daily.rainTodayMm, gCfg);
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
    if (bme280Present) {
        doc["pressureAbs"]  = pressureForOutput(bme280PressAbs, gCfg);
        doc["pressureQnh"]  = pressureForOutput(bme280PressSea, gCfg);
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
        if (gSrc.rfBatteryLow) doc["radioBatteryLow"] = true;
    }

    addDerivedJson(doc, pv->windSpeedMs, pv->windDirDeg);

    JsonObject status       = doc["status"].to<JsonObject>();
    status["bme280Present"] = bme280Present;
    status["bus1Present"]   = bus1Count > 0;
    status["sendFailCount"] = sendFailCount;
    status["pcbRev"]        = gCfg.pcbVersion;
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
        for (int i = 0; i < RF_MAX_SENSORS; i++) {
            const RfSensorReading& r = gRfSensors[i];
            if (!r.used) continue;
            JsonObject o = rf.add<JsonObject>();
            char id[11];
            snprintf(id, sizeof(id), "%08lX", (unsigned long)r.id);
            o["id"]        = id;
            o["type"]      = r.type;
            o["protocol"]  = rfProtoName(r.proto);
            if (r.model)   o["model"]   = r.model;      // Fine Offset: "WS90", "WH31", ...
            if (r.channel) o["channel"] = r.channel;    // WH31
            o["rssi"]      = (int)lroundf(r.rssi);
            // after a millis() wrap the raw difference is meaningless once the sensor is stale
            unsigned long age = (millis() - r.lastMs) / 1000;
            if ((r.staleMask & RF_VAL_ANY) && age < SRC_RF_FRESH_MS / 1000) age = SRC_RF_FRESH_MS / 1000;
            o["ageSec"]    = age;
            o["batteryOk"] = r.batteryOk;
            if (r.hasTemp)     o["temp"]     = temperatureForOutput(r.tempC, gCfg);
            if (r.hasHumidity) o["humidityPct"] = r.humidity;
            if (r.hasWind) {
                o["windSpeed"]  = windSpeedForOutput(r.windAvgMs, gCfg);
                o["windGust"]   = windSpeedForOutput(r.windGustMs, gCfg);
                o["windDirDeg"] = (int)lroundf(r.windDirDeg);
            }
            if (r.hasRain)  o["rainTotal"] = rainForOutput(r.rainTotalMm, gCfg);
            if (r.hasUv)    o["uvIndex"]   = r.uvIndex;
            if (r.hasLight) o["lightLux"]  = (long)lroundf(r.lightLux);
            if (r.hasMoisture) o["soilMoisturePct"] = r.moisturePct;
            if (r.hasSupercap) o["supercapV"] = roundf(r.supercapV * 10.0f) / 10.0f;   // only stations with one (WS85/WS90)
            if (r.hasLightning) {
                o["lightningCount"] = r.lightningCount;   // the sensor's 8-bit counter (wraps)
                if (r.lightningKm) o["lightningDistKm"] = r.lightningKm;
            }
            // values not received for more than 5 min (the sensor still sends others)
            const bool has[6] = { r.hasTemp, r.hasHumidity, r.hasWind, r.hasRain, r.hasUv, r.hasLight };
            static const char* const keys[6] = { "temp", "humidity", "wind", "rain", "uv", "light" };
            JsonArray stale;
            for (int k = 0; k < 6; k++)
                if (has[k] && !srcRfValueFresh(i, keys[k])) {
                    if (stale.isNull()) stale = o["stale"].to<JsonArray>();
                    stale.add(keys[k]);
                }
        }
    }

    if (gCfg.debugMode) {
        doc["cycle"]       = loopCount;
        doc["freeHeapKiB"] = (float)ESP.getFreeHeap() / 1024.0f;
        // esp_timer (64 bit) rather than millis(), which wraps after 49.7 days.
        doc["uptimeH"]     = (float)(esp_timer_get_time() / 1000000LL) / 3600.0f;
    }
}
