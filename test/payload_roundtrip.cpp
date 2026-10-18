// Host-side helper for payload_roundtrip.test.js: encodes a few measurement
// sets with the firmware's binary encoder (src/payload_binary.cpp) and prints
// them as JSON, so the test can decode them with chirpstack/decoder.js.
#include <stdio.h>
#include "payload.h"

static void emit(const char* name, const Measurements& m, bool last)
{
    uint8_t buf[BIN_MAX_SIZE];
    size_t n = encodeBinary(m, buf);
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
    emit("clamp", clamp, true);

    printf("]\n");
    return 0;
}
