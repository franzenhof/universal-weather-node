// Network sensors (net_sensor.h, net_json.h): the host-tested part - topics,
// payloads, the field mapping, the preset, the table write.
#include "net_json.h"
#include "psram_json.h"
#include "rf_sensors.h"
#include "sources.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

NetSlotStatus gNetStatus[NET_SLOTS];

void netSanitize(NetSlotCfg& c)
{
    if (c.type >= NET_TYPE_COUNT) c.type = NET_OFF;
    if (c.format >= NET_FMT_COUNT || (c.format == NET_FMT_PLAIN && c.type == NET_REST)) c.format = NET_FMT_JSON;
    if (c.freshMin < 1 || c.freshMin > NET_FRESH_MIN_MAX) c.freshMin = NET_FRESH_MIN_DEFAULT;
    if (c.pollSec < NET_POLL_SEC_MIN || c.pollSec > NET_POLL_SEC_MAX) c.pollSec = NET_POLL_SEC_DEFAULT;
    c.addr[NET_ADDR_LEN - 1] = '\0';
    for (int f = 0; f < VF_COUNT; f++) c.key[f][NET_KEY_LEN - 1] = '\0';
}

const char* netFormatKey(uint8_t f)
{
    static const char* const keys[NET_FMT_COUNT] = { "json", "plain", "uwn" };
    return f < NET_FMT_COUNT ? keys[f] : "";
}

const char* netErrKey(uint8_t e)
{
    static const char* const keys[] = { "", "http", "json", "noValues", "refused", "wait" };
    return e < sizeof(keys) / sizeof(keys[0]) ? keys[e] : "";
}

