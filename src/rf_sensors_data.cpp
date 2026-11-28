// Latest wireless-sensor values (filled by rf_sensors.cpp). Kept in its own
// file without RadioLib so the JSON/telemetry host tests can link it.
#include "rf_sensors.h"
#include "rf_schedule.h"
#include "config.h"
#include "globals.h"
#include "calibration.h"
#include "rf_stats.h"
#include "rf_names.h"
#include "rf_minrssi.h"
#include "lsensors.h"
#include <string.h>
#include <math.h>
#include "davis_diag.h"

RfSensorReading gRfSensors[RF_MAX_SENSORS];
RfHiddenSeen gRfHidden[RF_HIDDEN_SEEN];
DavisDiag gDavisDiag;

#include <stdlib.h>
#include <stdio.h>
#include <FineOffsetDecoder.h>

std::vector<uint32_t> rfParseIds(const char* s)
{
    std::vector<uint32_t> ids;
    const char* p = s;
    while (*p && ids.size() < RF_MAX_SENSORS) {
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        if (v) ids.push_back((uint32_t)v);
        p = end;
    }
    return ids;
}

unsigned long rfValueMs(const RfSensorReading& r, uint8_t bit)
{
    switch (bit) {
        case RF_VAL_TEMP:  return r.tempMs;
        case RF_VAL_HUM:   return r.humMs;
        case RF_VAL_WIND:  return r.windMs;
        case RF_VAL_RAIN:  return r.rainMs;
        case RF_VAL_UV:    return r.uvMs;
        case RF_VAL_LIGHT: return r.lightMs;
        case RF_VAL_PRESS: return r.pressMs;
        default:           return r.lastMs;
    }
}

volatile uint32_t gRfStoreCount = 0;

namespace {
// ★ = bound to a logical sensor. A table lookup under its lock (no list
// parsing, no allocation: called for every packet).
bool isLogicalSensor(uint32_t radioId)
{
    LsGuard g;
    return lsFindByRadioId(gLogicalSensors, radioId) >= 0;
}
} // namespace

ReceptionVerdict rfReceptionOf(const RfSensorReading& r)
{
    if (!r.used || rfIsNetProto(r.proto)) return ReceptionVerdict();
    return receptionEvaluate(r.reception, r.periodMs, (uint32_t)millis());
}

namespace {
// Phantom-rain filter limits (rfStoreValues(), rf_sensors.h). A step up to
// max(5 mm, 2 mm per minute since the last accepted total) is taken at once:
// 2 mm/min = 120 mm/h, a cloudburst; between two packets 16-49 s apart that is
// 5 mm. A confirmed larger step counts as rain up to 10 mm per minute (at
// least one minute) - DavisRainFilter's default rate of 50 tips of 0.2 mm.
constexpr float RAIN_STEP_AT_ONCE_MM = 5.0f;
constexpr float RAIN_AT_ONCE_MM_PER_MIN = 2.0f;
constexpr float RAIN_CONFIRMED_MAX_MM_PER_MIN = 10.0f;

// Rain between two totals of one sensor: a smaller total = the counter restarted from 0.
float rainStepMm(float fromTotalMm, float toTotalMm)
{
    return toTotalMm >= fromTotalMm ? toTotalMm - fromTotalMm : toTotalMm;
}

bool rainStepTakenAtOnce(float fromTotalMm, float toTotalMm, float elapsedMin)
{
    const float scaledMm = RAIN_AT_ONCE_MM_PER_MIN * elapsedMin;
    return toTotalMm >= fromTotalMm && toTotalMm - fromTotalMm <= (scaledMm > RAIN_STEP_AT_ONCE_MM ? scaledMm : RAIN_STEP_AT_ONCE_MM);
}

// The filter step for a sensor that already has a rain total. Returns true
// when totalMm is accepted (rainMm = the rain to count), false when it is held.
bool filterRainTotal(RfSensorReading& r, float totalMm, unsigned long elapsedMs, float& rainMm)
{
    const float elapsedMin = elapsedMs / 60000.0f;
    if (rainStepTakenAtOnce(r.rainTotalMm, totalMm, elapsedMin)) {   // (a held value was corrupt: dropped)
        r.rainTotalHeld = false;
        rainMm = totalMm - r.rainTotalMm;
        return true;
    }
    if (r.rainTotalHeld && rainStepTakenAtOnce(r.heldRainTotalMm, totalMm, 0.0f)) {   // confirms the held step
        r.rainTotalHeld = false;
        const float heldStepMm = rainStepMm(r.rainTotalMm, r.heldRainTotalMm);
        const float plausibleMm = RAIN_CONFIRMED_MAX_MM_PER_MIN * (elapsedMin > 1.0f ? elapsedMin : 1.0f);
        rainMm = (heldStepMm <= plausibleMm ? heldStepMm : 0.0f) + (totalMm - r.heldRainTotalMm);   // too much: re-based
        return true;
    }
    r.rainTotalHeld = true;   // (another implausible value replaces a held one)
    r.heldRainTotalMm = totalMm;
    rainMm = 0.0f;
    return false;
}
} // namespace

