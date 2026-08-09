#include "lora_payload.h"
#include "globals.h"
#include "units.h"
#include "serial_log.h"

#include <Arduino.h>
#include <DallasTemperature.h>   // DEVICE_DISCONNECTED_C

CayenneLPP lpp(200);
uint8_t    binBuf[64];
size_t     binLen = 0;

// CayenneLPP analog-input fields — and this project's own binary format,
// which mirrors the same scaling — pack values as (value * 100) into a
// signed 16-bit field. Anything outside +-327.67 silently wraps instead of
// failing visibly: CayenneLPP::addField() only range-checks the sign bit
// for negative values, and a raw C++ (int16_t) cast of an out-of-range float
// is equally unchecked. Wind speed/gust can no longer reach this range
// (MAX_PLAUSIBLE_WIND_SPEED_MS in sensors.cpp already caps them well below
// it), but rain rate is derived from the same kind of pulse counting without
// an equivalent guard, and the debug free-heap/uptime channels legitimately
// exceed it during normal long-term operation (e.g. >13.6 days uptime) —
// clamp defensively right before encoding rather than rely on each caller.
constexpr float LPP_ANALOG_LIMIT = 327.67f;
static inline float clampLppAnalog(float v)
{
    return constrain(v, -LPP_ANALOG_LIMIT, LPP_ANALOG_LIMIT);
}

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

// ============================================================================
// BUILD AND SEND LPP PACKET
// ============================================================================
void buildLppPacket()
{
    lpp.reset();
    if (gCfg.windEn) {
        lpp.addDirection  (1, windDirection);
        lpp.addAnalogInput(1, clampLppAnalog(windSpeedForOutput(windSpeedMs, gCfg)));
        lpp.addAnalogInput(2, clampLppAnalog(windSpeedForOutput(windGustMs,  gCfg)));
    }
    if (gCfg.rainEn) {
        lpp.addAnalogInput(3, clampLppAnalog(rainRateForOutput(rainRateMmH, gCfg)));
        // Distance field is intentionally used as a mm/in carrier for rain
        // values (unit indicated by the status byte's imperial bit below).
        lpp.addDistance   (1, rainForOutput(rainMM, gCfg));
        lpp.addDistance   (2, rainForOutput(rainMMSinceStart, gCfg));
    }

    lpp.addDigitalInput   (2, computeStatusByte());
    if (gCfg.lipoEn) lpp.addVoltage(1, batteryVolt);
    if (bme280Present) {
        lpp.addRelativeHumidity  (1, bme280Humidity);
        lpp.addBarometricPressure(1, pressureForOutput(bme280PressSea, gCfg));
        lpp.addBarometricPressure(2, pressureForOutput(bme280PressAbs, gCfg));
        lpp.addTemperature       (1, temperatureForOutput(bme280TempC, gCfg));
    }
    for (int i = 0; i < bus1Count; i++)
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C)
            lpp.addTemperature(2 + i, temperatureForOutput(bus1Temps[i], gCfg));

    if (gCfg.debugMode) {
        // Keep debug channels high so production channels stay stable and compact.
        lpp.addDigitalInput(200, loopCount);
        lpp.addTemperature (201, temperatureForOutput(cpuTemperature, gCfg));
        lpp.addDigitalInput(202, (uint8_t)min(sendFailCount, 255));
        lpp.addAnalogInput (203, clampLppAnalog((float)ESP.getFreeHeap() / 1024.0f));
        lpp.addAnalogInput (204, clampLppAnalog((float)millis() / 3600000.0f));
    }

    // EU868: DR0/SF12 = max 51 bytes, DR5/SF7 = max 222 bytes payload
    if (lpp.getSize() > 51)
        dbgSerial.printf("Info: LPP packet %d bytes (> DR0 limit 51, OK from DR3)\n", lpp.getSize());
}

// ============================================================================
// BUILD COMPACT BINARY PACKET (alternative to CayenneLPP, ~50% smaller)
//
// Layout: [version:1][presence bitmask:4 LE][field data...] — only fields
// whose bit is set in the mask are present, in ascending bit-index order.
// Same value semantics/scaling as the CayenneLPP fields above, just packed
// without per-field channel/type bytes. See chirpstack/decoder.js (parseBin)
// for the matching decoder, dispatched by FPort (3 = binary, 1 = CayenneLPP).
// ============================================================================
enum BinField : uint8_t {
    BIN_WIND_DIR = 0, BIN_WIND_SPEED, BIN_WIND_GUST,
    BIN_RAIN_RATE, BIN_RAIN_MM, BIN_RAIN_SINCE,
    BIN_STATUS, BIN_BATTERY,
    BIN_BME_HUMIDITY, BIN_BME_PRESS_QNH, BIN_BME_PRESS_ABS, BIN_BME_TEMP,
    BIN_DS18B20_0, BIN_DS18B20_1, BIN_DS18B20_2,
    BIN_DBG_CYCLE, BIN_DBG_CPU_TEMP, BIN_DBG_FAIL_COUNT, BIN_DBG_FREE_HEAP, BIN_DBG_UPTIME,
};
constexpr uint8_t BIN_PROTO_VERSION = 1;

