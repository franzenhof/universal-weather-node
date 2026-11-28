#include "lora_payload.h"
#include "globals.h"
#include "units.h"
#include "serial_log.h"

#include <Arduino.h>
#include <DallasTemperature.h>   // DEVICE_DISCONNECTED_C
#include <esp_timer.h>
#include "payload.h"
#include "board_io.h"
#include "uv_calibration.h"
#include "sources.h"
#include "sensors.h"

CayenneLPP lpp(200);
uint8_t    binBuf[BIN_MAX_SIZE];
size_t     binLen = 0;
char       lastPayloadDropped[256] = "";

// ============================================================================
// MEASUREMENTS FOR THIS CYCLE (shared by both wire formats)
// ============================================================================

// Status byte: BME280 Bit0, Bus1 Bit1, errors Bit2-5, imperial units Bit6,
// wind unit Bit7 (0=km/h, 1=m/s; ignored when Bit6 set). Shared by both wire
// formats — see the CayenneLPP Ch2 Digital Input and the binary format's
// BIN_STATUS field, which carry the exact same byte.
static uint8_t computeStatusByte()
{
    uint8_t status = 0;
    if (bme280Present)        status |= (1 << 0);
    if (bus1Count > 0)        status |= (1 << 1);
    status |= (uint8_t)(min(sendFailCount, 15) << 2);
    if (gCfg.windUnitMs)      status |= (1 << 7);
    if (gCfg.unitSystem == 1) status |= (1 << 6);
    return status;
}