float rfStoreValues(RfSensorReading& r, const RfValues& v, unsigned long now, unsigned long& rainGapMs)
{
    gRfStoreCount = gRfStoreCount + 1;
    const uint8_t wasStale = r.staleMask;
    r.lastMs = now;
    const unsigned long fixedIntervalMs = rfFixedIntervalMs(r);
    const unsigned long previousPacketMs = r.schedMs;   // 0 = first packet of this entry
    if (previousPacketMs) r.periodMs = rfLearnPeriod(r.periodMs, fixedIntervalMs, now - previousPacketMs);
    r.schedMs = now;
    // the previous values: a "repeat" with other values is a second transmitter (rfGapImpossible())
    const bool hadTemp = r.hasTemp, hadHumidity = r.hasHumidity;
    const float previousTempC = r.tempC;
    const uint8_t previousHumidity = r.humidity;
    r.staleMask &= (uint8_t)~(v.ok | RF_VAL_ANY);
    // Values not in this packet are kept (a 6-in-1 sends two alternating messages).
    // per-sensor offsets / gains (calibration.h): every output of this sensor sees the corrected value
    RfCal cal;
    if (v.ok & (RF_VAL_TEMP | RF_VAL_HUM | RF_VAL_WIND | RF_VAL_RAIN | RF_VAL_UV | RF_VAL_LIGHT)) rfCalGet(gCfg.rfCal, r.id, cal);
    const float dT = cal.t10 / 10.0f, dH = cal.h10 / 10.0f;
    if (v.ok & RF_VAL_TEMP) { r.hasTemp = true;     r.tempC = v.tempC + dT;  r.tempMs = now; }
    if (v.ok & RF_VAL_HUM) {
        float h = v.humidity + dH;
        r.hasHumidity = true; r.humidity = (uint8_t)(h < 0.0f ? 0 : h > 100.0f ? 100 : (int)(h + 0.5f)); r.humMs = now;
    }
    if (v.ok & RF_VAL_WIND) {
        r.hasWind    = true;
        r.windMs     = now;
        r.windAvgMs  = v.windAvgMs * cal.wind / 1000.0f;
        r.windGustMs = v.windGustMs * cal.wind / 1000.0f;
        r.windDirDeg = v.windDirDeg;
    }
    float rainDelta = 0.0f;
    rainGapMs = 0;
    if (v.ok & RF_VAL_RAIN) {
        // Increase since the previous packet; a smaller total = sensor reset
        // (battery change). No previous value (node start): 0 - the rain while
        // the node was down is unknown.
        bool accepted = true;
        if (r.hasRain) {
            const unsigned long age = now - r.rainMs, fresh = rfFreshMs(r);
            // expired across a millis() wrap: the age is unknown, at least the window
            const bool ageUnknown = (wasStale & RF_VAL_RAIN) && age <= fresh;
            const unsigned long elapsedMs = ageUnknown ? fresh + 1 : age;
            if (r.proto == RF_PROTO_DAVIS || rfIsNetProto(r.proto))
                rainDelta = rainStepMm(r.rainTotalMm, v.rainTotalMm);
            else
                accepted = filterRainTotal(r, v.rainTotalMm, elapsedMs, rainDelta);
            if (accepted && elapsedMs > fresh) rainGapMs = elapsedMs;
        }
        if (accepted) { r.hasRain = true;  r.rainTotalMm = v.rainTotalMm;  r.rainMs = now; }
        else          r.staleMask |= wasStale & RF_VAL_RAIN;   // a held total is not a fresh rain value
    }
    // per-sensor rain today / last hour and lightning statistics (rf_stats.h)
    if ((v.ok & RF_VAL_RAIN) || r.hasLightning) {
        uint32_t t, ymd;
        rfStatsClock(t, ymd);
        rfStatsLock();
        RfStat* st = rfStatFor(r.id, true);
        if (!st) {   // full: drop the statistics of sensors that left the table (ID change after a battery change)
            rfStatsPrune([](uint32_t id) { for (const RfSensorReading& x : gRfSensors) if (x.used && x.id == id) return true; return false; });
            st = rfStatFor(r.id, true);
        }
        // rain gain / WN20 preset on the increment, never on the raw counter (rate: this sensor's last 10-20 min)
        if (rainDelta > 0.0f && (cal.rain != 1000 || cal.wn20 || cal.wh40))
            rainDelta *= rfRainGain(cal, st ? rfStatRecentRate(*st, t) : 0.0f);
        if (st) {
            if (v.ok & RF_VAL_RAIN) rfStatRain(*st, rainDelta, t, ymd);
            if (r.hasLightning) rfStatLightning(*st, r.lightningCount, t, ymd);
        }
        rfStatsUnlock();
    }
    if (v.ok & RF_VAL_UV)    { r.hasUv = true;    r.uvIndex = v.uvIndex * cal.uv / 1000.0f;    r.uvMs = now; }
    if (v.ok & RF_VAL_LIGHT) { r.hasLight = true; r.lightLux = v.lightLux * cal.lux / 1000.0f; r.lightMs = now; }
    if (v.ok & RF_VAL_PRESS) { r.hasPressure = true; r.pressureHpa = v.pressureHpa; r.pressMs = now; }

    // reception watchdog: counted only while ★ (radio sensors); a sensor that is
    // not ★ - also the old radio ID after a re-bind - keeps no window, so being
    // ★ed or re-bound starts a fresh hour
    if (!rfIsNetProto(r.proto) && isLogicalSensor(r.id)) {
        const bool valuesDiffer = ((v.ok & RF_VAL_TEMP) && hadTemp && fabsf(r.tempC - previousTempC) >= 0.05f)
                               || ((v.ok & RF_VAL_HUM) && hadHumidity && r.humidity != previousHumidity);
        const bool impossibleGap = previousPacketMs && rfGapImpossible((uint32_t)(now - previousPacketMs), fixedIntervalMs, valuesDiffer);
        receptionCountPacket(r.reception, (uint32_t)now, impossibleGap);
    } else {
        receptionStop(r.reception);
    }
    return rainDelta;
}

