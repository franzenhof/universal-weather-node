#include "sources.h"
#include "derived.h"
#include "globals.h"
#include "board_io.h"
#include "rf_sensors.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

SrcValues gSrc;

namespace {
constexpr float DEG2RAD = 0.017453292519943295f;

bool boardHas(SrcGroup g)
{
    switch (g) {
        case SG_AIR:   return bme280Present;
        case SG_WIND:  return gCfg.windEn;
        case SG_RAIN:  return gCfg.rainEn;
        case SG_UV:    return ioV2() && ioUvActive();
        default:       return false;   // no light sensor on the board
    }
}

// The last packet carrying this value (RF_VAL_*) is at most SRC_RF_FRESH_MS
// old; staleMask keeps an expired one stale across the millis() wrap.
bool fresh(const RfSensorReading& r, uint8_t bit)
{
    return !(r.staleMask & bit) && millis() - rfValueMs(r, bit) <= SRC_RF_FRESH_MS;
}

bool radioHas(int idx, SrcGroup g)
{
    if (idx < 0 || idx >= RF_MAX_SENSORS || !gCfg.rfEn) return false;
    const RfSensorReading& r = gRfSensors[idx];
    if (!r.used) return false;
    switch (g) {   // the last packet carrying *this* value, not just any packet
        case SG_AIR:   return r.hasTemp  && fresh(r, RF_VAL_TEMP);
        case SG_WIND:  return r.hasWind  && fresh(r, RF_VAL_WIND);
        case SG_RAIN:  return r.hasRain  && fresh(r, RF_VAL_RAIN);
        case SG_UV:    return r.hasUv    && fresh(r, RF_VAL_UV);
        case SG_LIGHT: return r.hasLight && fresh(r, RF_VAL_LIGHT);
        default:       return false;
    }
}

// Wireless wind: per path accumulator and a 10-min ring of packets.
struct RfWindAcc {
    float  distM = 0.0f, ms = 0.0f, gustMax = 0.0f, s = 0.0f, c = 0.0f;
    int    n = 0;
};
struct RfWindEntry { uint32_t tMs; uint32_t dtMs; float avg, gust; uint16_t dir; };
constexpr int RF_RING = 80;   // >= 10 min at one packet per 12 s
RfWindAcc   acc[SRC_PATHS];
RfWindEntry ring[RF_RING];
int         ringHead = 0, ringCount = 0;
int         lastWindIdx = -1;
uint32_t    lastWindMs = 0;

WindStats statsOf(float distM, float ms, float gust, float s, float c, int n)
{
    WindStats w;
    if (n == 0 || ms <= 0.0f) return w;
    w.valid  = true;
    w.meanMs = distM / ms;
    w.gustMs = gust > w.meanMs ? gust : w.meanMs;
    w.dirValid = true;
    int deg = (int)lroundf(atan2f(s, c) / DEG2RAD);
    w.dirDeg = (uint16_t)((deg % 360 + 360) % 360);
    return w;
}
} // namespace

int rfSensorIndexForSlot(int slot)
{
    if (slot < 0 || slot >= RF_MAX_SENSORS) return -1;
    std::vector<uint32_t> ids = rfParseIds(gCfg.rfIds);
    if (!ids.empty()) {
        if (slot >= (int)ids.size()) return -1;
        for (int i = 0; i < RF_MAX_SENSORS; i++)
            if (gRfSensors[i].used && gRfSensors[i].id == ids[slot]) return i;
        return -1;
    }
    // no ID list: in the order the sensors were first received
    return gRfSensors[slot].used ? slot : -1;
}

int srcResolve(SrcGroup g)
{
    if (g >= SG_COUNT) return SRC_NONE;
    uint8_t sel = gCfg.srcSel[g];
    if (sel == SRC_OFF) return SRC_NONE;
    if (sel == SRC_BOARD) return boardHas(g) ? SRC_IS_BOARD : SRC_NONE;
    if (sel >= SRC_RF1 && sel <= SRC_RF4) {
        int idx = rfSensorIndexForSlot(sel - SRC_RF1);
        return radioHas(idx, g) ? idx : SRC_NONE;
    }
    // Auto: board first, then wireless 1..4
    if (boardHas(g)) return SRC_IS_BOARD;
    for (int slot = 0; slot < RF_MAX_SENSORS; slot++) {
        int idx = rfSensorIndexForSlot(slot);
        if (radioHas(idx, g)) return idx;
    }
    return SRC_NONE;
}

