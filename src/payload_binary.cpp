// Compact binary encoder (FPort 3). Plain C++ on purpose - see payload.h.
#include "payload.h"
#include <math.h>
#include <string.h>

namespace {

struct Writer {
    uint8_t* buf;
    size_t   n = 0;
    uint32_t mask = 0;

    explicit Writer(uint8_t* b) : buf(b) {}

    // Out-of-range values are clamped instead of wrapping.
    static long long clampLL(long long v, long long lo, long long hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static long long scaled(float v, float scale) { return llroundf(v * scale); }

    void u8(uint8_t v) { buf[n++] = v; }
    void u16(uint16_t v) { buf[n++] = (uint8_t)(v >> 8); buf[n++] = (uint8_t)(v & 0xFF); }
    void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)(v & 0xFFFF)); }

    void i16s(float v, float scale) { u16((uint16_t)(int16_t)clampLL(scaled(v, scale), -32768, 32767)); }
    void u16s(float v, float scale) { u16((uint16_t)clampLL(scaled(v, scale), 0, 65535)); }
    void u32s(float v, float scale) { u32((uint32_t)clampLL(scaled(v, scale), 0, 4294967295LL)); }

    void set(BinField f) { mask |= (1UL << f); }
};

} // namespace

size_t encodeBinary(const Measurements& m, uint8_t* out)
{
    // Data goes behind the 5-byte header; the mask is only known at the end.
    Writer w(out + 5);
    const float rainScale  = m.imperial() ? 1000.0f : 10.0f;   // 0.001 in / 0.1 mm
    const float pressScale = m.imperial() ? 100.0f  : 10.0f;   // 0.01 inHg / 0.1 hPa

    if (m.hasWind) {
        w.set(BIN_WIND_DIR);   w.u16(m.windDirDeg);
        w.set(BIN_WIND_SPEED); w.i16s(m.windSpeed, 100.0f);
        w.set(BIN_WIND_GUST);  w.i16s(m.windGust,  100.0f);
    }
    if (m.hasRain) {
        w.set(BIN_RAIN_RATE);  w.i16s(m.rainRate, 100.0f);
        w.set(BIN_RAIN_MM);    w.u16s(m.rainCycle, rainScale);
        w.set(BIN_RAIN_SINCE); w.u32s(m.rainSinceStart, rainScale);
    }

    w.set(BIN_STATUS); w.u8(m.status);

    if (m.hasBattery) { w.set(BIN_BATTERY); w.u16s(m.batteryV, 100.0f); }

    if (m.hasBme) {
        w.set(BIN_BME_HUMIDITY);  w.u8((uint8_t)Writer::clampLL(Writer::scaled(m.humidityPct, 2.0f), 0, 255));
        w.set(BIN_BME_PRESS_QNH); w.u16s(m.pressureQnh, pressScale);
        w.set(BIN_BME_PRESS_ABS); w.u16s(m.pressureAbs, pressScale);
        w.set(BIN_BME_TEMP);      w.i16s(m.bmeTemp, 10.0f);
    }

    for (int i = 0; i < 3; i++)
        if (m.hasDs18b20[i]) { w.set((BinField)(BIN_DS18B20_0 + i)); w.i16s(m.ds18b20[i], 10.0f); }

    if (m.debug) {
        w.set(BIN_DBG_CYCLE);      w.u8(m.dbgCycle);
        w.set(BIN_DBG_CPU_TEMP);   w.i16s(m.dbgCpuTemp, 10.0f);
        w.set(BIN_DBG_FAIL_COUNT); w.u8(m.dbgSendFail);
        w.set(BIN_DBG_FREE_HEAP);  w.u16s(m.dbgHeapKib, 1.0f);
        w.set(BIN_DBG_UPTIME);     w.u16s(m.dbgUptimeH, 10.0f);
    }

    out[0] = BIN_PROTO_VERSION;
    out[1] = (uint8_t)(w.mask & 0xFF);
    out[2] = (uint8_t)((w.mask >> 8) & 0xFF);
    out[3] = (uint8_t)((w.mask >> 16) & 0xFF);
    out[4] = (uint8_t)((w.mask >> 24) & 0xFF);
    return 5 + w.n;
}