int rfSlotFor(uint32_t id, unsigned long now, unsigned long takeOverMs, int onlyProto, bool mine)
{
    int freeIdx = -1, oldIdx = -1;
    unsigned long oldAge = 0;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (r.used && r.id == id) return i;
        if (!r.used) { if (freeIdx < 0) freeIdx = i; continue; }
        if (onlyProto && r.proto != onlyProto) continue;
        // expired (staleMask): older than any timestamp, also after a millis() wrap
        unsigned long age = (r.staleMask & RF_VAL_ANY) ? ~0UL : now - r.lastMs;
        // a sensor with a window longer than the standard one (10-minute air
        // quality sensors) keeps its place between packets; the others go after
        // takeOverMs (IT+ new battery: 60 s, rf_sensors.cpp storeItPlus())
        const unsigned long fresh = rfFreshMs(r);
        const unsigned long takeOverAfterMs = fresh > SRC_RF_FRESH_MS && fresh > takeOverMs ? fresh : takeOverMs;
        if (age > takeOverAfterMs && (oldIdx < 0 || age > oldAge)) { oldIdx = i; oldAge = age; }
    }
    if (freeIdx >= 0) { gRfSensors[freeIdx].firstMs = now; return freeIdx; }
    if (oldIdx < 0 && mine) {
        // full of other sensors heard recently: "Mine" goes first
        for (int i = 0; i < RF_MAX_SENSORS; i++) {
            const RfSensorReading& r = gRfSensors[i];
            if (rfStateOf(r.id) == RF_STATE_MINE) continue;
            unsigned long age = (r.staleMask & RF_VAL_ANY) ? ~0UL : now - r.lastMs;
            if (oldIdx < 0 || age > oldAge) { oldIdx = i; oldAge = age; }
        }
    }
    if (oldIdx >= 0) { gRfSensors[oldIdx] = RfSensorReading(); gRfSensors[oldIdx].firstMs = now; }   // sensor gone (battery change: new ID)
    return oldIdx;
}