static Measurements currentMeasurements(const PathValues* pv)
{
    // Wind/rain of this path's own window (main.cpp); the globals hold the
    // base cycle's values and are only the fallback.
    PathValues g;
    if (!pv) {
        g.windValid = windCycleValid;
        g.windSpeedMs = windSpeedMs; g.windGustMs = windGustMs; g.windDirDeg = windDirection;
        g.rainMm = rainMM; g.rainRateMmH = rainRateMmH;
        pv = &g;
    }
    Measurements m;
    m.status = computeStatusByte();
    m.hasWind = srcActive(SG_WIND) && pv->windValid;   // no valid sample: left out, not stale/0
    m.windDirDeg = pv->windDirDeg;
    m.windSpeed  = windSpeedForOutput(pv->windSpeedMs, gCfg);
    m.windGust   = windSpeedForOutput(pv->windGustMs,  gCfg);
    m.hasRain = srcActive(SG_RAIN);
    m.rainRate       = rainRateForOutput(pv->rainRateMmH, gCfg);
    m.rainCycle      = rainForOutput(pv->rainMm, gCfg);
    m.rainSinceStart = rainForOutput(rainMMSinceStart, gCfg);
    m.hasBattery = gCfg.lipoEn;
    m.batteryV   = batteryVolt;
    // temperature/humidity and pressure from the selected sources
    m.hasBme = gSrc.tempValid;
    m.hasHumidity = gSrc.humValid;
    m.humidityPct = gSrc.humidity;
    m.hasPressure = gSrc.pressValid;
    m.pressureQnh = pressureForOutput(gSrc.pressSeaHpa, gCfg);
    m.pressureAbs = pressureForOutput(gSrc.pressAbsHpa, gCfg);
    m.bmeTemp     = temperatureForOutput(gSrc.tempC, gCfg);
    for (int i = 0; i < 3; i++) {
        m.hasDs18b20[i] = (i < bus1Count) && bus1Temps[i] != DEVICE_DISCONNECTED_C;
        m.ds18b20[i]    = temperatureForOutput(bus1Temps[i], gCfg);
    }
    if (ioAnyEnabled()) {
        m.hasIoStatus  = true;               m.ioStatus = ioStatusByte();
        m.hasAuxAin    = ioAuxAinActive();      m.auxAinV = gIo.auxAinV;
        m.hasAuxPulses = gCfg.auxPulseEn && !gCfg.wind2Mode;  m.auxPulses = gIo.auxPulsesCycle;  m.auxPulsesTotal = gIo.auxPulsesTotal;
        m.hasAuxAin    = m.hasAuxAin && gCfg.wind2Mode != 2;
        m.hasWind2     = gCfg.wind2Mode != 0 && gIo.wind2Valid;  m.hasWind2Dir = m.hasWind2 && ioWind2DirActive();
        m.wind2DirDeg  = gIo.wind2DirDeg;
        m.wind2Speed   = windSpeedForOutput(gIo.wind2SpeedMs, gCfg);
        m.wind2Gust    = windSpeedForOutput(gIo.wind2GustMs,  gCfg);
        m.hasOutputs   = gCfg.heaterMode || gCfg.fanMode;
        m.heaterDuty   = gIo.heaterDuty;     m.fanDuty = gIo.fanDuty;
        m.hasFanRpm    = gCfg.tachoEn;       m.fanRpm = gIo.fanRpm;
        m.hasVe        = gCfg.veDirectEn && gIo.veValid;
        m.veBattV = gIo.ve.battV;  m.veBattA = gIo.ve.battA;  m.vePvV = gIo.ve.pvV;  m.vePvW = gIo.ve.pvW;
        m.veYieldKwh = gIo.ve.yieldTodayKwh;  m.veSocPct = gIo.ve.socPct;  m.veLoadA = gIo.ve.loadA;
        m.veChargeState = gIo.ve.chargeState; m.veError = gIo.ve.error;
    }
    if (gSrc.uvBoard) {
        m.hasUv = ioV2() && ioUvActive() && isfinite(gIo.uvSensorVolts) &&
                  gIo.uvSensorVolts >= 0.0f && gIo.uvSensorVolts <= 5.5f;
        if (m.hasUv) {
            m.uvSensorVolts = gIo.uvSensorVolts;
            if (gIo.uvIndexCalibrated && isfinite(gIo.uvIndex)) m.uvIndex = gIo.uvIndex;
        }
    } else if (gSrc.uvRadio) {   // wireless sensor: index only, no terminal voltage
        m.hasUv = true;
        m.uvSensorVolts = NAN;
        m.uvIndex = gSrc.uvIndexRadio;
    }
    m.hasLight = gSrc.lightValid;  m.lightLux = gSrc.lightLux;
    m.hasSources = gCfg.rfEn != 0;  m.sources = srcBits();
    m.debug = gCfg.debugMode;
    m.dbgCycle    = loopCount;
    m.dbgSendFail = (uint8_t)min(sendFailCount, 255);
    m.dbgCpuTemp  = temperatureForOutput(cpuTemperature, gCfg);
    m.dbgHeapKib  = (float)ESP.getFreeHeap() / 1024.0f;
    // esp_timer (64 bit) rather than millis(), which wraps after 49.7 days.
    m.dbgUptimeH  = (float)(esp_timer_get_time() / 1000000LL) / 3600.0f;
    return m;
}

// ============================================================================
// CAYENNELPP (FPort 1)
// ============================================================================

// CayenneLPP converts with a truncating cast ((uint32_t)(|v| * mult)), so
// e.g. 12.34 would be sent as 12.33. Round to the field's resolution first
// and nudge a quarter step away from zero so the cast lands on the rounded
// value. Values outside the field's range are clamped instead of wrapping:
// CayenneLPP::addField() doesn't range-check, so an analog input above
// ±327.67 would wrap silently. Callers skip non-finite values (fin()): NAN
// would otherwise go on air as a plausible-looking number.
static bool  fin(float v) { return isfinite(v); }
static float lppPrep(float v, float mult, float lo, float hi)
{
    v = constrain(v, lo, hi);
    float steps = roundf(v * mult);
    return (steps + (steps >= 0.0f ? 0.25f : -0.25f)) / mult;
}
static void lppAnalog(uint8_t ch, float v) { if (fin(v)) lpp.addAnalogInput(ch, lppPrep(v, 100.0f, -327.68f, 327.67f)); }
static void lppTemp  (uint8_t ch, float v) { if (fin(v)) lpp.addTemperature(ch, lppPrep(v, 10.0f, -3276.8f, 3276.7f)); }
static void lppVolt  (uint8_t ch, float v) { if (fin(v)) lpp.addVoltage(ch, lppPrep(v, 100.0f, 0.0f, 655.35f)); }
static void lppDist  (uint8_t ch, float v) { if (fin(v)) lpp.addDistance(ch, lppPrep(v, 1000.0f, 0.0f, 4294967.0f)); }

