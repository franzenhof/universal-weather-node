#include "params.h"
#include "settings_reset.h"
#include "system_restart.h"
#include "loxone.h"
#include "wind_window.h"
#include "globals.h"
#include "sensors.h"
#include "board_io.h"
#include "heltec_board.h"
#include "serial_log.h"
#include "sources.h"
#include "records.h"
#include "alarms.h"

// records_store.cpp (weak default: host builds without it)
__attribute__((weak)) void recordsResetRequest(bool) {}
// alarms_run.cpp (weak default: host builds without it)
__attribute__((weak)) bool alarmsSetEnabledMask(uint16_t) { return false; }
#include <Arduino.h>

bool applyParam(uint8_t paramId, int16_t value)
{
    // The V4 Full I/O functions can't be switched on without the hardware (V4 Light, no PCB); the
    // board_io module picks up every change below on its next poll.
    bool ioParam = (paramId >= PARAM_AUX_AIN_EN && paramId <= PARAM_HEATER_FULL_BELOW_DC) ||
                   paramId == PARAM_WIND2_MODE;
    if (ioParam && !ioFull()) {
        bool switchesOff = value == 0 && (paramId == PARAM_AUX_AIN_EN || paramId == PARAM_AUX_PULSE_EN ||
                                          paramId == PARAM_WIND2_MODE ||
                                          paramId == PARAM_HEATER_MODE || paramId == PARAM_FAN_MODE ||
                                          paramId == PARAM_TACHO_EN || paramId == PARAM_VEDIRECT_EN);
        if (!switchesOff) return false;
    }
    if (paramId == PARAM_UV_EN && value != 0 && !ioFull()) return false;
    // without a PCB (V4 Full / V4 Light) there are no wind/rain inputs (the V4's GPIO2/7 drive the RF front end)
    if ((paramId == PARAM_WIND_EN || paramId == PARAM_RAIN_EN) && value != 0 && !boardPcbAllowed()) return false;

    switch (paramId) {
        case PARAM_UPLINK_INTERVAL_SEC: // uplinkIntervalSec
            if (value < 5 || value > 3600) return false;
            gCfg.uplinkIntervalSec = value;
            return true;
        case PARAM_SAMPLE_SEC: // wind sample interval: 1 or 3 s, other values map to a step
            if (value < 1 || value > 3600) return false;
            gCfg.sampleSec = windSampleSecNormalized(value);
            return true;
        case PARAM_MQTT_INTERVAL_SEC:
            if (value != 0 && (value < 2 || value > 3600)) return false;
            gCfg.mqttIntervalSec = value;
            return true;
        case PARAM_LORA_WIND_AGG:
            if (value != WIND_AGG_INTERVAL && value != WIND_AGG_10MIN) return false;
            gCfg.loraWindAgg = value;
            return true;
        case PARAM_MQTT_WIND_AGG:
            if (value != WIND_AGG_INTERVAL && value != WIND_AGG_10MIN) return false;
            gCfg.mqttWindAgg = value;
            return true;
        case PARAM_SRC_TEMP_HUM: case PARAM_SRC_WIND: case PARAM_SRC_RAIN:
        case PARAM_SRC_UV: case PARAM_SRC_LIGHT: {
            if (!srcSelValid(value)) return false;
            const SrcGroup g = (SrcGroup)(paramId - PARAM_SRC_TEMP_HUM);
            gCfg.srcSel[g] = (uint8_t)value;
            srcBindSel(g);   // pin the chosen wireless sensor by ID
            return true;
        }
        case PARAM_RF_ONLY_MINE:
            if (value != 0 && value != 1) return false;
            gCfg.rfOnlyMine = value;
            return true;
        case PARAM_CAL_TEMP_DC:     if (value < -100 || value > 100)  return false; gCfg.calTempDc = value;     return true;
        case PARAM_CAL_HUM_DPCT:    if (value < -200 || value > 200)  return false; gCfg.calHumDpct = value;    return true;
        case PARAM_CAL_PRESS_DHPA:  if (value < -200 || value > 200)  return false; gCfg.calPressDhpa = value;  return true;
        case PARAM_CAL_RAIN_PERMIL: if (value < 500 || value > 2000)  return false; gCfg.calRainPermil = value; return true;
        case PARAM_CAL_WIND_PERMIL: if (value < 500 || value > 2000)  return false; gCfg.calWindPermil = value; return true;
        case PARAM_RECORDS_RESET:   if (value != 1 && value != 2)     return false; recordsResetRequest(value == 2); return true;
        case PARAM_ALARM_ENABLED:   if (value < 0 || value > 4095)    return false; return alarmsSetEnabledMask((uint16_t)value);
        case PARAM_SUN_METHOD:      if (value != 0 && value != 1)     return false; gCfg.sunMethod = value;     return true;
        case PARAM_SETUP_DONE:
            if (value != 0 && value != 1) return false;
            gCfg.setupDone = value;
            return true;
        case PARAM_UI_LANG:
            if (value < 0 || value > 5) return false;
            gCfg.uiLang = value;
            return true;
        case PARAM_RF_HIDDEN_CLEAR:
            if (value != 0) return false;
            gCfg.rfIgnore[0] = '\0';
            return true;
        case PARAM_SRC_PRESSURE:
            if (!srcSelValid(value)) return false;
            gCfg.srcSel[SG_PRESS] = (uint8_t)value;
            srcBindSel(SG_PRESS);   // pin the chosen wireless sensor by ID
            return true;
        case PARAM_RF_PROTO:
            if (!rfProtoValid(value)) return false;
            gCfg.rfProto = value;
            return true;
        case PARAM_RF_EN:
            if (value != RF_RADIO_OFF && value != RF_RADIO_SX1262) return false;
            gCfg.rfEn = value;
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
        case PARAM_LOX_EN:
            if (value != 0 && value != 1) return false;
            gCfg.loxEn = value;
            return true;
        case PARAM_LOX_INTERVAL_SEC:
            if (!loxIntervalValid(value)) return false;
            gCfg.loxSec = value;
            return true;
        case PARAM_RAIN_PERSIST:
            if (value != 0 && value != 1) return false;
            gCfg.rainPersist = value;
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
        case PARAM_AUX_AIN_EN:
            if (value != 0 && value != 1) return false;
            if (value && gCfg.wind2Mode == 2) return false;   // AIN is the 2nd wind vane
            gCfg.auxAinEn = value;
            return true;
        case PARAM_AUX_AIN_SCALE_X1000: // auxAinScale x1000 (1667 for the R25 15k / 0-5 V divider)
            if (value < 1 || value > 20000) return false;
            gCfg.auxAinScale = value / 1000.0f;
            return true;
        case PARAM_AUX_PULSE_EN:
            if (value != 0 && value != 1) return false;
            if (value && gCfg.wind2Mode) return false;        // PULSE is the 2nd anemometer
            gCfg.auxPulseEn = value;
            return true;
        case PARAM_HEATER_MODE:
            if (value < 0 || value > 2) return false;
            gCfg.heaterMode = value;
            return true;
        case PARAM_HEATER_DUTY_PCT:
            if (value < 1 || value > 100) return false;
            gCfg.heaterDutyPct = value;
            return true;
        case PARAM_HEATER_ON_BELOW_DC: // 0.1 °C
            if (value < -400 || value > 400) return false;
            gCfg.heaterOnBelowDc = value;
            // the full-power temperature can't lie above the switch-on threshold
            if (gCfg.heaterFullBelowDc > value) gCfg.heaterFullBelowDc = value;
            return true;
        case PARAM_FAN_MODE:
            if (value < 0 || value > 2) return false;
            gCfg.fanMode = value;
            return true;
        case PARAM_FAN_DUTY_PCT:
            if (value < 1 || value > 100) return false;
            gCfg.fanDutyPct = value;
            return true;
        case PARAM_FAN_ON_ABOVE_DC: // 0.1 °C
            if (value < -400 || value > 600) return false;
            gCfg.fanOnAboveDc = value;
            return true;
        case PARAM_FAN_MIN_PV_W:
            if (value < 0 || value > 5000) return false;
            gCfg.fanMinPvW = value;
            return true;
        case PARAM_TACHO_EN:
            if (value != 0 && value != 1) return false;
            gCfg.tachoEn = value;
            return true;
        case PARAM_TACHO_PPR:
            if (value < 1 || value > 8) return false;
            gCfg.tachoPpr = value;
            return true;
        case PARAM_VEDIRECT_EN:
            if (value != 0 && value != 1) return false;
            gCfg.veDirectEn = value;
            return true;
        case PARAM_OUT_MIN_BATT_DV: // 0.1 V
            if (value < 0 || value > 600) return false;
            gCfg.outMinBattDv = value;
            return true;
        case PARAM_HEATER_MIN_DUTY_PCT:
            if (value < 1 || value > 100) return false;
            gCfg.heaterMinDutyPct = value;
            return true;
        case PARAM_HEATER_FULL_BELOW_DC: // 0.1 °C, at most heaterOnBelowDc
            if (value < -400 || value > 400 || value > gCfg.heaterOnBelowDc) return false;
            gCfg.heaterFullBelowDc = value;
            return true;
        case PARAM_LOW_POWER_EN:
            if (value != 0 && value != 1) return false;
            gCfg.lowPowerEn = value;
            return true;
        case PARAM_VANE_POWER_SAVE:
            if (value != 0 && value != 1) return false;
            gCfg.vanePowerSave = value;
            return true;
        case PARAM_WIND2_MODE:
            // The 2nd wind sensor takes over the AUX inputs it uses.
            if (value < 0 || value > 2) return false;
            gCfg.wind2Mode = value;
            if (value >= 1) gCfg.auxPulseEn = 0;
            if (value == 2) gCfg.auxAinEn = 0;
            return true;
        case PARAM_WIND2_DIR_OFFSET_DEG:
            if (value < -180 || value > 180) return false;
            gCfg.wind2DirOffsetDeg = value;
            return true;
        case PARAM_UV_EN:
            if (value != 0 && value != 1) return false;
            gCfg.uvEn = value;
            return true;
        case PARAM_UV_AIN_SCALE_X1000:
            if (value < 1000 || value > 5000) return false;
            gCfg.uvAinScale = value / 1000.0f;
            return true;
        case PARAM_UV_INDEX_EN:
            if (value != 0 && value != 1) return false;
            gCfg.uvIndexEn = value;
            return true;
        case PARAM_UV_CALIBRATED:
            if (value != 0 && value != 1) return false;
            gCfg.uvCalibrated = value;
            return true;
        case PARAM_UV_INDEX_SLOPE_X1000:
            if (value < 1 || value > 30000) return false;
            gCfg.uvIndexSlope = value / 1000.0f;
            return true;
        case PARAM_UV_INDEX_OFFSET_X1000:
            if (value < -10000 || value > 10000) return false;
            gCfg.uvIndexOffset = value / 1000.0f;
            return true;
        default:
            return false;
    }
}

void cmdResetRainAccumulator()
{
    resetRainAccumulator();   // the per-path rain windows keep counting
    dbgSerial.println("Command: rain accumulator reset.");
}

void cmdResetAuxPulses()
{
    ioResetAuxPulseTotal();
    dbgSerial.println("Command: AUX pulse total reset.");
}

void cmdRestart()
{
    dbgSerial.println("Command: restarting...");
    delay(500);
    systemRestart("restart command");
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
    // "Reset settings" (settings_reset.h): defaults, but the node stays
    // reachable - network access, admin password and LoRaWAN keys are kept;
    // measurement data too. Erasing everything is the web page's only.
    dbgSerial.println("Command: reset settings requested.");
    storageResetRequest(STORAGE_RESET_SETTINGS);
}
