#include "sources.h"
#include "derived.h"
#include "globals.h"
#include "board_io.h"
#include "rf_sensors.h"
#include "params.h"
#include "calibration.h"
#include "astro.h"
#include "lsensors.h"
#include <FineOffsetDecoder.h>

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
        case SG_UV:    return ioFull() && ioUvActive();
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
    if (idx < 0 || idx >= RF_MAX_SENSORS || !rfTableOn()) return false;
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

// Wireless wind: per path accumulator, and the last 10 min in fixed time
// buckets - coverage and memory do not depend on the packet rate (Davis ISS
// 2.56 s, WS80 4.75 s, a network source as often as it is pushed).
struct RfWindAcc {
    float  distM = 0.0f, ms = 0.0f, gustMax = 0.0f, s = 0.0f, c = 0.0f;
    int    n = 0;
    void add(float avg, float gust, float rad, uint32_t dtMs)
    {
        distM += avg * dtMs; ms += dtMs;
        if (n == 0 || gust > gustMax) gustMax = gust;
        s += sinf(rad); c += cosf(rad); n++;
    }
    void add(const RfWindAcc& o)
    {
        if (!o.n) return;
        if (n == 0 || o.gustMax > gustMax) gustMax = o.gustMax;
        distM += o.distM; ms += o.ms; s += o.s; c += o.c; n += o.n;
    }
};
constexpr uint32_t RF_BUCKET_MS = 15000;
static_assert(WIND_SPAN_10MIN_MS % RF_BUCKET_MS == 0, "whole buckets per 10 min");
// + 1: the oldest bucket still overlapping the 10 min (the window covers 10 to 10.25 min)
constexpr int RF_BUCKETS = WIND_SPAN_10MIN_MS / RF_BUCKET_MS + 1;
RfWindAcc   acc[SRC_PATHS];
RfWindAcc   bucket[RF_BUCKETS];   // [bucketHead] is the current one, started at bucketHeadMs
int         bucketHead = 0;
uint32_t    bucketHeadMs = 0;
bool        bucketStarted = false;
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
// The "Mine" IDs, copied on first use and then kept for the whole call:
// srcCompute() resolves ~10 groups with up to 5 slot lookups each - one
// locked copy on the stack instead of a heap vector per lookup.
class MineIds {
public:
    MineIds() {}   // ids is filled by load()
    int size() const { load(); return n; }
    int pos(uint32_t id) const   // position in the list, -1 = not listed
    {
        for (int k = 0; k < size(); k++) if (ids[k] == id) return k;
        return -1;
    }
    uint32_t operator[](int k) const { load(); return ids[k]; }
private:
    void load() const { if (n < 0) n = lsMineIds(ids); }
    mutable uint32_t ids[LS_MAX];
    mutable int      n = -1;
};
// slot of table entry i (used), given the list and the "Mine" entries before it
int slotOf(const MineIds& ids, int i, int mineBefore)
{
    const int k = ids.pos(gRfSensors[i].id);
    if (k >= 0) return k;
    int slot = ids.size() + i - mineBefore;
    return slot < RF_MAX_SENSORS ? slot : -1;
}
int indexForSlot(const MineIds& ids, int slot)
{
    if (slot < 0 || slot >= RF_MAX_SENSORS) return -1;
    int mineBefore = 0;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (!r.used) continue;
        if (slotOf(ids, i, mineBefore) == slot) return i;
        if (ids.pos(r.id) >= 0) mineBefore++;
    }
    return -1;
}
}

int rfSensorIndexForSlot(int slot) { return indexForSlot(MineIds(), slot); }

int rfSlotOfIndex(int idx)
{
    if (idx < 0 || idx >= RF_MAX_SENSORS || !gRfSensors[idx].used) return -1;
    const MineIds ids;
    int mineBefore = 0;
    for (int i = 0; i < idx; i++) if (gRfSensors[i].used && ids.pos(gRfSensors[i].id) >= 0) mineBefore++;
    return slotOf(ids, idx, mineBefore);
}

// gRfSensors[] index of a used sensor with this ID, -1 = not in the table.
static int rfIndexOfId(uint32_t id)
{
    if (!id) return -1;
    for (int i = 0; i < RF_MAX_SENSORS; i++)
        if (gRfSensors[i].used && gRfSensors[i].id == id) return i;
    return -1;
}