// Generic Sensor, exact: addGenericSensor() takes a float (exact only up to
// 2^24, so a large AUX pulse total would be rounded) - add the field, then
// write the exact big-endian uint32 into its four data bytes.
static void lppUInt(uint8_t ch, uint32_t v)
{
    if (!lpp.addGenericSensor(ch, 0.0f)) return;   // buffer full (error noted by the library)
    uint8_t* p = lpp.getBuffer() + lpp.getSize() - 4;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
// Measured values (lux, rpm, ...): rounded and clamped to 0..2^32-1.
static void lppUIntF(uint8_t ch, float v)
{
    if (!fin(v)) return;
    double d = v;
    lppUInt(ch, d <= 0.0 ? 0u : d >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)llround(d));
}

static void encodeLpp(const Measurements& m)
{
    lpp.reset();
    if (m.hasWind) {
        lpp.addDirection(1, m.windDirDeg);
        lppAnalog(1, m.windSpeed);
        lppAnalog(2, m.windGust);
    }
    if (m.hasRain) {
        lppAnalog(3, m.rainRate);
        // Distance field is intentionally used as a mm/in carrier for rain
        // values (x1000 uint32; unit indicated by the status byte's imperial bit).
        lppDist(1, m.rainCycle);
        lppDist(2, m.rainSinceStart);
    }

    lpp.addDigitalInput(2, m.status);
    if (m.hasBattery) lppVolt(1, m.batteryV);
    if (m.hasBme && m.hasHumidity && fin(m.humidityPct))
        lpp.addRelativeHumidity(1, lppPrep(m.humidityPct, 2.0f, 0.0f, 127.5f));
    if (m.hasPressure) {
        // Barometric is uint16 x10. In imperial mode the value is sent x10
        // (field holds inHg x100) - 0.1 inHg would be 3.4 hPa resolution;
        // the decoder divides by 10 again when the imperial bit is set.
        const float pScale = m.imperial() ? 10.0f : 1.0f;
        if (fin(m.pressureQnh)) lpp.addBarometricPressure(1, lppPrep(m.pressureQnh * pScale, 10.0f, 0.0f, 6553.5f));
        if (fin(m.pressureAbs)) lpp.addBarometricPressure(2, lppPrep(m.pressureAbs * pScale, 10.0f, 0.0f, 6553.5f));
    }
    if (m.hasBme) lppTemp(1, m.bmeTemp);
    for (int i = 0; i < 3; i++)
        if (m.hasDs18b20[i]) lppTemp(2 + i, m.ds18b20[i]);

    // PCB v2.0 I/O block
    if (m.hasIoStatus)  lpp.addDigitalInput(3, m.ioStatus);
    if (m.hasAuxAin)    lppAnalog(5, m.auxAinV);
    if (m.hasAuxPulses) { lppUInt(10, m.auxPulses); lppUInt(11, m.auxPulsesTotal); }
    if (m.hasWind2) {
        if (m.hasWind2Dir) lpp.addDirection(2, m.wind2DirDeg);
        lppAnalog(6, m.wind2Speed);
        lppAnalog(7, m.wind2Gust);
    }
    if (m.hasUv) {
        const bool volts = fin(m.uvSensorVolts);   // absent for a wireless UV sensor
        const bool index = fin(m.uvIndex) && m.uvIndex >= 0.0f;
        if (volts) lppVolt(12, m.uvSensorVolts);
        if (index) lppAnalog(13, fminf(m.uvIndex, UV_INDEX_REPORT_MAX));
        // the flags say exactly which of the two values are in this uplink
        lpp.addDigitalInput(14, (uint8_t)((volts ? 0x01 : 0x00) | (index ? 0x02 : 0x00)));
    }
    if (m.hasLight)   lppUIntF(15, m.lightLux);
    if (m.hasSources) lppUInt(16, m.sources);
    if (m.hasOutputs)   { lpp.addDigitalOutput(20, m.heaterDuty); lpp.addDigitalOutput(21, m.fanDuty); }
    if (m.hasFanRpm)    lppUIntF(22, m.fanRpm);
    if (m.hasVe) {
        lppVolt(30, m.veBattV);
        lppAnalog(30, m.veBattA);
        lppAnalog(31, m.veLoadA);
        lppVolt(31, m.vePvV);
        if (fin(m.vePvW))      lpp.addPower(30, lppPrep(m.vePvW, 1.0f, 0.0f, 65535.0f));
        if (fin(m.veYieldKwh)) lpp.addEnergy(30, lppPrep(m.veYieldKwh, 1000.0f, 0.0f, 4294967.0f));
        if (fin(m.veSocPct))   lpp.addPercentage(30, (uint32_t)lroundf(constrain(m.veSocPct, 0.0f, 100.0f)));
        if (m.veChargeState >= 0) lpp.addDigitalInput(30, (uint8_t)min(m.veChargeState, 255));
        if (m.veError >= 0)       lpp.addDigitalInput(31, (uint8_t)min(m.veError, 255));
    }

    if (m.debug) {
        // Keep debug channels high so production channels stay stable and compact.
        lpp.addDigitalInput(200, m.dbgCycle);
        lppTemp            (201, m.dbgCpuTemp);
        lpp.addDigitalInput(202, m.dbgSendFail);
        lppUIntF           (203, m.dbgHeapKib);           // KiB
        lppUIntF           (204, m.dbgUptimeH * 10.0f);   // 0.1 h
    }

    if (lpp.getError() != LPP_ERROR_OK)
        dbgSerial.printf("LPP encoding error %d - payload truncated\n", lpp.getError());
}

