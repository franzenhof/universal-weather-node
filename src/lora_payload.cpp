#include "lora_payload.h"
#include "globals.h"
#include "units.h"
#include "serial_log.h"

#include <Arduino.h>
#include <DallasTemperature.h>   // DEVICE_DISCONNECTED_C
#include <esp_timer.h>
#include "payload.h"

CayenneLPP lpp(200);
uint8_t    binBuf[BIN_MAX_SIZE];
size_t     binLen = 0;

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

static Measurements currentMeasurements()
{
    Measurements m;
    m.status = computeStatusByte();
    m.hasWind = gCfg.windEn;
    m.windDirDeg = windDirection;
    m.windSpeed  = windSpeedForOutput(windSpeedMs, gCfg);
    m.windGust   = windSpeedForOutput(windGustMs,  gCfg);
    m.hasRain = gCfg.rainEn;
    m.rainRate       = rainRateForOutput(rainRateMmH, gCfg);
    m.rainCycle      = rainForOutput(rainMM, gCfg);
    m.rainSinceStart = rainForOutput(rainMMSinceStart, gCfg);
    m.hasBattery = gCfg.lipoEn;
    m.batteryV   = batteryVolt;
    m.hasBme = bme280Present;
    m.humidityPct = bme280Humidity;
    m.pressureQnh = pressureForOutput(bme280PressSea, gCfg);
    m.pressureAbs = pressureForOutput(bme280PressAbs, gCfg);
    m.bmeTemp     = temperatureForOutput(bme280TempC, gCfg);
    for (int i = 0; i < 3; i++) {
        m.hasDs18b20[i] = (i < bus1Count) && bus1Temps[i] != DEVICE_DISCONNECTED_C;
        m.ds18b20[i]    = temperatureForOutput(bus1Temps[i], gCfg);
    }
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
// ±327.67 would wrap silently.
static float lppPrep(float v, float mult, float lo, float hi)
{
    v = constrain(v, lo, hi);
    float steps = roundf(v * mult);
    return (steps + (steps >= 0.0f ? 0.25f : -0.25f)) / mult;
}
static void lppAnalog(uint8_t ch, float v) { lpp.addAnalogInput(ch, lppPrep(v, 100.0f, -327.68f, 327.67f)); }
static void lppTemp  (uint8_t ch, float v) { lpp.addTemperature(ch, lppPrep(v, 10.0f, -3276.8f, 3276.7f)); }
static void lppUInt  (uint8_t ch, float v) { lpp.addGenericSensor(ch, lppPrep(v, 1.0f, 0.0f, 4294967295.0f)); }

void buildLppPacket()
{
    const Measurements m = currentMeasurements();
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
        lpp.addDistance(1, lppPrep(m.rainCycle,      1000.0f, 0.0f, 4294967.0f));
        lpp.addDistance(2, lppPrep(m.rainSinceStart, 1000.0f, 0.0f, 4294967.0f));
    }

    lpp.addDigitalInput(2, m.status);
    if (m.hasBattery) lpp.addVoltage(1, lppPrep(m.batteryV, 100.0f, 0.0f, 655.35f));
    if (m.hasBme) {
        lpp.addRelativeHumidity(1, lppPrep(m.humidityPct, 2.0f, 0.0f, 127.5f));
        // Barometric is uint16 x10. In imperial mode the value is sent x10
        // (field holds inHg x100) - 0.1 inHg would be 3.4 hPa resolution;
        // the decoder divides by 10 again when the imperial bit is set.
        const float pScale = m.imperial() ? 10.0f : 1.0f;
        lpp.addBarometricPressure(1, lppPrep(m.pressureQnh * pScale, 10.0f, 0.0f, 6553.5f));
        lpp.addBarometricPressure(2, lppPrep(m.pressureAbs * pScale, 10.0f, 0.0f, 6553.5f));
        lppTemp(1, m.bmeTemp);
    }
    for (int i = 0; i < 3; i++)
        if (m.hasDs18b20[i]) lppTemp(2 + i, m.ds18b20[i]);

    if (m.debug) {
        // Keep debug channels high so production channels stay stable and compact.
        lpp.addDigitalInput(200, m.dbgCycle);
        lppTemp            (201, m.dbgCpuTemp);
        lpp.addDigitalInput(202, m.dbgSendFail);
        lppUInt            (203, m.dbgHeapKib);           // KiB
        lppUInt            (204, m.dbgUptimeH * 10.0f);   // 0.1 h
    }

    if (lpp.getError() != LPP_ERROR_OK)
        dbgSerial.printf("LPP encoding error %d - payload truncated\n", lpp.getError());
    // EU868: DR0-DR2 = max 51 bytes, DR3 = 115, DR4-DR5 = 222
    if (lpp.getSize() > 51)
        dbgSerial.printf("Info: LPP packet %d bytes (> 51, needs DR3 or faster on EU868)\n", lpp.getSize());
}

// ============================================================================
// COMPACT BINARY (FPort 3) - layout and encoder: payload.h / payload_binary.cpp
// ============================================================================
void buildBinaryPacket()
{
    binLen = encodeBinary(currentMeasurements(), binBuf);
    dbgSerial.printf("Info: binary packet %d bytes (payloadFormat=1)\n", (int)binLen);
}
