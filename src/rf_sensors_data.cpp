// Latest wireless-sensor values (filled by rf_sensors.cpp). Kept in its own
// file without RadioLib so the JSON/telemetry host tests can link it.
#include "rf_sensors.h"
#include "config.h"
#include "davis_diag.h"

RfSensorReading gRfSensors[RF_MAX_SENSORS];
DavisDiag gDavisDiag;

#include <stdlib.h>
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
        default:           return r.lastMs;
    }
}

float rfStoreValues(RfSensorReading& r, const RfValues& v, unsigned long now, unsigned long& rainGapMs)
{
    const uint8_t wasStale = r.staleMask;
    r.lastMs = now;
    r.staleMask &= (uint8_t)~(v.ok | RF_VAL_ANY);
    // Values not in this packet are kept (a 6-in-1 sends two alternating messages).
    if (v.ok & RF_VAL_TEMP) { r.hasTemp = true;     r.tempC = v.tempC;       r.tempMs = now; }
    if (v.ok & RF_VAL_HUM)  { r.hasHumidity = true; r.humidity = v.humidity; r.humMs = now; }
    if (v.ok & RF_VAL_WIND) {
        r.hasWind    = true;
        r.windMs     = now;
        r.windAvgMs  = v.windAvgMs;
        r.windGustMs = v.windGustMs;
        r.windDirDeg = v.windDirDeg;
    }
    float rainDelta = 0.0f;
    rainGapMs = 0;
    if (v.ok & RF_VAL_RAIN) {
        // Increase since the previous packet; a smaller total = sensor reset
        // (battery change). No previous value (node start): 0 - the rain while
        // the node was down is unknown.
        if (r.hasRain) {
            rainDelta = v.rainTotalMm >= r.rainTotalMm ? v.rainTotalMm - r.rainTotalMm : v.rainTotalMm;
            unsigned long age = now - r.rainMs;
            // expired across a millis() wrap: the age is unknown, at least the window
            if (age > SRC_RF_FRESH_MS || (wasStale & RF_VAL_RAIN))
                rainGapMs = age > SRC_RF_FRESH_MS ? age : SRC_RF_FRESH_MS + 1;
        }
        r.hasRain = true;  r.rainTotalMm = v.rainTotalMm;  r.rainMs = now;
    }
    if (v.ok & RF_VAL_UV)    { r.hasUv = true;    r.uvIndex = v.uvIndex;   r.uvMs = now; }
    if (v.ok & RF_VAL_LIGHT) { r.hasLight = true; r.lightLux = v.lightLux; r.lightMs = now; }
    return rainDelta;
}

int rfSlotFor(uint32_t id, unsigned long now, unsigned long takeOverMs, int onlyProto)
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
        if (age > takeOverMs && (oldIdx < 0 || age > oldAge)) { oldIdx = i; oldAge = age; }
    }
    if (freeIdx >= 0) return freeIdx;
    if (oldIdx >= 0) gRfSensors[oldIdx] = RfSensorReading();   // sensor gone (battery change: new ID)
    return oldIdx;
}

bool rfIsIndoor(const RfSensorReading& r)
{
    if (!r.used || r.proto != RF_PROTO_FINEOFFSET) return false;
    const uint8_t family = (uint8_t)(r.id >> 24);
    return family == FO_FAMILY_WH45 || family == FO_FAMILY_WH46 || family == FO_FAMILY_WH25;
}

void rfSensorsClear()
{
    for (RfSensorReading& r : gRfSensors) r = RfSensorReading();
}

void rfExpireValues(unsigned long now)
{
    static const uint8_t bits[] = { RF_VAL_TEMP, RF_VAL_HUM, RF_VAL_WIND, RF_VAL_RAIN, RF_VAL_UV, RF_VAL_LIGHT, RF_VAL_ANY };
    for (RfSensorReading& r : gRfSensors) {
        if (!r.used) continue;
        for (uint8_t b : bits)
            if (!(r.staleMask & b) && now - rfValueMs(r, b) > SRC_RF_FRESH_MS) r.staleMask |= b;
    }
}

const char* rfProtoName(int proto)
{
    switch (proto) {
        case RF_PROTO_ITPLUS17:   return "IT+ 17.241 kbit/s";
        case RF_PROTO_ITPLUS9:    return "IT+ 9.579 kbit/s";
        case RF_PROTO_FINEOFFSET: return "Fine Offset";
        case RF_PROTO_DAVIS:      return "Davis";
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