static void noteDropped(const char* format, size_t len, size_t limit)
{
    if (lastPayloadDropped[0])
        dbgSerial.printf("Payload (%s): %u bytes, limit %u at the current data rate - left out: %s\n",
                         format, (unsigned)len, (unsigned)limit, lastPayloadDropped);
    else
        dbgSerial.printf("Payload (%s): %u bytes (limit %u)\n", format, (unsigned)len, (unsigned)limit);
}

void buildLppPacket(size_t limit, const PathValues* pv)
{
    Measurements m = currentMeasurements(pv);
    lastPayloadDropped[0] = '\0';
    size_t used = 0;
    encodeLpp(m);
    while (lpp.getSize() > limit) {
        const char* g = dropNextGroup(m);
        if (!g) break;
        // "a, b, c"; if the buffer is too small, the list ends in "..."
        const size_t room = sizeof(lastPayloadDropped);
        size_t len = strlen(g);
        if (used + len + 2 + 4 < room) {
            used += snprintf(lastPayloadDropped + used, room - used, "%s%s", used ? ", " : "", g);
        } else if (used + 4 < room && (used < 3 || strcmp(lastPayloadDropped + used - 3, "...") != 0)) {
            used += snprintf(lastPayloadDropped + used, room - used, "...");
        }
        encodeLpp(m);
    }
    noteDropped("CayenneLPP", lpp.getSize(), limit);
}

// ============================================================================
// COMPACT BINARY (FPort 3) - layout and encoder: payload.h / payload_binary.cpp
// ============================================================================
void buildBinaryPacket(size_t limit, const PathValues* pv)
{
    binLen = encodeBinaryFitted(currentMeasurements(pv), binBuf, limit, lastPayloadDropped, sizeof(lastPayloadDropped));
    noteDropped("binary", binLen, limit);
}