bool rfIsIndoor(const RfSensorReading& r)
{
    if (!r.used || r.proto != RF_PROTO_FINEOFFSET) return false;
    const uint8_t family = (uint8_t)(r.id >> 24);
    return family == FO_FAMILY_WH45 || family == FO_FAMILY_WH46 || family == FO_FAMILY_WH25;
}

// IT+ (TFA / LaCrosse, both data rates): "the sensors send about every 4 s"
// (docs/wireless-sensors.md, lib/TfaWeatherSensorReceiver/README.md); measured
// 4.07 s on a 17.241 kbit/s sensor (V4 serial log, 2026-11-27).
constexpr unsigned long IT_PLUS_INTERVAL_MS = 4000;
// Fine Offset air quality sensors: 10 min on battery, 60 s on USB.
constexpr unsigned long FO_AIR_QUALITY_BATTERY_INTERVAL_MS = 600000;

unsigned long rfNominalIntervalMs(const RfSensorReading& r)
{
    if (r.proto == RF_PROTO_ITPLUS17 || r.proto == RF_PROTO_ITPLUS9) return IT_PLUS_INTERVAL_MS;
    if (r.proto != RF_PROTO_FINEOFFSET) return 0;
    switch ((uint8_t)(r.id >> 24)) {
        case FO_FAMILY_WS80: return 4750;
        case FO_FAMILY_WS90: return 8800;
        case FO_FAMILY_WH65: return 16000;
        case FO_FAMILY_WS68: return 16500;
        case FO_FAMILY_WH40: return 49000;
        case FO_FAMILY_WH31: case FO_FAMILY_WH31B: return 61000;
        case FO_FAMILY_WH25: case FO_FAMILY_WH32:  return 64000;
        case FO_FAMILY_WH51: case FO_FAMILY_WH52:  return 70000;
        case FO_FAMILY_WN34: case FO_FAMILY_WN38:  return 77000;
        case FO_FAMILY_WH57: return 79000;
        case FO_FAMILY_WN35: return 79500;
        case FO_FAMILY_WH55: return 60000;
        case FO_FAMILY_LDS01: return 80000;
        case FO_FAMILY_WH41: case FO_FAMILY_WH0290: case FO_FAMILY_WH43:
        case FO_FAMILY_WH45: case FO_FAMILY_WH46:  return FO_AIR_QUALITY_BATTERY_INTERVAL_MS;   // on battery (60 s on USB)
        default: return 0;
    }
}

unsigned long rfFixedIntervalMs(const RfSensorReading& r)
{
    const unsigned long nominalMs = rfNominalIntervalMs(r);
    if (r.proto == RF_PROTO_FINEOFFSET && nominalMs == FO_AIR_QUALITY_BATTERY_INTERVAL_MS) return 0;   // depends on the power supply
    return nominalMs;
}

unsigned long rfFreshMs(const RfSensorReading& r)
{
    if (rfIsNetProto(r.proto)) {   // network sensor: its slot's window
        const int s = netSlotOfId(r.id);
        return s >= 0 ? netFreshMs(gCfg.net[s]) : NET_FRESH_MIN_DEFAULT * 60000UL;
    }
    const unsigned long k = rfNominalIntervalMs(r) * 5 / 2;
    return k > SRC_RF_FRESH_MS ? k : SRC_RF_FRESH_MS;
}

bool rfSensorFresh(const RfSensorReading& r)
{
    return !(r.staleMask & RF_VAL_ANY) && millis() - r.lastMs <= rfFreshMs(r);
}

bool rfTableOn() { return gCfg.rfEn || netAnyOn(gCfg.net); }

