// Compact binary encoder (FPort 3). Plain C++ on purpose - see payload.h.
#include "payload.h"
#include "uv_calibration.h"
#include <math.h>
#include <string.h>

namespace {

struct Writer {
    uint8_t* buf;
    size_t   n = 0;
    uint64_t mask = 0;

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

    void set(BinField f) { mask |= (1ULL << f); }

    // A non-finite value (NAN from a failed read, INF) is left out - its bit
    // stays clear - instead of becoming a plausible-looking number
    // (llroundf(NAN) is unspecified).
    void i16f(BinField f, float v, float scale) { if (isfinite(v)) { set(f); i16s(v, scale); } }
    void u16f(BinField f, float v, float scale) { if (isfinite(v)) { set(f); u16s(v, scale); } }
    void u32f(BinField f, float v, float scale) { if (isfinite(v)) { set(f); u32s(v, scale); } }
};

} // namespace

size_t encodeBinary(const Measurements& m, uint8_t* out)
{
    // Fields are written in ascending bit order into a scratch buffer; the
    // variable-length mask in front is only known at the end.
    uint8_t data[BIN_MAX_SIZE];
    Writer w(data);
    const float rainScale  = m.imperial() ? 1000.0f : 10.0f;   // 0.001 in / 0.1 mm
    const float pressScale = m.imperial() ? 100.0f  : 10.0f;   // 0.01 inHg / 0.1 hPa
    const bool uvVolts = m.hasUv && isfinite(m.uvSensorVolts) &&
                         m.uvSensorVolts >= 0.0f && m.uvSensorVolts <= 5.5f;
    const bool uvIndex = m.hasUv && isfinite(m.uvIndex) && m.uvIndex >= 0.0f;

    if (m.hasWind) {
        w.set(BIN_WIND_DIR);   w.u16(m.windDirDeg);
        w.i16f(BIN_WIND_SPEED, m.windSpeed, 100.0f);
        w.i16f(BIN_WIND_GUST,  m.windGust,  100.0f);
    }
    if (m.hasRain) {
        w.i16f(BIN_RAIN_RATE,  m.rainRate, 100.0f);
        w.u16f(BIN_RAIN_MM,    m.rainCycle, rainScale);
        w.u32f(BIN_RAIN_SINCE, m.rainSinceStart, rainScale);
    }

    w.set(BIN_STATUS); w.u8(m.status);

    if (m.hasBattery) w.u16f(BIN_BATTERY, m.batteryV, 100.0f);

    if (m.hasBme && m.hasHumidity && isfinite(m.humidityPct)) {
        w.set(BIN_BME_HUMIDITY);  w.u8((uint8_t)Writer::clampLL(Writer::scaled(m.humidityPct, 2.0f), 0, 255));
    }
    if (m.hasPressure) {
        w.u16f(BIN_BME_PRESS_QNH, m.pressureQnh, pressScale);
        w.u16f(BIN_BME_PRESS_ABS, m.pressureAbs, pressScale);
    }
    if (m.hasBme) w.i16f(BIN_BME_TEMP, m.bmeTemp, 10.0f);

    for (int i = 0; i < 3; i++)
        if (m.hasDs18b20[i]) w.i16f((BinField)(BIN_DS18B20_0 + i), m.ds18b20[i], 10.0f);

    if (m.debug) {
        w.set(BIN_DBG_CYCLE);      w.u8(m.dbgCycle);
        w.i16f(BIN_DBG_CPU_TEMP,   m.dbgCpuTemp, 10.0f);
        w.set(BIN_DBG_FAIL_COUNT); w.u8(m.dbgSendFail);
        w.u16f(BIN_DBG_FREE_HEAP,  m.dbgHeapKib, 1.0f);
        w.u16f(BIN_DBG_UPTIME,     m.dbgUptimeH, 10.0f);
    }

    if (m.hasIoStatus)  { w.set(BIN_IO_STATUS);  w.u8(m.ioStatus); }
    if (m.hasAuxAin)    w.u16f(BIN_AUX_AIN, m.auxAinV, 1000.0f);
    if (m.hasAuxPulses) {
        w.set(BIN_AUX_PULSES); w.u16((uint16_t)(m.auxPulses > 65535 ? 65535 : m.auxPulses));
        w.set(BIN_AUX_TOTAL);  w.u32(m.auxPulsesTotal);
    }
    if (m.hasOutputs)   { w.set(BIN_OUTPUT_DUTY); w.u8(m.heaterDuty); w.u8(m.fanDuty); }
    if (m.hasFanRpm)    w.u16f(BIN_FAN_RPM, m.fanRpm, 1.0f);
    if (m.hasVe)        w.u16f(BIN_VE_BATT_V, m.veBattV, 100.0f);
    if (m.hasVe)        w.i16f(BIN_VE_LOAD,   m.veLoadA, 100.0f);
    if (m.hasWind2) {
        if (m.hasWind2Dir) { w.set(BIN_WIND2_DIR); w.u16(m.wind2DirDeg); }
        w.i16f(BIN_WIND2_SPEED, m.wind2Speed, 100.0f);
        w.i16f(BIN_WIND2_GUST,  m.wind2Gust,  100.0f);
    }
    if (uvVolts) w.u16f(BIN_UV_VOLTS, m.uvSensorVolts, 1000.0f);
    if (uvIndex) w.u16f(BIN_UV_INDEX, fminf(m.uvIndex, UV_INDEX_REPORT_MAX), 100.0f);
    if (m.hasSources) { w.set(BIN_SOURCES); w.u16(m.sources); }
    if (m.hasLight)   w.u32f(BIN_LIGHT, m.lightLux, 1.0f);
    if (m.hasVe) {
        w.i16f(BIN_VE_BATT_A, m.veBattA, 100.0f);
        w.u16f(BIN_VE_PV_V,   m.vePvV, 100.0f);
        w.u16f(BIN_VE_PV_W,   m.vePvW, 1.0f);
        if (m.veChargeState >= 0) { w.set(BIN_VE_CS);     w.u8((uint8_t)(m.veChargeState > 255 ? 255 : m.veChargeState)); }
        if (m.veError >= 0)       { w.set(BIN_VE_ERR);    w.u8((uint8_t)(m.veError > 255 ? 255 : m.veError)); }
        w.u16f(BIN_VE_YIELD,  m.veYieldKwh, 100.0f);
        w.u16f(BIN_VE_SOC,    m.veSocPct, 10.0f);
    }

    // header: version, then the mask 7 bits per byte (bit 7 = more follows)
    size_t n = 0;
    out[n++] = BIN_PROTO_VERSION;
    uint64_t mask = w.mask;
    do {
        uint8_t b = (uint8_t)(mask & 0x7F);
        mask >>= 7;
        out[n++] = (uint8_t)(b | (mask ? 0x80 : 0x00));
    } while (mask);
    memcpy(out + n, data, w.n);
    return n + w.n;
}

const char* dropNextGroup(Measurements& m)
{
    if (m.debug)         { m.debug = false;         return "debug"; }
    if (m.hasAuxPulses)  { m.hasAuxPulses = false;  return "AUX pulses"; }
    if (m.hasAuxAin)     { m.hasAuxAin = false;     return "AUX analog"; }
    if (m.hasSources)    { m.hasSources = false;    return "sources"; }
    if (m.hasLight)      { m.hasLight = false;      return "light"; }
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
    if (m.hasBme || m.hasPressure) { m.hasBme = m.hasPressure = false; return "temperature/humidity/pressure"; }
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
            // "a, b, c"; if the buffer is too small, the list ends in "..."
            size_t len = strlen(g);
            if (used + len + 2 + 4 < droppedSize) {
                if (used) { dropped[used++] = ','; dropped[used++] = ' '; }
                memcpy(dropped + used, g, len + 1);
                used += len;
            } else if (used + 4 < droppedSize && (used < 3 || strcmp(dropped + used - 3, "...") != 0)) {
                memcpy(dropped + used, "...", 4);
                used += 3;
            }
        }
        n = encodeBinary(m, out);
    }
    return n;
}