int rfIndexOfPlaceholder(const char* name, const char** field)
{
    *field = nullptr;
    if (!name) return -1;
    if (!strncmp(name, "rf", 2)) {   // old forms: rf<ID>_ (radio ID), rf<N>_ (wireless number)
        const char* p = name + 2;
        size_t n = 0;
        bool digits = true;
        for (; isxdigit((unsigned char)p[n]); n++) digits &= p[n] >= '0' && p[n] <= '9';
        if (p[n] == '_' && (n == 8 || (digits && n >= 1 && n <= 2))) {
            *field = p + n + 1;
            if (n == 8) return rfIndexOfId((uint32_t)strtoul(p, nullptr, 16));
            return rfSensorIndexForSlot(atoi(p) - 1);
        }
    }
    // a logical sensor (lsensors.h): stored "@L3F9A2C_temp" or typed "<custom ID>_temp"
    uint32_t radio = 0;
    {
        LsGuard g;   // also called from the main loop (OLED) and the upload task
        const int ls = lsOfPlaceholder(gLogicalSensors, name, field);
        if (ls < 0) return -1;
        radio = gLogicalSensors.s[ls].radioId;
    }
    return rfIndexOfId(radio);
}


void srcBindSel(SrcGroup g)
{
    if (g >= SG_COUNT) return;
    // wireless 1..n are the ★ sensors in table order; a number past them is a
    // received-only sensor (no logical sensor): bound by its number then
    const int slot = srcSelSlot(gCfg.srcSel[g]);
    LsGuard lg;   // applyParam(): also a LoRaWAN downlink / MQTT command, not on the web task
    if (slot >= 0 && slot < gLogicalSensors.count()) snprintf(gCfg.sourceInternalId[g], sizeof(gCfg.sourceInternalId[g]), "%s", gLogicalSensors.s[slot].internalId);
    else gCfg.sourceInternalId[g][0] = '\0';
}

const char* srcBoundInternalId(SrcGroup g)
{
    return g < SG_COUNT && srcSelSlot(gCfg.srcSel[g]) >= 0 ? gCfg.sourceInternalId[g] : "";
}

void srcFormValue(SrcGroup g, char* out, size_t len)
{
    const char* internalId = srcBoundInternalId(g);
    if (internalId[0]) snprintf(out, len, "%s", internalId);
    else        snprintf(out, len, "%u", g < SG_COUNT ? (unsigned)gCfg.srcSel[g] : 0u);
}