bool rfSameKind(const RfSensorReading& a, const RfSensorReading& b)
{
    if (a.proto != b.proto || rfIsNetProto(a.proto)) return false;   // a network sensor has no battery change
    switch (a.proto) {
        case RF_PROTO_ITPLUS17:
        case RF_PROTO_ITPLUS9:    return (a.id & RF_ID_ITPLUS_HUM) == (b.id & RF_ID_ITPLUS_HUM);
        case RF_PROTO_FINEOFFSET: return (a.model == b.model) || (a.model && b.model && !strcmp(a.model, b.model));
        case RF_PROTO_BRESSER:    return a.type == b.type;
        default:                  return true;
    }
}

int rfReplaceCandidate(const RfSensorReading* all, int n, const RfSensorReading& star, unsigned long now)
{
    const unsigned long fresh = rfFreshMs(star);
    if (!(star.staleMask & RF_VAL_ANY) && now - star.lastMs <= fresh) return -1;   // not silent
    int found = -1;
    for (int i = 0; i < n; i++) {
        const RfSensorReading& c = all[i];
        if (!c.used || c.id == star.id || rfStateOf(c.id) != RF_STATE_RECEIVED || !rfSameKind(star, c)) continue;
        // first heard after the ★ one's last packet (a battery change: the new ID sends within seconds);
        // one that was already there while the ★ one still spoke is a neighbour
        if (!c.firstMs || (long)(c.firstMs - star.lastMs) <= 0) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

int rfTwinIndex(uint32_t id)
{
    const uint32_t twin = rfItPlusTwin(id);
    if (!twin) return -1;
    for (int i = 0; i < RF_MAX_SENSORS; i++) if (gRfSensors[i].used && gRfSensors[i].id == twin) return i;
    return -1;
}

void rfSensorsClear()
{
    rfStatsClear();
    for (RfSensorReading& r : gRfSensors) r = RfSensorReading();
}

void rfExpireValues(unsigned long now)
{
    static const uint8_t bits[] = { RF_VAL_TEMP, RF_VAL_HUM, RF_VAL_WIND, RF_VAL_RAIN, RF_VAL_UV, RF_VAL_LIGHT, RF_VAL_PRESS, RF_VAL_ANY };
    for (RfSensorReading& r : gRfSensors) {
        if (!r.used) continue;
        for (uint8_t b : bits)
            if (!(r.staleMask & b) && now - rfValueMs(r, b) > rfFreshMs(r)) r.staleMask |= b;
        receptionStopIfSilent(r.reception, (uint32_t)now);   // wrap-safe end of the window, like staleMask
    }
    for (RfHiddenSeen& h : gRfHidden)
        if (h.id && now - h.lastMs > RF_HIDDEN_RANGE_MS) h.outOfRange = true;
}

const char* rfProtoName(int proto)
{
    switch (proto) {
        case RF_PROTO_ITPLUS17:   return "IT+ 17.241 kbit/s";
        case RF_PROTO_ITPLUS9:    return "IT+ 9.579 kbit/s";
        case RF_PROTO_FINEOFFSET: return "Fine Offset";
        case RF_PROTO_DAVIS:      return "Davis";
        case RF_PROTO_MQTT:       return "MQTT";
        case RF_PROTO_REST:       return "REST";
        default:                  return "Bresser";
    }
}

int rfPhaseSet(int protoSet)
{
    int p = protoSet & (RF_PROTO_BRESSER | RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9);
    if (protoSet & RF_PROTO_FINEOFFSET) p |= RF_PROTO_ITPLUS17;   // same receive phase
    return p ? p : RF_PROTO_BRESSER;
}

int rfProtoSetFor(int cfgProto)
{
    if (cfgProto == RF_PROTO_AUTO) return RF_PROTO_ALL;
    if (cfgProto == RF_PROTO_DAVIS) return 0;
    int p = cfgProto & RF_PROTO_ALL;
    return p ? p : RF_PROTO_BRESSER;
}

int rfNextPhase(int phaseSet, int cur)
{
    for (int i = 0; i < 3; i++) {
        cur = cur == RF_PROTO_ITPLUS9 ? RF_PROTO_BRESSER : cur << 1;
        if (phaseSet & cur) return cur;
    }
    return RF_PROTO_BRESSER;
}

unsigned long rfPhaseDwellMs(int phase, bool fineOffsetPhase)
{
    if (phase == RF_PROTO_BRESSER) return RF_DWELL_BRESSER_MS;
    return fineOffsetPhase ? RF_DWELL_FINEOFFSET_MS : RF_DWELL_ITPLUS_MS;
}

int rfAutoDetectedProto(int receivedProto)
{
    switch (receivedProto) {
        case RF_PROTO_BRESSER:    return RF_PROTO_BRESSER;
        case RF_PROTO_ITPLUS17:
        case RF_PROTO_FINEOFFSET: return RF_PROTO_ITPLUS17 | RF_PROTO_FINEOFFSET;
        case RF_PROTO_ITPLUS9:    return RF_PROTO_ITPLUS9;
        default:                  return 0;
    }
}

bool rfAutoShouldLock(int cfgProto, bool idAccepted)
{
    return cfgProto == RF_PROTO_AUTO && idAccepted;
}

// ---- sensor states ----

namespace {
bool inList(const std::vector<uint32_t>& v, uint32_t id)
{
    for (uint32_t x : v) if (x == id) return true;
    return false;
}
void removeId(std::vector<uint32_t>& v, uint32_t id)
{
    for (size_t i = 0; i < v.size();) { if (v[i] == id) v.erase(v.begin() + i); else i++; }
}
}

void rfFormatIds(const std::vector<uint32_t>& ids, char* out, size_t len)
{
    if (!len) return;
    out[0] = '\0';
    size_t n = 0;
    for (uint32_t id : ids) {
        if (n + 11 >= len) break;
        n += (size_t)snprintf(out + n, len - n, "%s%08lX", n ? ", " : "", (unsigned long)id);
    }
}

bool rfEcowittId(uint32_t id, uint32_t& shortId)
{
    static const uint8_t fam[] = { 0x20, 0x24, 0x25, 0x30, 0x32, 0x34, 0x35, 0x37, 0x38, 0x40, 0x41, 0x42, 0x43, 0x45,
                                   0x46, 0x51, 0x55, 0x57, 0x68, 0x80, 0x85, 0x90, 0xA2 };   // FineOffsetDecoder.h FO_FAMILY_*
    const uint8_t f = (uint8_t)(id >> 24);
    bool fo = false;
    for (uint8_t x : fam) if (x == f) fo = true;
    if (!fo) return false;
    const bool id8 = f == 0x30 || f == 0x37 || f == 0x25 || f == 0x32;   // WH31 (channel in bits 8-15), WH25 / WH32
    shortId = id8 ? (id & 0xFF) : (id & 0xFFFFFF);
    return true;
}

bool rfNormalizeShortId(uint32_t fullId)
{
    // only the hidden list is typed by hand (★ sensors are taken over from the table)
    uint32_t s;
    if (!rfEcowittId(fullId, s)) return false;
    std::vector<uint32_t> ids = rfParseIds(gCfg.rfIgnore);
    bool hit = false;
    for (uint32_t& x : ids) if (x != fullId && x == s && x < 0x1000000UL) { x = fullId; hit = true; }
    if (!hit) return false;
    std::vector<uint32_t> uniq;   // the full ID may already be listed too
    for (uint32_t x : ids) if (!inList(uniq, x)) uniq.push_back(x);
    rfFormatIds(uniq, gCfg.rfIgnore, sizeof(gCfg.rfIgnore));
    return true;
}

RfSensorState rfStateOf(uint32_t id)
{
    if (inList(rfParseIds(gCfg.rfIgnore), id)) return RF_STATE_HIDDEN;
    LsGuard g;   // radio path: a lookup, microseconds (lsensors.h, "concurrent access")
    if (lsFindByRadioId(gLogicalSensors, id) >= 0)             return RF_STATE_MINE;
    return RF_STATE_RECEIVED;
}

bool rfOnlyMineActive() { return gCfg.rfOnlyMine && lsMineCount() > 0; }

bool rfIdAccepted(uint32_t id)
{
    RfSensorState st = rfStateOf(id);
    if (st == RF_STATE_HIDDEN) return false;
    return st == RF_STATE_MINE || !rfOnlyMineActive();
}

bool rfSetState(uint32_t id, RfSensorState st)
{
    if (!id) return false;
    std::vector<uint32_t> hidden = rfParseIds(gCfg.rfIgnore);
    LsGuard g;   // a writer (web task): the whole change under the table's lock
    const int mine = lsFindByRadioId(gLogicalSensors, id);
    if (st == RF_STATE_MINE && mine < 0) {
        // take over: a logical sensor (lsensors.h) with the received-only name
        // and minimum signal, which then live in its entry
        const int i = lsAdd(gLogicalSensors, id, rfNameOf(id), lsRandom);
        if (i < 0) return false;
        gLogicalSensors.s[i].minRssiDbm = (int8_t)rfMinRssiLookup(gCfg.rfMinRssi, id);
        rfNameSet(gCfg.rfNames, sizeof(gCfg.rfNames), id, "");
        rfMinRssiSet(gCfg.rfMinRssi, sizeof(gCfg.rfMinRssi), id, 0);
    }
    if (st == RF_STATE_HIDDEN && !inList(hidden, id) && hidden.size() >= (size_t)RF_MAX_SENSORS) return false;
    if (st != RF_STATE_MINE && mine >= 0) {
        // given up: name and minimum signal go back to the received-only lists
        rfNameSet(gCfg.rfNames, sizeof(gCfg.rfNames), id, gLogicalSensors.s[mine].displayName);
        rfMinRssiSet(gCfg.rfMinRssi, sizeof(gCfg.rfMinRssi), id, gLogicalSensors.s[mine].minRssiDbm);
        lsRemove(gLogicalSensors, mine);
    }
    if (st != RF_STATE_HIDDEN) removeId(hidden, id);
    if (st == RF_STATE_HIDDEN && !inList(hidden, id)) hidden.push_back(id);
    rfFormatIds(hidden, gCfg.rfIgnore, sizeof(gCfg.rfIgnore));
    return true;
}

bool rfMoveMine(uint32_t id, int dir)
{
    LsGuard g;   // a writer
    const int i = lsFindByRadioId(gLogicalSensors, id), j = i + (dir < 0 ? -1 : 1);
    if (i < 0 || j < 0 || j >= gLogicalSensors.count()) return false;
    lsMove(gLogicalSensors, i, dir < 0 ? -1 : 1);
    return true;
}

void rfRemoveRejected()
{
    for (RfSensorReading& r : gRfSensors)
        // Davis (exclusive mode) knows no "Only my sensors", but a hidden one goes too
        if (r.used && (r.proto == RF_PROTO_DAVIS ? rfStateOf(r.id) == RF_STATE_HIDDEN : !rfIdAccepted(r.id)))
            r = RfSensorReading();
}

void rfNoteHidden(uint32_t id, const char* model, float rssi, unsigned long now)
{
    int slot = -1;
    for (int i = 0; i < RF_HIDDEN_SEEN && slot < 0; i++) if (gRfHidden[i].id == id) slot = i;
    for (int i = 0; i < RF_HIDDEN_SEEN && slot < 0; i++) if (!gRfHidden[i].id) slot = i;
    if (slot < 0) {   // full: the one heard longest ago
        slot = 0;
        for (int i = 1; i < RF_HIDDEN_SEEN; i++)
            if (now - gRfHidden[i].lastMs > now - gRfHidden[slot].lastMs) slot = i;
    }
    RfHiddenSeen& h = gRfHidden[slot];
    if (h.id != id) { h = RfHiddenSeen(); h.id = id; }
    if (model) h.model = model;
    h.rssi = rssi;
    h.lastMs = now;
    h.outOfRange = false;
    h.packets++;
}

bool rfHiddenSeenInRange(const RfHiddenSeen& h, unsigned long now)
{
    return h.id && !h.outOfRange && now - h.lastMs <= RF_HIDDEN_RANGE_MS && rfStateOf(h.id) == RF_STATE_HIDDEN;
}

int rfHiddenInRange(unsigned long now)
{
    int n = 0;
    for (const RfHiddenSeen& h : gRfHidden) if (rfHiddenSeenInRange(h, now)) n++;
    return n;
}
