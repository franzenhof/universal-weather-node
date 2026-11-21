#include "sources.h"
#include "derived.h"
#include "globals.h"
#include "board_io.h"
#include "rf_sensors.h"
#include "params.h"
#include "calibration.h"
#include "astro.h"
#include <FineOffsetDecoder.h>

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
        case SG_PRESS: return bme280Present;
        default:       return false;   // no light sensor on the board
    }
}

// The last packet carrying this value (RF_VAL_*) is at most SRC_RF_FRESH_MS
// old; staleMask keeps an expired one stale across the millis() wrap.
bool fresh(const RfSensorReading& r, uint8_t bit)
{
    return !(r.staleMask & bit) && millis() - rfValueMs(r, bit) <= rfFreshMs(r);   // per family (rf_sensors.h)
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
        case SG_PRESS: return r.hasPressure && fresh(r, RF_VAL_PRESS);
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

// Slots (0-based): the "Mine" IDs first in list order (also while not
// received: wireless 1-4 keep their meaning), then the other received sensors
// by table position - the reception order, gaps included, so a number stays
// put while another sensor drops out. Without "Mine" IDs: slot = table index
// (as always). Numbers beyond RF_MAX_SENSORS: none.
namespace {
bool listed(const std::vector<uint32_t>& ids, uint32_t id)
{
    for (uint32_t x : ids) if (x == id) return true;
    return false;
}
// slot of table entry i (used), given the list and the "Mine" entries before it
int slotOf(const std::vector<uint32_t>& ids, int i, int mineBefore)
{
    const uint32_t id = gRfSensors[i].id;
    for (size_t k = 0; k < ids.size(); k++) if (ids[k] == id) return (int)k;
    int slot = (int)ids.size() + i - mineBefore;
    return slot < RF_MAX_SENSORS ? slot : -1;
}
}

int rfSensorIndexForSlot(int slot)
{
    if (slot < 0 || slot >= RF_MAX_SENSORS) return -1;
    std::vector<uint32_t> ids = rfParseIds(gCfg.rfIds);
    int mineBefore = 0;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (!r.used) continue;
        if (slotOf(ids, i, mineBefore) == slot) return i;
        if (listed(ids, r.id)) mineBefore++;
    }
    return -1;
}

int rfSlotOfIndex(int idx)
{
    if (idx < 0 || idx >= RF_MAX_SENSORS || !gRfSensors[idx].used) return -1;
    std::vector<uint32_t> ids = rfParseIds(gCfg.rfIds);
    int mineBefore = 0;
    for (int i = 0; i < idx; i++) if (gRfSensors[i].used && listed(ids, gRfSensors[i].id)) mineBefore++;
    return slotOf(ids, idx, mineBefore);
}

// Auto among wireless 1-4: Ecowitt's own order when several deliver the same
// group (Ecowitt wiki by Gyvate, section "sensor hierarchy"); lower = first,
// ties and other sensors (Bresser, IT+, Davis, other families) by number.
int srcHierarchyRank(const RfSensorReading& r, SrcGroup g)
{
    if (r.proto != RF_PROTO_FINEOFFSET) return 9;
    const uint8_t f = (uint8_t)(r.id >> 24);
    switch (g) {
        case SG_AIR:    // a dedicated outdoor thermo-hygrometer first, then the arrays
            return f == FO_FAMILY_WH32 ? 0 : f == FO_FAMILY_WS90 ? 1 : f == FO_FAMILY_WS80 ? 2 : f == FO_FAMILY_WH65 ? 3 : 9;
        case SG_RAIN:   // tipping buckets before the piezo arrays
            return f == FO_FAMILY_WH40 ? 0 : f == FO_FAMILY_WN20 ? 1 : f == FO_FAMILY_WH65 ? 2 :
                   (f == FO_FAMILY_WS90 || f == FO_FAMILY_WS85) ? 3 : 9;
        case SG_WIND:   // ultrasonic before cups
            return f == FO_FAMILY_WS90 ? 0 : f == FO_FAMILY_WS80 ? 1 : f == FO_FAMILY_WS85 ? 2 : f == FO_FAMILY_WH65 ? 3 :
                   f == FO_FAMILY_WS68 ? 4 : 9;
        case SG_UV: case SG_LIGHT:
            return f == FO_FAMILY_WS90 ? 0 : f == FO_FAMILY_WS80 ? 1 : f == FO_FAMILY_WH65 ? 2 : f == FO_FAMILY_WS68 ? 3 : 9;
        default:
            return 9;
    }
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
    // Auto: board first, then wireless 1..4 (only the value-source slots,
    // never sensors 5..32; for temperature / humidity no indoor sensor)
    if (boardHas(g)) return SRC_IS_BOARD;
    int best = SRC_NONE, bestRank = 99;
    for (int slot = 0; slot < RF_SOURCE_SLOTS; slot++) {
        int idx = rfSensorIndexForSlot(slot);
        // indoor sensors never feed the (outdoor) temperature / humidity by themselves
        if (g == SG_AIR && idx >= 0 && rfIsIndoor(gRfSensors[idx])) continue;
        if (!radioHas(idx, g)) continue;
        const int rank = srcHierarchyRank(gRfSensors[idx], g);   // ties: the lower number
        if (rank < bestRank) { best = idx; bestRank = rank; }
    }
    return best;
}

