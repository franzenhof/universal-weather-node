#include "params.h"
#include "globals.h"
#include "sensors.h"
#include "serial_log.h"
#include <Arduino.h>
#include <Preferences.h>

bool applyParam(uint8_t paramId, int16_t value)
{
    switch (paramId) {
        case PARAM_UPLINK_INTERVAL_SEC: // uplinkIntervalSec
            if (value < 5 || value > 3600) return false;
            gCfg.uplinkIntervalSec = value;
            if (gCfg.sampleSec > gCfg.uplinkIntervalSec) {
                gCfg.sampleSec = gCfg.uplinkIntervalSec;
            }
            return true;
        case PARAM_SAMPLE_SEC: // sampleSec
            if (value < 1 || value > gCfg.uplinkIntervalSec) return false;
            gCfg.sampleSec = value;
            return true;
        case PARAM_WIND_EN: // windEn
            if (value != 0 && value != 1) return false;
            if (gCfg.windEn == value) return true;
            gCfg.windEn = value;
            if (gCfg.windEn) {
                configureWindPcnt();
            } else {
                stopWindPcnt();
                windSpeedMs = 0.0f;
                windGustMs = 0.0f;
            }
            return true;
        case PARAM_RAIN_EN: // rainEn
            if (value != 0 && value != 1) return false;
            if (gCfg.rainEn == value) return true;
            gCfg.rainEn = value;
            if (gCfg.rainEn) {
                configureRainPcnt();
            } else {
                stopRainPcnt();
                rainMM = 0.0f;
                rainRateMmH = 0.0f;
            }
            return true;
        case PARAM_LIPO_EN: // lipoEn
            if (value != 0 && value != 1) return false;
            gCfg.lipoEn = value;
            return true;
        case PARAM_DEBUG_MODE: // debugMode
            if (value != 0 && value != 1) return false;
            gCfg.debugMode = value;
            return true;
        case PARAM_WIND_DIR_OFFSET_DEG: // windDirOffsetDeg
            if (value < -180 || value > 180) return false;
            gCfg.windDirOffsetDeg = value;
            return true;
        case PARAM_WIND_UNIT_MS: // windUnitMs
            if (value != 0 && value != 1) return false;
            gCfg.windUnitMs = value;
            return true;
        case PARAM_CPU_FREQ_MHZ: // cpuFreqMhz
            if (value != 80 && value != 240) return false;
            gCfg.cpuFreqMhz = value;
            setCpuFrequencyMhz(gCfg.cpuFreqMhz);
            return true;
        case PARAM_PAYLOAD_FORMAT: // payloadFormat
            if (value != 0 && value != 1) return false;
            gCfg.payloadFormat = value;
            return true;
        case PARAM_UNIT_SYSTEM: // unitSystem
            if (value != 0 && value != 1) return false;
            gCfg.unitSystem = value;
            return true;
        case PARAM_RAIN_MM_PER_TIP_X1000: // rainMmPerTip, value = mm/tip x1000 (e.g. 200 = 0.2mm)
            if (value < 1 || value > 5000) return false;
            gCfg.rainMmPerTip = value / 1000.0f;
            return true;
        case PARAM_WIND_MS_PER_PPS_X1000: // windMsPerPps, value = (m/s per pulse/s) x1000
            if (value < 1 || value > 10000) return false;
            gCfg.windMsPerPps = value / 1000.0f;
            return true;
        case PARAM_WIND_DIR_SENSOR_TYPE: // windDirSensorType
            if (value != 0 && value != 1) return false;
            gCfg.windDirSensorType = value;
            return true;
        default:
            return false;
    }
}

void cmdResetRainAccumulator()
{
    rainMMSinceStart = 0.0f;
    dbgSerial.println("Command: rain accumulator reset.");
}

void cmdRestart()
{
    dbgSerial.println("Command: restarting...");
    delay(500);
    ESP.restart();
}

void cmdSetDebugMode(bool enabled)
{
    gCfg.debugMode = enabled ? 1 : 0;
    saveConfig(gCfg);
    dbgSerial.printf("Command: debug mode %s.\n", enabled ? "enabled" : "disabled");
}

void cmdRequestConfigPortal()
{
    dbgSerial.println("Command: config portal requested.");
    portalRequestedViaDownlink = true;
}

void cmdFactoryReset()
{
    dbgSerial.println("Command: factory reset — wiping config, restarting...");
    Preferences prefs;
    if (prefs.begin("config", false, "cfgdata")) {
        prefs.clear();
        prefs.end();
    }
    delay(500);
    ESP.restart();
}