bool netParseNumber(const char* s, size_t len, float& out)
{
    char b[32];
    while (len && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    if (!len || len >= sizeof(b)) return false;
    memcpy(b, s, len);
    b[len] = '\0';
    char* end = nullptr;
    const float v = strtof(b, &end);
    if (end == b || *end || !isfinite(v)) return false;
    out = v;
    return true;
}

namespace {
bool hasMqttWildcard(const char* s) { return strpbrk(s, "+#") != nullptr; }
}

bool netSlotTopicsValid(const NetSlotCfg& c)
{
    if (c.type != NET_MQTT) return true;
    if (hasMqttWildcard(c.addr)) return false;
    if (c.format != NET_FMT_PLAIN) return c.addr[0] != '\0';
    for (int f = 0; f < VF_COUNT; f++) if (hasMqttWildcard(c.key[f])) return false;
    return true;
}

bool netTopicOf(const NetSlotCfg& c, int field, char* out, size_t len)
{
    if (c.type != NET_MQTT || !len) return false;
    out[0] = '\0';
    if (hasMqttWildcard(c.addr)) return false;
    if (c.format == NET_FMT_PLAIN) {
        if (field < 0 || field >= VF_COUNT || !c.key[field][0] || hasMqttWildcard(c.key[field])) return false;
        const size_t la = strlen(c.addr), lk = strlen(c.key[field]);
        if (la + lk + 1 > len) return false;
        memcpy(out, c.addr, la);
        memcpy(out + la, c.key[field], lk + 1);
        return true;
    }
    if (field != -1 || !c.addr[0] || strlen(c.addr) + 1 > len) return false;
    strcpy(out, c.addr);
    return true;
}

int netMatchTopic(const NetSlotCfg cfg[NET_SLOTS], const char* topic, int* field)
{
    for (int i = 0; i < NET_SLOTS; i++) {
        const NetSlotCfg& c = cfg[i];
        if (c.type != NET_MQTT) continue;
        if (c.format != NET_FMT_PLAIN) {
            if (c.addr[0] && !strcmp(topic, c.addr)) { if (field) *field = -1; return i; }
            continue;
        }
        const size_t la = strlen(c.addr);
        if (strncmp(topic, c.addr, la)) continue;
        for (int f = 0; f < VF_COUNT; f++)
            if (c.key[f][0] && !strcmp(topic + la, c.key[f])) { if (field) *field = f; return i; }
    }
    return -1;
}

bool netToRfValues(uint8_t format, const VsValues& in, NetWindMemo& slotMemo, RfValues& out)
{
    out = RfValues();
    NetWindMemo thisMessageOnly;
    NetWindMemo& memo = format == NET_FMT_PLAIN ? slotMemo : thisMessageOnly;
    auto has = [&in](int f) { return (in.has & (1u << f)) && isfinite(in.v[f]); };
    if (has(VF_TEMP)) { out.ok |= RF_VAL_TEMP; out.tempC = in.v[VF_TEMP]; }
    if (has(VF_HUM)) {
        const long h = lroundf(in.v[VF_HUM]);
        out.ok |= RF_VAL_HUM; out.humidity = (uint8_t)(h < 0 ? 0 : h > 100 ? 100 : h);
    }
    if (has(VF_PRESS)) { out.ok |= RF_VAL_PRESS; out.pressureHpa = in.v[VF_PRESS]; }
    if (has(VF_GUST))  { memo.gust = in.v[VF_GUST]; memo.gustKnown = true; }
    if (has(VF_DIR))   { float d = fmodf(in.v[VF_DIR], 360.0f); memo.dir = d < 0.0f ? d + 360.0f : d; memo.dirKnown = true; }
    if (has(VF_WIND)) {
        out.ok |= RF_VAL_WIND;
        out.windAvgMs  = in.v[VF_WIND];
        out.windGustMs = memo.gustKnown ? memo.gust : out.windAvgMs;
        out.windDirDeg = memo.dirKnown ? memo.dir : 0.0f;
    }
    if (has(VF_RAIN)) { out.ok |= RF_VAL_RAIN;  out.rainTotalMm = in.v[VF_RAIN]; }
    if (has(VF_UV))   { out.ok |= RF_VAL_UV;    out.uvIndex = in.v[VF_UV]; }
    if (has(VF_LUX))  { out.ok |= RF_VAL_LIGHT; out.lightLux = in.v[VF_LUX]; }
    return out.ok != 0;
}

bool netApply(int slot, uint8_t type, const RfValues& v, unsigned long now)
{
    if (slot < 0 || slot >= NET_SLOTS || !v.ok) return false;
    const uint32_t id = netSensorId(slot);
    if (!rfIdAccepted(id)) return false;
    const int idx = rfSlotFor(id, now, SRC_RF_FRESH_MS, 0, rfStateOf(id) == RF_STATE_MINE);
    if (idx < 0) return false;
    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = id;
    r.type      = 0;
    r.decoder   = 0;
    r.proto     = type == NET_REST ? RF_PROTO_REST : RF_PROTO_MQTT;
    r.rssi      = 0.0f;
    r.batteryOk = true;
    r.packets++;
    unsigned long rainGapMs;
    const float rainDelta = rfStoreValues(r, v, now, rainGapMs);
    srcRfPacket(idx, v.ok & RF_VAL_WIND, rainDelta, v.ok & RF_VAL_LIGHT, rainGapMs);
    return true;
}

// ---- JSON ----

namespace {
bool digits(const char* s)
{
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

// The preset: what another node of this firmware puts into <prefix>/data and
// /api/data (json_payload.cpp), and the unit field of each value.
struct UwnKey { uint8_t field; const char* key; const char* unitKey; };
const UwnKey UWN_KEYS[] = {
    { VF_TEMP,  "temp",           "tempUnit" },
    { VF_HUM,   "humidityPct",    nullptr },
    { VF_PRESS, "pressureAbs",    "pressureUnit" },
    { VF_WIND,  "windSpeed",      "windUnit" },
    { VF_GUST,  "windGust",       "windUnit" },
    { VF_DIR,   "windDirDeg",     nullptr },
    { VF_RAIN,  "rainSinceStart", "rainUnit" },
    { VF_UV,    "uvIndex",        nullptr },
    { VF_LUX,   "lightLux",       nullptr },
};

// another node's output unit -> metric (units.cpp, the other way round)
float uwnToMetric(uint8_t field, float v, const char* unit)
{
    if (!unit) return v;
    switch (field) {
        case VF_TEMP:  return !strcmp(unit, "F") ? (v - 32.0f) / 1.8f : v;
        case VF_PRESS: return !strcmp(unit, "inHg") ? v * 33.8639f : v;
        case VF_WIND:
        case VF_GUST:  return !strcmp(unit, "km/h") ? v / 3.6f : !strcmp(unit, "mph") ? v / 2.23694f : v;
        case VF_RAIN:  return !strcmp(unit, "in") ? v * 25.4f : v;
        default:       return v;
    }
}

void filterPath(JsonDocument& filter, const char* path)
{
    if (!path || !path[0]) return;
    if (filter.isNull()) filter.to<JsonObject>();
    JsonVariant v = filter.as<JsonVariant>();
    char seg[NET_KEY_LEN];
    for (const char* p = path; *p;) {
        if (v.is<bool>()) return;   // a shorter path keeps this whole part already
        const char* dot = strchr(p, '.');
        const size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (!n || n >= sizeof(seg)) return;
        memcpy(seg, p, n);
        seg[n] = '\0';
        JsonVariant child;
        if (digits(seg)) {   // an array index: the filter's first element applies to every element
            JsonArray a = v.is<JsonArray>() ? v.as<JsonArray>() : v.to<JsonArray>();
            child = a.size() ? a[0].as<JsonVariant>() : a.add<JsonVariant>();
        } else {             // existing parts are kept (two keys under "main")
            JsonObject o = v.is<JsonObject>() ? v.as<JsonObject>() : v.to<JsonObject>();
            child = o[(const char*)seg].as<JsonVariant>();
            if (child.isUnbound()) child = o[(const char*)seg].to<JsonVariant>();
        }
        v = child;
        if (!dot) break;
        p = dot + 1;
    }
    v.set(true);
}
} // namespace

bool netPath(JsonVariantConst root, const char* path, float& out)
{
    if (!path || !path[0]) return false;
    JsonVariantConst v = root;
    char seg[NET_KEY_LEN];
    for (const char* p = path; *p;) {
        const char* dot = strchr(p, '.');
        const size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (!n || n >= sizeof(seg)) return false;
        memcpy(seg, p, n);
        seg[n] = '\0';
        if (v.is<JsonArrayConst>() && digits(seg)) v = v[(size_t)strtoul(seg, nullptr, 10)];
        else if (v.is<JsonObjectConst>()) v = v[(const char*)seg];
        else return false;
        if (v.isNull()) return false;
        if (!dot) break;
        p = dot + 1;
        if (!*p) return false;
    }
    if (v.is<bool>()) return false;
    if (v.is<float>()) {
        const float f = v.as<float>();
        if (!isfinite(f)) return false;
        out = f;
        return true;
    }
    const char* s = v.as<const char*>();
    return s && netParseNumber(s, strlen(s), out);
}

void netFilter(const NetSlotCfg& c, JsonDocument& filter)
{
    filter.clear();
    if (c.format == NET_FMT_UWN) {
        for (const UwnKey& k : UWN_KEYS) {
            filter[k.key] = true;
            if (k.unitKey) filter[k.unitKey] = true;
        }
        return;
    }
    for (int f = 0; f < VF_COUNT; f++) filterPath(filter, c.key[f]);
}

void netExtract(const NetSlotCfg& c, JsonVariantConst root, VsValues& out)
{
    out = VsValues();
    float v;
    if (c.format == NET_FMT_UWN) {
        for (const UwnKey& k : UWN_KEYS) {
            if (!netPath(root, k.key, v)) continue;
            out.v[k.field] = uwnToMetric(k.field, v, k.unitKey ? root[k.unitKey].as<const char*>() : nullptr);
            out.has |= (uint16_t)(1u << k.field);
        }
        return;
    }
    for (int f = 0; f < VF_COUNT; f++)
        if (c.key[f][0] && netPath(root, c.key[f], v)) { out.v[f] = v; out.has |= (uint16_t)(1u << f); }
}

uint8_t netParseJson(const NetSlotCfg& c, const uint8_t* json, size_t len, VsValues& out)
{
    out = VsValues();
    JsonDocument filter(psramJsonAllocator()), doc(psramJsonAllocator());
    netFilter(c, filter);
    if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return NET_ERR_JSON;
    netExtract(c, doc.as<JsonVariantConst>(), out);
    return out.has ? NET_ERR_NONE : NET_ERR_NO_VALUES;
}

uint8_t netRestResult(const NetSlotCfg& c, int httpCode, DeserializationError err, JsonVariantConst root, VsValues& out)
{
    out = VsValues();
    if (httpCode != 200) return NET_ERR_HTTP;
    if (err) return NET_ERR_JSON;
    netExtract(c, root, out);
    return out.has ? NET_ERR_NONE : NET_ERR_NO_VALUES;
}
