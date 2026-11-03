#include "json_payload.h"
#include "globals.h"
#include "units.h"
#include <Arduino.h>
#include <DallasTemperature.h>
#include "board_io.h"

// Unit-neutral field names (temp, pressureAbs, ...) — the actual unit used
// depends on gCfg.unitSystem and is always given by a sibling "*Unit" field
// (same pattern as the pre-existing windUnit), so consumers don't need to
// hardcode an assumption about which system is active.
void buildJsonPayload(JsonDocument& doc)
{
    doc.clear();

    if (gCfg.windEn) {
        doc["windDirDeg"] = windDirection;
        doc["windSpeed"]  = windSpeedForOutput(windSpeedMs, gCfg);
        doc["windGust"]   = windSpeedForOutput(windGustMs,  gCfg);
        doc["windUnit"]   = windSpeedUnitLabel(gCfg);
    }
    if (gCfg.rainEn) {
        doc["rainRate"]        = rainRateForOutput(rainRateMmH, gCfg);
        doc["rainRateUnit"]    = rainRateUnitLabel(gCfg);
        doc["rain"]            = rainForOutput(rainMM, gCfg);
        doc["rainSinceStart"]  = rainForOutput(rainMMSinceStart, gCfg);
        doc["rainUnit"]        = rainUnitLabel(gCfg);
    }
    if (bme280Present) {
        doc["temp"]         = temperatureForOutput(bme280TempC, gCfg);
        doc["tempUnit"]     = temperatureUnitLabel(gCfg);
        doc["humidityPct"]  = bme280Humidity;
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
        if (gCfg.uvEn) {
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
        if (gCfg.auxAinEn && gCfg.wind2Mode != 2) doc["auxAnalogV"] = gIo.auxAinV;
        if (gCfg.wind2Mode) {
            JsonObject w2 = doc["wind2"].to<JsonObject>();
            w2["speed"] = windSpeedForOutput(gIo.wind2SpeedMs, gCfg);
            w2["gust"]  = windSpeedForOutput(gIo.wind2GustMs,  gCfg);
            if (gCfg.wind2Mode == 2) w2["dirDeg"] = gIo.wind2DirDeg;
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

    JsonObject status       = doc["status"].to<JsonObject>();
    status["bme280Present"] = bme280Present;
    status["bus1Present"]   = bus1Count > 0;
    status["sendFailCount"] = sendFailCount;
    status["pcbRev"]        = gCfg.pcbVersion;
    status["uvEnabled"]     = ioV2() && (gCfg.uvEn != 0);
    status["uvIndexCalibrated"] = ioV2() && gCfg.uvEn && gIo.uvIndexCalibrated;
    if (gIo.battLockout) status["batteryLowLockout"] = true;

    // Time left until the current measurement cycle completes and the next
    // transmit/publish happens (clamped to 0 — a cycle boundary can be
    // crossed between millis() here and the next loop() iteration).
    unsigned long sendTargetMs = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
    unsigned long elapsedMs    = millis() - lastSendMs;
    doc["nextCycleInSec"] = (elapsedMs < sendTargetMs) ? (sendTargetMs - elapsedMs) / 1000 : 0;

    if (gCfg.debugMode) {
        doc["cycle"]       = loopCount;
        doc["cpuTemp"]     = temperatureForOutput(cpuTemperature, gCfg);
        doc["cpuTempUnit"] = temperatureUnitLabel(gCfg);
        doc["freeHeapKiB"] = (float)ESP.getFreeHeap() / 1024.0f;
        doc["uptimeH"]     = (float)millis() / 3600000.0f;
    }
}