const char* srcGroupKey(SrcGroup g)
{
    static const char* const keys[SG_COUNT] = { "tempHum", "wind", "rain", "uv", "light" };
    return g < SG_COUNT ? keys[g] : "";
}

void srcDescribe(SrcGroup g, char* out, int len)
{
    int r = srcResolve(g);
    if (r == SRC_NONE)          snprintf(out, len, "none");
    else if (r == SRC_IS_BOARD) snprintf(out, len, "board");
    else                        snprintf(out, len, "radio:%08lX", (unsigned long)gRfSensors[r].id);
}

uint16_t srcBits()
{
    uint16_t bits = 0;
    for (int g = 0; g < SG_COUNT; g++) {
        int r = srcResolve((SrcGroup)g);
        uint16_t v = r == SRC_NONE ? 0 : (r == SRC_IS_BOARD ? 1 : 2);
        bits |= (uint16_t)(v << (2 * g));
        if (r >= 0 && !gRfSensors[r].batteryOk) bits |= 0x8000;
    }
    return bits;
}

void srcUpdateCycle()
{
    SrcValues v;
    int r = srcResolve(SG_AIR);
    if (r == SRC_IS_BOARD) {
        v.tempValid = v.humValid = true;
        v.tempC = bme280TempC; v.humidity = bme280Humidity;
    } else if (r >= 0) {
        v.tempValid = true;  v.tempC = gRfSensors[r].tempC;
        v.humValid  = gRfSensors[r].hasHumidity && fresh(gRfSensors[r], RF_VAL_HUM);  v.humidity = gRfSensors[r].humidity;
    }
    r = srcResolve(SG_UV);
    if (r == SRC_IS_BOARD) v.uvBoard = true;
    else if (r >= 0) { v.uvRadio = true; v.uvIndexRadio = gRfSensors[r].uvIndex; }
    r = srcResolve(SG_LIGHT);
    if (r >= 0) { v.lightValid = true; v.lightLux = gRfSensors[r].lightLux; }
    v.rfBatteryLow = (srcBits() & 0x8000) != 0;
    gSrc = v;
}

void srcBoardRain(float mm)
{
    if (srcResolve(SG_RAIN) == SRC_IS_BOARD) rainMMSinceStart += mm;
}

// Sunshine sink (wx_upload.cpp); weak default so the host tests link without it.
__attribute__((weak)) void wxAddSunshine(float) {}

namespace {
int      lastLightIdx = -1;
uint32_t lastLightMs  = 0;

// Rain that fell while the rain sensor was not heard (see srcRfPacket()).
constexpr unsigned long RAIN_SPREAD_MAX_MS = 3600000UL;
int      lateRainIdx   = -1;
float    lateRainMm    = 0.0f;
float    lateRainPerMs = 0.0f;
uint32_t lateRainMs    = 0;
}