static inline void binPutU8 (uint8_t* buf, size_t& pos, uint8_t v)  { buf[pos++] = v; }
static inline void binPutU16(uint8_t* buf, size_t& pos, uint16_t v) { buf[pos++] = (v >> 8) & 0xFF; buf[pos++] = v & 0xFF; }
static inline void binPutI16(uint8_t* buf, size_t& pos, int16_t v)  { binPutU16(buf, pos, (uint16_t)v); }

void buildBinaryPacket()
{
    uint32_t mask = 0;
    uint8_t  data[48];
    size_t   dpos = 0;

    if (gCfg.windEn) {
        mask |= (1UL << BIN_WIND_DIR);   binPutU16(data, dpos, windDirection);
        mask |= (1UL << BIN_WIND_SPEED); binPutI16(data, dpos, (int16_t)roundf(clampLppAnalog(windSpeedForOutput(windSpeedMs, gCfg)) * 100));
        mask |= (1UL << BIN_WIND_GUST);  binPutI16(data, dpos, (int16_t)roundf(clampLppAnalog(windSpeedForOutput(windGustMs,  gCfg)) * 100));
    }
    if (gCfg.rainEn) {
        mask |= (1UL << BIN_RAIN_RATE); binPutI16(data, dpos, (int16_t)roundf(clampLppAnalog(rainRateForOutput(rainRateMmH, gCfg)) * 100));
        mask |= (1UL << BIN_RAIN_MM);   binPutU16(data, dpos, (uint16_t)roundf(rainForOutput(rainMM, gCfg) * 10));
        mask |= (1UL << BIN_RAIN_SINCE);binPutU16(data, dpos, (uint16_t)roundf(rainForOutput(rainMMSinceStart, gCfg) * 10));
    }

    mask |= (1UL << BIN_STATUS); binPutU8(data, dpos, computeStatusByte());

    if (gCfg.lipoEn) {
        mask |= (1UL << BIN_BATTERY);
        binPutU16(data, dpos, (uint16_t)roundf(batteryVolt * 100));
    }

    if (bme280Present) {
        mask |= (1UL << BIN_BME_HUMIDITY);  binPutU8 (data, dpos, (uint8_t)roundf(bme280Humidity * 2));
        mask |= (1UL << BIN_BME_PRESS_QNH); binPutU16(data, dpos, (uint16_t)roundf(pressureForOutput(bme280PressSea, gCfg) * 10));
        mask |= (1UL << BIN_BME_PRESS_ABS); binPutU16(data, dpos, (uint16_t)roundf(pressureForOutput(bme280PressAbs, gCfg) * 10));
        mask |= (1UL << BIN_BME_TEMP);      binPutI16(data, dpos, (int16_t)roundf(temperatureForOutput(bme280TempC, gCfg) * 10));
    }

    for (int i = 0; i < bus1Count && i < 3; i++) {
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C) {
            mask |= (1UL << (BIN_DS18B20_0 + i));
            binPutI16(data, dpos, (int16_t)roundf(temperatureForOutput(bus1Temps[i], gCfg) * 10));
        }
    }

    if (gCfg.debugMode) {
        mask |= (1UL << BIN_DBG_CYCLE);      binPutU8 (data, dpos, loopCount);
        mask |= (1UL << BIN_DBG_CPU_TEMP);   binPutI16(data, dpos, (int16_t)roundf(temperatureForOutput(cpuTemperature, gCfg) * 10));
        mask |= (1UL << BIN_DBG_FAIL_COUNT); binPutU8 (data, dpos, (uint8_t)min(sendFailCount, 255));
        mask |= (1UL << BIN_DBG_FREE_HEAP);  binPutU16(data, dpos, (uint16_t)(ESP.getFreeHeap() / 1024));
        mask |= (1UL << BIN_DBG_UPTIME);     binPutU16(data, dpos, (uint16_t)roundf((float)millis() / 3600000.0f * 10));
    }

    size_t pos = 0;
    binBuf[pos++] = BIN_PROTO_VERSION;
    binBuf[pos++] = (mask >>  0) & 0xFF;
    binBuf[pos++] = (mask >>  8) & 0xFF;
    binBuf[pos++] = (mask >> 16) & 0xFF;
    binBuf[pos++] = (mask >> 24) & 0xFF;
    memcpy(binBuf + pos, data, dpos);
    binLen = pos + dpos;

    dbgSerial.printf("Info: binary packet %d bytes (payloadFormat=1)\n", (int)binLen);
}
