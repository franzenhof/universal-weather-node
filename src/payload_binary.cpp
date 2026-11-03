// Compact binary encoder (FPort 3). Plain C++ on purpose - see payload.h.
#include "payload.h"
#include "uv_calibration.h"
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

    // Optional value: NAN -> "not available" marker.
    void u16opt(float v, float scale) { if (isnan(v)) u16(0xFFFF); else u16((uint16_t)clampLL(scaled(v, scale), 0, 65534)); }
    void i16opt(float v, float scale) { if (isnan(v)) u16(0x8000); else u16((uint16_t)(int16_t)clampLL(scaled(v, scale), -32767, 32767)); }
    void u8opt(int v) { u8(v < 0 ? 0xFF : (uint8_t)(v > 254 ? 254 : v)); }
};

} // namespace

size_t encodeBinary(const Measurements& m, uint8_t* out)
{
    // Data goes behind the 5-byte header; the mask is only known at the end.
    Writer w(out + 5);
    const float rainScale  = m.imperial() ? 1000.0f : 10.0f;   // 0.001 in / 0.1 mm
    const float pressScale = m.imperial() ? 100.0f  : 10.0f;   // 0.01 inHg / 0.1 hPa
    const bool uvPresent = m.hasUv && isfinite(m.uvSensorVolts) &&
                           m.uvSensorVolts >= 0.0f && m.uvSensorVolts <= 5.5f;

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

    if (m.hasIoStatus)  { w.set(BIN_IO_STATUS);  w.u8(m.ioStatus); }
    if (m.hasAuxAin)    { w.set(BIN_AUX_AIN);    w.u16s(m.auxAinV, 1000.0f); }
    if (m.hasAuxPulses) {
        w.set(BIN_AUX_PULSES); w.u16((uint16_t)(m.auxPulses > 65535 ? 65535 : m.auxPulses));
        w.set(BIN_AUX_TOTAL);  w.u32(m.auxPulsesTotal);
    }
    if (m.hasOutputs)   { w.set(BIN_OUTPUT_DUTY); w.u8(m.heaterDuty); w.u8(m.fanDuty); }
    if (m.hasFanRpm)    { w.set(BIN_FAN_RPM);    w.u16s(m.fanRpm, 1.0f); }
    if (m.hasVe) {
        w.set(BIN_VE_DIRECT);
        w.u16opt(m.veBattV, 100.0f);
        w.i16opt(m.veBattA, 100.0f);
        w.u16opt(m.vePvV, 100.0f);
        w.u16opt(m.vePvW, 1.0f);
        w.u8opt(m.veChargeState);
        w.u8opt(m.veError);
        w.u16opt(m.veYieldKwh, 100.0f);
        w.u16opt(m.veSocPct, 10.0f);
        if (!isnan(m.veLoadA)) { w.set(BIN_VE_LOAD); w.i16s(m.veLoadA, 100.0f); }
    }
    if (m.hasWind2) {
        if (m.hasWind2Dir) { w.set(BIN_WIND2_DIR); w.u16(m.wind2DirDeg); }
        w.set(BIN_WIND2_SPEED); w.i16s(m.wind2Speed, 100.0f);
        w.set(BIN_WIND2_GUST);  w.i16s(m.wind2Gust,  100.0f);
    }
    if (uvPresent) {
        w.set(BIN_UV);
        w.u16s(m.uvSensorVolts, 1000.0f);
        if (isfinite(m.uvIndex) && m.uvIndex >= 0.0f) {
            w.u16s(fminf(m.uvIndex, UV_INDEX_REPORT_MAX), 100.0f);
            w.u8(0x03);
        } else {
            w.u16(0xFFFF);
            w.u8(0x01);
        }
    }

    out[0] = uvPresent ? BIN_PROTO_VERSION_UV : BIN_PROTO_VERSION;
    out[1] = (uint8_t)(w.mask & 0xFF);
    out[2] = (uint8_t)((w.mask >> 8) & 0xFF);
    out[3] = (uint8_t)((w.mask >> 16) & 0xFF);
    out[4] = (uint8_t)((w.mask >> 24) & 0xFF);
    return 5 + w.n;
}

const char* dropNextGroup(Measurements& m)
{
    if (m.debug)         { m.debug = false;         return "debug"; }
    if (m.hasAuxPulses)  { m.hasAuxPulses = false;  return "AUX pulses"; }
    if (m.hasAuxAin)     { m.hasAuxAin = false;     return "AUX analog"; }
    if (m.hasUv)         { m.hasUv = false;         return "UV"; }
    if (m.hasWind2)      { m.hasWind2 = false;      return "wind 2"; }
    if (m.hasFanRpm)     { m.hasFanRpm = false;     return "fan speed"; }
    if (m.hasOutputs)    { m.hasOutputs = false;    return "output power"; }
    if (m.hasIoStatus)   { m.hasIoStatus = false;   return "I/O status"; }
    if (m.hasVe)         { m.hasVe = false;         return "Victron"; }
    if (m.hasBattery)    { m.hasBattery = false;    return "LiPo"; }
    for (int i = 2; i >= 0; i--)
        if (m.hasDs18b20[i]) {
            m.hasDs18b20[i] = false;
            static const char* const names[] = { "DS18B20 #0", "DS18B20 #1", "DS18B20 #2" };
            return names[i];
        }
    if (m.hasBme)        { m.hasBme = false;        return "BME280"; }
    if (m.hasRain)       { m.hasRain = false;       return "rain"; }
    if (m.hasWind)       { m.hasWind = false;       return "wind"; }
    return nullptr;
}

size_t encodeBinaryFitted(Measurements m, uint8_t* out, size_t limit, char* dropped, size_t droppedSize)
{
    size_t used = 0;
    if (dropped && droppedSize) dropped[0] = '\0';
    size_t n = encodeBinary(m, out);
    while (n > limit) {
        const char* g = dropNextGroup(m);
        if (!g) break;                      // only the status byte left - nothing more to drop
        if (dropped && droppedSize) {
            size_t len = strlen(g);
            if (used + len + 3 < droppedSize) {
                if (used) { dropped[used++] = ','; dropped[used++] = ' '; }
                memcpy(dropped + used, g, len + 1);
                used += len;
            }
        }
        n = encodeBinary(m, out);
    }
    return n;
}