void srcRfPacket(int idx, bool hasWind, float rainDeltaMm, bool hasLight, unsigned long rainGapMs)
{
    if (srcResolve(SG_RAIN) == idx) {
        // After an outage (rainGapMs) the increase holds all the rain since the
        // sensor was last heard. Counted at once it would look like a burst
        // (every path's rain rate = amount / its send interval), so it is
        // released at its average rate over the outage (at most 1 h) with the
        // following packets instead - late, but the total stays complete.
        uint32_t now = millis();
        if (rainGapMs && rainDeltaMm > 0.0f) {
            if (lateRainIdx != idx) lateRainMm = 0.0f;   // another sensor's rest: not the selected source
            lateRainIdx   = idx;
            lateRainMm   += rainDeltaMm;
            lateRainPerMs = lateRainMm / (float)(rainGapMs < RAIN_SPREAD_MAX_MS ? rainGapMs : RAIN_SPREAD_MAX_MS);
            lateRainMs    = now;
            rainDeltaMm   = 0.0f;
        } else if (lateRainIdx == idx && lateRainMm > 0.0f) {
            float part = lateRainPerMs * (float)(now - lateRainMs);
            if (part > lateRainMm) part = lateRainMm;
            lateRainMm  -= part;
            rainDeltaMm += part;
            lateRainMs   = now;
        }
        if (rainDeltaMm > 0.0f) rainMMSinceStart += rainDeltaMm;
    }
    if (hasLight && srcResolve(SG_LIGHT) == idx) {
        // sunshine duration: the time since the previous light packet of this
        // sensor counts as sunny if this packet is above the threshold
        uint32_t now = millis();
        uint32_t dt = (lastLightIdx == idx && lastLightMs) ? now - lastLightMs : 12000;
        if (dt < 1000) dt = 1000;
        if (dt > 60000) dt = 60000;
        lastLightIdx = idx; lastLightMs = now;
        if (gRfSensors[idx].lightLux >= SUNSHINE_LUX_THRESHOLD) wxAddSunshine(dt / 1000.0f);
    }
    if (!hasWind || srcResolve(SG_WIND) != idx) return;
    const RfSensorReading& s = gRfSensors[idx];
    uint32_t now = millis();
    // time this packet stands for: since the previous packet of the same sensor
    uint32_t dt = (lastWindIdx == idx && lastWindMs) ? now - lastWindMs : 12000;
    if (dt < 1000) dt = 1000;
    if (dt > 60000) dt = 60000;
    lastWindIdx = idx; lastWindMs = now;
    float rad = s.windDirDeg * DEG2RAD;
    for (int p = 0; p < SRC_PATHS; p++) {
        RfWindAcc& a = acc[p];
        a.distM += s.windAvgMs * dt; a.ms += dt;
        if (a.n == 0 || s.windGustMs > a.gustMax) a.gustMax = s.windGustMs;
        a.s += sinf(rad); a.c += cosf(rad); a.n++;
    }
    ring[ringHead] = { now, dt, s.windAvgMs, s.windGustMs, (uint16_t)lroundf(s.windDirDeg) };
    ringHead = (ringHead + 1) % RF_RING;
    if (ringCount < RF_RING) ringCount++;
}

WindStats srcRfWind(int path, bool tenMin)
{
    if (!tenMin) {
        if (path < 0 || path >= SRC_PATHS) return WindStats();
        const RfWindAcc& a = acc[path];
        return statsOf(a.distM, a.ms, a.gustMax, a.s, a.c, a.n);
    }
    uint32_t now = millis();
    float dist = 0, ms = 0, gust = 0, s = 0, c = 0; int n = 0;
    for (int k = 0; k < ringCount; k++) {
        const RfWindEntry& e = ring[(ringHead + RF_RING - 1 - k) % RF_RING];
        if (now - e.tMs > WIND_SPAN_10MIN_MS) break;
        dist += e.avg * e.dtMs; ms += e.dtMs;
        if (n == 0 || e.gust > gust) gust = e.gust;
        s += sinf(e.dir * DEG2RAD); c += cosf(e.dir * DEG2RAD); n++;
    }
    return statsOf(dist, ms, gust, s, c, n);
}

void srcRfWindReset(int path)
{
    if (path >= 0 && path < SRC_PATHS) acc[path] = RfWindAcc();
}

bool srcRfValueFresh(int idx, const char* value)
{
    if (idx < 0 || idx >= RF_MAX_SENSORS) return false;
    uint8_t bit = !strcmp(value, "temp") ? RF_VAL_TEMP : !strcmp(value, "humidity") ? RF_VAL_HUM :
                  !strcmp(value, "wind") ? RF_VAL_WIND : !strcmp(value, "rain") ? RF_VAL_RAIN :
                  !strcmp(value, "uv")   ? RF_VAL_UV   : RF_VAL_LIGHT;
    return fresh(gRfSensors[idx], bit);
}