bool srcSetFormId(SrcGroup g, const char* v)
{
    if (g >= SG_COUNT || !v) return false;
    if (v[0] == 'r' && strlen(v) == 9) return true;   // a radio ID (backup version 1): ignored, not converted
    if (!lsIsInternalId(v)) return false;
    const int i = lsFindByInternalId(gLogicalSensors, v);
    if (i < 0) return true;   // a sensor no longer in the table: the source stays as it is
    // the number only matters for displays / parameters: the ★ position
    if (i < SRC_RF_SLOTS_MAX) gCfg.srcSel[g] = (uint8_t)srcSelForSlot(i);
    snprintf(gCfg.sourceInternalId[g], sizeof(gCfg.sourceInternalId[g]), "%s", v);
    return true;
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

static int resolve(SrcGroup g, const MineIds& mine)
{
    if (g >= SG_COUNT) return SRC_NONE;
    uint8_t sel = gCfg.srcSel[g];
    if (sel == SRC_OFF) return SRC_NONE;
    if (sel == SRC_BOARD) return boardHas(g) ? SRC_IS_BOARD : SRC_NONE;
    if (srcSelSlot(sel) >= 0) {
        // bound to the chosen sensor by ID (position-independent); the slot is
        // only a fallback for configs saved before the binding existed
        int idx;
        if (gCfg.sourceInternalId[g][0]) {
            LogicalSensor e;   // the bound logical sensor's current radio ID (copied: main loop)
            idx = lsCopyByInternalId(gCfg.sourceInternalId[g], e) ? rfIndexOfId(e.radioId) : -1;   // removed from the table: nothing
        } else {
            idx = indexForSlot(mine, srcSelSlot(sel));
        }
        return radioHas(idx, g) ? idx : SRC_NONE;
    }
    // Auto: board first, then
    //  - with ★ sensors: all of them in their number order (the order of the
    //    ★ list, arrows on the settings page) - the first that delivers; the
    //    others (neighbours) never;
    //  - without: wireless 1..4 in Ecowitt's sensor order (never 5..32).
    // For temperature / humidity no indoor sensor.
    if (boardHas(g)) return SRC_IS_BOARD;
    if (mine.size() > 0) {
        for (int k = 0; k < mine.size(); k++) {
            const int i = rfIndexOfId(mine[k]);
            if (i >= 0 && !(g == SG_AIR && rfIsIndoor(gRfSensors[i])) && radioHas(i, g)) return i;
        }
        return SRC_NONE;
    }
    int best = SRC_NONE, bestRank = 99;
    for (int slot = 0; slot < RF_SOURCE_SLOTS; slot++) {
        int idx = indexForSlot(mine, slot);
        // indoor sensors never feed the (outdoor) temperature / humidity by themselves
        if (g == SG_AIR && idx >= 0 && rfIsIndoor(gRfSensors[idx])) continue;
        if (!radioHas(idx, g)) continue;
        const int rank = srcHierarchyRank(gRfSensors[idx], g);   // ties: the lower number
        if (rank < bestRank) { best = idx; bestRank = rank; }
    }
    return best;
}

int srcResolve(SrcGroup g) { return resolve(g, MineIds()); }

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

static uint16_t bitsOf(const MineIds& mine)
{
    uint16_t bits = 0;
    for (int g = 0; g < SG_COUNT; g++) {
        int r = resolve((SrcGroup)g, mine);
        uint16_t v = r == SRC_NONE ? 0 : (r == SRC_IS_BOARD ? 1 : 2);
        bits |= (uint16_t)(v << (2 * g));
        if (r >= 0 && !gRfSensors[r].batteryOk) bits |= 0x8000;
    }
    return bits;
}

uint16_t srcBits() { return bitsOf(MineIds()); }

// Side-effect-free: fill v from the current board and wireless readings using
// the same source selection the cycle uses. srcUpdateCycle() stores the result
// in gSrc; srcLive() returns it for the display without touching any state.
void srcCompute(SrcValues& v)
{
    v = SrcValues();
    const MineIds mine;   // one copy for all groups
    int r = resolve(SG_AIR, mine);
    if (r == SRC_IS_BOARD) {
        v.tempValid = v.humValid = true;
        v.tempC = bme280TempC; v.humidity = bme280Humidity;
    } else if (r >= 0) {
        v.tempValid = true;  v.tempC = gRfSensors[r].tempC;
        v.humValid  = gRfSensors[r].hasHumidity && fresh(gRfSensors[r], RF_VAL_HUM);  v.humidity = gRfSensors[r].humidity;
    }
    r = resolve(SG_UV, mine);
    if (r == SRC_IS_BOARD) v.uvBoard = true;
    else if (r >= 0) { v.uvRadio = true; v.uvIndexRadio = gRfSensors[r].uvIndex; }
    r = resolve(SG_LIGHT, mine);
    if (r >= 0) { v.lightValid = true; v.lightLux = gRfSensors[r].lightLux; }
    r = resolve(SG_PRESS, mine);
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
    v.rfBatteryLow = (bitsOf(mine) & 0x8000) != 0;
}

volatile uint32_t gSrcCycleCount = 0;
void srcUpdateCycle() { srcCompute(gSrc); gSrcCycleCount = gSrcCycleCount + 1; }

SrcValues srcLive() { SrcValues v; srcCompute(v); return v; }

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
    for (int p = 0; p < SRC_PATHS; p++) acc[p].add(s.windAvgMs, s.windGustMs, rad, dt);
    if (!bucketStarted) { bucketStarted = true; bucketHeadMs = now; }
    const uint32_t steps = (now - bucketHeadMs) / RF_BUCKET_MS;   // buckets begun since the current one
    for (uint32_t k = 0; k < steps && k < (uint32_t)RF_BUCKETS; k++) {
        bucketHead = (bucketHead + 1) % RF_BUCKETS;
        bucket[bucketHead] = RfWindAcc();
    }
    bucketHeadMs += steps * RF_BUCKET_MS;
    bucket[bucketHead].add(s.windAvgMs, s.windGustMs, rad, dt);
}

WindStats srcRfWind(int path, bool tenMin)
{
    if (!tenMin) {
        if (path < 0 || path >= SRC_PATHS) return WindStats();
        const RfWindAcc& a = acc[path];
        return statsOf(a.distM, a.ms, a.gustMax, a.s, a.c, a.n);
    }
    // bucket k back ends at bucketHeadMs - (k - 1) * RF_BUCKET_MS: taken while
    // that end lies within the last 10 min
    const uint32_t age = millis() - bucketHeadMs, span = WIND_SPAN_10MIN_MS + RF_BUCKET_MS;
    RfWindAcc t;
    for (int k = 0; bucketStarted && k < RF_BUCKETS && age < span && k * RF_BUCKET_MS < span - age; k++)
        t.add(bucket[(bucketHead + RF_BUCKETS - k) % RF_BUCKETS]);
    return statsOf(t.distM, t.ms, t.gustMax, t.s, t.c, t.n);
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
