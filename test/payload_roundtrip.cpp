// Host-side helper for payload_roundtrip.test.js: encodes a few measurement
// sets with the firmware's binary encoder (src/payload_binary.cpp) and prints
// them as JSON, so the test can decode them with chirpstack/decoder.js.
#include <math.h>
#include <stdio.h>
#include "payload.h"
#include "uv_calibration.h"

static void emit(const char* name, const Measurements& m, bool last, size_t limit = BIN_MAX_SIZE)
{
    uint8_t buf[BIN_MAX_SIZE];
    size_t n = (limit == BIN_MAX_SIZE) ? encodeBinary(m, buf) : encodeBinaryFitted(m, buf, limit);
    printf("  {\"name\": \"%s\", \"bytes\": [", name);
    for (size_t i = 0; i < n; i++) printf("%s%u", i ? ", " : "", buf[i]);
    printf("]}%s\n", last ? "" : ",");
}

static Measurements full(bool imperial)
{
    Measurements m;
    m.status = 0x83 | (imperial ? 0x40 : 0x00);   // BME, bus1, m/s (+ imperial)
    m.hasWind = true;    m.windDirDeg = 338; m.windSpeed = 7.0f; m.windGust = 10.42f;
    m.hasRain = true;    m.rainRate = 2.4f;
    m.rainCycle      = imperial ? 0.008f : 0.2f;      // one 0.2 mm tip
    m.rainSinceStart = imperial ? 315.354f : 8010.0f; // > 6553.5: wrapped in v1
    m.hasBattery = true; m.batteryV = 3.97f;
    m.hasBme = true;     m.humidityPct = 49.5f; m.bmeTemp = imperial ? 82.9f : 28.3f;
    m.hasPressure = true;
    m.pressureQnh = imperial ? 29.99f : 1015.8f;
    m.pressureAbs = imperial ? 28.06f : 950.1f;
    for (int i = 0; i < 3; i++) { m.hasDs18b20[i] = true; m.ds18b20[i] = 10.5f + i; }
    m.debug = true;      m.dbgCycle = 42; m.dbgSendFail = 1; m.dbgCpuTemp = 41.2f;
    m.dbgHeapKib = 121.6f; m.dbgUptimeH = 1234.56f;
    return m;
}

int main()
{
    printf("[\n");
    emit("metric", full(false), false);
    emit("imperial", full(true), false);

    Measurements minimal;                          // only the status byte
    emit("minimal", minimal, false);

    Measurements clamp;                            // out of range: clamp, not wrap
    clamp.hasWind = true; clamp.windSpeed = 400.0f; clamp.windGust = -400.0f;
    clamp.hasRain = true; clamp.rainCycle = -1.0f;
    emit("clamp", clamp, false);

    Measurements io;                               // PCB v2.0 I/O block, MPPT (no SOC)
    io.hasIoStatus = true;  io.ioStatus = 0x05;    // heater on, fan stalled
    io.hasAuxAin = true;    io.auxAinV = 4.321f;
    io.hasAuxPulses = true; io.auxPulses = 70000;  io.auxPulsesTotal = 123456789;
    io.hasOutputs = true;   io.heaterDuty = 60;    io.fanDuty = 100;
    io.hasFanRpm = true;    io.fanRpm = 2950.4f;
    io.hasVe = true;        io.veBattV = 13.28f;   io.veBattA = -1.23f;  io.vePvV = 37.5f;  io.vePvW = 45.0f;
    io.veYieldKwh = 0.12f;  io.veSocPct = NAN;  io.veLoadA = 0.31f;     io.veChargeState = 3;  io.veError = 0;
    io.hasWind2 = true;     io.hasWind2Dir = true; io.wind2DirDeg = 271;  io.wind2Speed = 4.56f;  io.wind2Gust = 9.87f;
    emit("io", io, false);

    Measurements uv;
    uv.hasUv = true; uv.uvSensorVolts = 4.999f; uv.uvIndex = 400.0f;
    emit("uv", uv, false);
    Measurements uvUncalibrated;
    uvUncalibrated.hasUv = true; uvUncalibrated.uvSensorVolts = 3.3f;
    emit("uv_uncalibrated", uvUncalibrated, false);
    Measurements uvOutOfRange;
    uvOutOfRange.hasUv = true; uvOutOfRange.uvSensorVolts = 5.501f;
    emit("uv_out_of_range", uvOutOfRange, false);

    if (uvCalibrationValid(true, 0.0f, 0.0f) ||
        uvCalibrationValid(false, 1.0f, 0.0f) ||
        !uvCalibrationValid(true, 30.0f, -10.0f) ||
        !uvCalibrationValid(true, 0.001f, 0.0f) ||
        !isnan(calculateUvIndex(1.0f, 30.001f, 0.0f)) ||
        !isnan(calculateUvIndex(1.0f, 0.0f, 0.0f)) ||
        !isnan(calculateUvIndex(1.0f, 1.0f, 10.001f)) ||
        !isnan(calculateUvIndex(-0.001f, 1.0f, 0.0f)) ||
        !isnan(calculateUvIndex(5.501f, 1.0f, 0.0f)) ||
        calculateUvIndex(0.0f, 1.0f, -10.0f) != 0.0f ||
        calculateUvIndex(5.5f, 30.0f, 10.0f) != 175.0f)
        return 2;

    Measurements radio;                            // wireless sources: UV index only, light, sources, no pressure
    radio.hasBme = true; radio.hasHumidity = false; radio.bmeTemp = 4.5f;
    radio.hasUv = true; radio.uvSensorVolts = NAN; radio.uvIndex = 3.4f;
    radio.hasLight = true; radio.lightLux = 98765.0f;
    radio.hasSources = true; radio.sources = (uint16_t)(2 | (2 << 2) | (1 << 4) | (2 << 6) | (2 << 8) | 0x8000);
    emit("radio", radio, false);

    Measurements all = full(false);                // everything incl. the v2.0 block and debug
    all.hasIoStatus = io.hasIoStatus; all.ioStatus = io.ioStatus;
    all.hasAuxAin = true; all.auxAinV = io.auxAinV;
    all.hasAuxPulses = true; all.auxPulses = 12; all.auxPulsesTotal = 345;
    all.hasOutputs = true; all.heaterDuty = 60; all.fanDuty = 100;
    all.hasFanRpm = true; all.fanRpm = 2950.0f;
    all.hasVe = true; all.veBattV = 13.28f; all.veBattA = -1.23f; all.vePvV = 37.5f; all.vePvW = 45.0f;
    all.veYieldKwh = 0.12f; all.veSocPct = NAN; all.veChargeState = 3; all.veError = 0;
    all.hasWind2 = true; all.hasWind2Dir = true; all.wind2DirDeg = 271; all.wind2Speed = 4.56f; all.wind2Gust = 9.87f;
    all.hasUv = true; all.uvSensorVolts = 5.0f; all.uvIndex = 12.34f;
    all.hasLight = true; all.lightLux = 120000.0f; all.hasSources = true; all.sources = 0x0155;
    emit("all", all, false);
    emit("fit51", all, false, 51);                 // EU868 SF10-SF12
    emit("fit11", all, true, 11);                  // US915 DR0

    printf("]\n");
    return 0;
}