const char* srcGroupKey(SrcGroup g)
{
    static const char* const keys[SG_COUNT] = { "tempHum", "wind", "rain", "uv", "light", "pressure" };
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
    r = srcResolve(SG_PRESS);
    if (r == SRC_IS_BOARD) {
        v.pressValid = true;
        v.pressAbsHpa = bme280PressAbs; v.pressSeaHpa = bme280PressSea;
    } else if (r >= 0) {
        v.pressValid = true;
        v.pressAbsHpa = gRfSensors[r].pressureHpa;
        v.pressSeaHpa = seaLevelPressureHpa(v.pressAbsHpa, gCfg.altitudeM);
    }
    // main-value calibration (calibration.h): after the source, for board and wireless alike
    if (v.tempValid) v.tempC = calTemp(v.tempC);
    if (v.humValid)  v.humidity = calHumidity(v.humidity);
    if (v.pressValid && gCfg.calPressDhpa) {
        v.pressAbsHpa = calStationPressure(v.pressAbsHpa);
        v.pressSeaHpa = seaLevelPressureHpa(v.pressAbsHpa, gCfg.altitudeM);
    }
    v.rfBatteryLow = (srcBits() & 0x8000) != 0;
    gSrc = v;
}

void srcBoardRain(float mm)
{
    if (srcResolve(SG_RAIN) == SRC_IS_BOARD) rainMMSinceStart += calRain(mm);
}

// Sunshine sink (wx_upload.cpp); weak default so the host tests link without it.
__attribute__((weak)) void wxAddSunshine(float) {}
// Sunshine by radiation (astro_store.cpp); weak default: not available
__attribute__((weak)) bool astroSunnyByRadiation(float, bool&) { return false; }

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
        rainDeltaMm = calRain(rainDeltaMm);   // rain factor (calibration.h)
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
        // Settings: illuminance threshold, or (sunMethod 1, a W/m2 sensor, location
        // set) solar radiation >= 0.4 x clear-sky maximum (astro.h)
        const RfSensorReading& l = gRfSensors[idx];
        bool sunny = l.lightLux >= SUNSHINE_LUX_THRESHOLD, byRad;
        if (gCfg.sunMethod == 1 && l.hasSolar && astroSunnyByRadiation(l.solarWm2, byRad)) sunny = byRad;
        if (sunny) wxAddSunshine(dt / 1000.0f);
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
                  !strcmp(value, "uv")   ? RF_VAL_UV   : !strcmp(value, "pressure") ? RF_VAL_PRESS : RF_VAL_LIGHT;
    return fresh(gRfSensors[idx], bit);
}

int srcParamId(SrcGroup g)
{
    return g == SG_PRESS ? PARAM_SRC_PRESSURE : PARAM_SRC_TEMP_HUM + (int)g;
}
