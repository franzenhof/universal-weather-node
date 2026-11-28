#include "alarms.h"
#include "lsensors.h"
#include <stddef.h>
#include "units.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

AlarmEvent alarmStep(const AlarmRule& r, AlarmState& s, bool valid, float v, uint32_t now)
{
    s.valid = valid;
    if (!r.enabled || r.src == AS_OFF) {
        const bool was = s.active;
        s = AlarmState();
        return was ? AE_CLEAR : AE_NONE;
    }
    if (!valid || !isfinite(v)) {   // no data: neither raise nor clear; a pending raise starts over
        s.pendingSince = 0;
        return AE_NONE;
    }
    s.value = v;
    const bool raiseCond = r.above ? v > r.raise : v < r.raise;
    const bool clearCond = r.above ? v <= r.clear : v >= r.clear;
    if (!s.active) {
        if (!raiseCond) { s.pendingSince = 0; return AE_NONE; }
        if (!s.pendingSince) s.pendingSince = now ? now : 1;
        if (now - s.pendingSince < (uint32_t)r.minDurationMin * 60U) return AE_NONE;
        s.active = true;
        s.since = s.lastNotify = now;
        s.pendingSince = 0;
        return AE_RAISE;
    }
    if (clearCond) {
        s.active = false;
        s.pendingSince = 0;
        return AE_CLEAR;
    }
    if (r.repeatMin && now - s.lastNotify >= (uint32_t)r.repeatMin * 60U) {
        s.lastNotify = now;
        return AE_REPEAT;
    }
    return AE_NONE;
}

PhKind alarmKind(const AlarmRule& r)
{
    switch (r.src) {
        case AS_TEMP: case AS_FEELS: case AS_DEW: return PK_TEMP;
        case AS_QNH: case AS_PTEND:               return PK_PRESS;
        case AS_WIND: case AS_GUST:               return PK_SPEED;
        case AS_RAINRATE:                         return PK_RATE;
        case AS_RAINDAY: case AS_RAIN1H:          return PK_RAIN;
        case AS_RF:
            switch (r.rfField) {
                case AF_TEMP: return PK_TEMP;
                case AF_WIND: case AF_GUST: return PK_SPEED;
                default: return PK_PLAIN;
            }
        default: return PK_PLAIN;
    }
}

const char* alarmSrcKey(uint8_t src)
{
    static const char* const k[AS_COUNT] = { "off", "temp", "feelsLike", "humidity", "dewPoint", "pressureQnh",
        "pressureTrend3h", "windSpeed", "windGust", "rainRate", "rainToday", "rain1h", "uvIndex", "frost", "fog",
        "sultry", "sensor" };
    return src < AS_COUNT ? k[src] : "off";
}

const char* alarmRfFieldKey(uint8_t f)
{
    static const char* const k[AF_COUNT] = { "temp", "humidity", "windSpeed", "windGust", "uvIndex", "lightLux", "leak",
        "lightningKm", "moisturePct", "co2Ppm", "pm25", "silentMin", "batteryLow", "receptionPct" };
    return f < AF_COUNT ? k[f] : "temp";
}

const char* alarmChannelKey(uint8_t c)
{
    static const char* const k[AC_COUNT] = { "ntfy", "pushover", "telegram", "mqtt", "webhook" };
    return c < AC_COUNT ? k[c] : "";
}

uint16_t alarmCapMask(bool temp, bool hum, bool wind, bool uv, bool light, bool leak, bool lightning, bool moisture,
                      bool co2, bool pm25, bool radio)
{
    uint16_t m = (1u << AF_SILENT_MIN) | (1u << AF_BATTERY_LOW);
    if (radio)     m |= 1u << AF_RECEPTION_PCT;
    if (temp)      m |= 1u << AF_TEMP;
    if (hum)       m |= 1u << AF_HUM;
    if (wind)      m |= (1u << AF_WIND) | (1u << AF_GUST);
    if (uv)        m |= 1u << AF_UV;
    if (light)     m |= 1u << AF_LUX;
    if (leak)      m |= 1u << AF_LEAK;
    if (lightning) m |= 1u << AF_LIGHTNING_KM;
    if (moisture)  m |= 1u << AF_MOISTURE;
    if (co2)       m |= 1u << AF_CO2;
    if (pm25)      m |= 1u << AF_PM25;
    return m;
}

bool alarmTemplate(int tpl, AlarmRule& r)
{
    r = AlarmRule();
    r.enabled = 1;
    r.above = 1;
    r.allClear = 1;
    r.raise = 0.5f; r.clear = 0.5f;
    switch (tpl) {
        case 0: r.src = AS_FROST;  strcpy(r.name, "Frost");  break;
        case 1: r.src = AS_FOG;    strcpy(r.name, "Fog");    break;
        case 2: r.src = AS_SULTRY; strcpy(r.name, "Sultry"); break;
        case 3: r.src = AS_PTEND; r.above = 0; r.raise = -6.0f; r.clear = -3.0f; r.priority = 4; strcpy(r.name, "Storm"); break;
        case 4: r.src = AS_RAINRATE; r.raise = 0.1f; r.clear = 0.0f; r.allClear = 0; strcpy(r.name, "Rain"); break;
        default: return false;
    }
    return true;
}

float alarmToDisplay(PhKind k, float m, const Config& c)
{
    switch (k) {
        case PK_TEMP:  return temperatureForOutput(m, c);
        case PK_SPEED: return windSpeedForOutput(m, c);
        case PK_RAIN:  return rainForOutput(m, c);
        case PK_RATE:  return rainRateForOutput(m, c);
        case PK_PRESS: return pressureForOutput(m, c);
        default:       return m;
    }
}

float alarmFromDisplay(PhKind k, float d, const Config& c)
{
    if (k == PK_TEMP) return c.unitSystem == 1 ? (d - 32.0f) / 1.8f : d;
    const float one = alarmToDisplay(k, 1.0f, c);   // the other conversions are factors
    return one != 0.0f ? d / one : d;
}

const char* alarmUnitLabel(PhKind k, uint8_t src, uint8_t f, const Config& c)
{
    switch (k) {
        case PK_TEMP:  return temperatureUnitLabel(c);
        case PK_SPEED: return windSpeedUnitLabel(c);
        case PK_RAIN:  return rainUnitLabel(c);
        case PK_RATE:  return rainRateUnitLabel(c);
        case PK_PRESS: return pressureUnitLabel(c);
        default: break;
    }
    if (src == AS_HUM) return "%";
    if (src != AS_RF) return "";
    switch (f) {
        case AF_HUM: case AF_MOISTURE: case AF_RECEPTION_PCT: return "%";
        case AF_LUX: return "lx";
        case AF_LIGHTNING_KM: return "km";
        case AF_CO2: return "ppm";
        case AF_PM25: return "ug/m3";
        case AF_SILENT_MIN: return "min";
        default: return "";
    }
}

namespace {
constexpr uint32_t ALARM_MAGIC    = 0x414C5232;   // "ALR2": sensorInternalId (A5c)
constexpr uint32_t ALARM_MAGIC_V1 = 0x414C5231;   // "ALR1": uint32_t rfId at the same place
// "ALR1" rule: 8 bytes of flags, the 4-byte radio ID, then the same fields as now
constexpr size_t ALARM_V1_RULE = sizeof(AlarmRule) - sizeof(AlarmRule::sensorInternalId) + 4;
constexpr size_t ALARM_V1_BLOB = 4 + ALARM_V1_RULE * ALARM_RULES + 4;
uint32_t fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
}

void alarmsEncode(const AlarmRule* rules, uint8_t* out)
{
    const uint32_t m = ALARM_MAGIC;
    memcpy(out, &m, 4);
    memcpy(out + 4, rules, sizeof(AlarmRule) * ALARM_RULES);
    const uint32_t h = fnv1a(out, ALARM_BLOB_SIZE - 4);
    memcpy(out + ALARM_BLOB_SIZE - 4, &h, 4);
}

bool alarmsDecode(const uint8_t* in, size_t len, AlarmRule* rules)
{
    if (len != ALARM_BLOB_SIZE && len != ALARM_V1_BLOB) return false;
    uint32_t m, h;
    memcpy(&m, in, 4);
    memcpy(&h, in + len - 4, 4);
    if (h != fnv1a(in, len - 4)) return false;
    if (len == ALARM_BLOB_SIZE && m == ALARM_MAGIC) {
        memcpy(rules, in + 4, sizeof(AlarmRule) * ALARM_RULES);
    } else if (len == ALARM_V1_BLOB && m == ALARM_MAGIC_V1) {   // before A5c: the radio ID is dropped
        constexpr size_t head = offsetof(AlarmRule, sensorInternalId);
        for (int i = 0; i < ALARM_RULES; i++) {
            const uint8_t* r = in + 4 + ALARM_V1_RULE * i;
            uint8_t* o = reinterpret_cast<uint8_t*>(&rules[i]);
            memset(o, 0, sizeof(AlarmRule));
            memcpy(o, r, head);
            memcpy(o + head + sizeof(AlarmRule::sensorInternalId), r + head + 4, ALARM_V1_RULE - head - 4);
        }
    } else {
        return false;
    }
    for (int i = 0; i < ALARM_RULES; i++) {   // never trust stored strings / enums
        AlarmRule& r = rules[i];
        r.name[sizeof(r.name) - 1] = r.msg[sizeof(r.msg) - 1] = r.clearMsg[sizeof(r.clearMsg) - 1] = '\0';
        if (!lsIsInternalId(r.sensorInternalId)) r.sensorInternalId[0] = '\0';
        if (r.src >= AS_COUNT) r.src = AS_OFF;
        if (r.rfField >= AF_COUNT) r.rfField = AF_TEMP;
        if (r.priority < 1 || r.priority > 5) r.priority = 3;
    }
    return true;
}

void alarmsJson(JsonDocument& doc, const AlarmRule* rules, const AlarmState* st, const AlarmChannels& ch,
                const Config& c, uint32_t now, uint32_t (*sensorOf)(const char* internalId, char* customId))
{
    doc.clear();
    doc["apiVersion"] = 1;
    JsonArray active = doc["active"].to<JsonArray>();
    JsonArray arr = doc["rules"].to<JsonArray>();
    auto round2 = [](float v) { return roundf(v * 100.0f) / 100.0f; };
    for (int i = 0; i < ALARM_RULES; i++) {
        const AlarmRule& r = rules[i];
        if (r.src == AS_OFF) continue;
        const PhKind k = alarmKind(r);
        JsonObject o = arr.add<JsonObject>();
        o["n"]       = i + 1;
        o["name"]    = r.name;
        o["enabled"] = r.enabled != 0;
        o["value"]   = alarmSrcKey(r.src);
        if (r.src == AS_RF) {
            // the bound logical sensor: its custom ID, and its current radio ID (compatibility)
            char customId[17] = "";
            const uint32_t rid = sensorOf && r.sensorInternalId[0] ? sensorOf(r.sensorInternalId, customId) : 0;
            if (rid) {
                char id[12];
                snprintf(id, sizeof(id), "%08lX", (unsigned long)rid);
                o["sensorId"] = id;
            }
            if (customId[0]) o["customId"] = customId;
            o["sensorValue"] = alarmRfFieldKey(r.rfField);
        }
        o["compare"]  = r.above ? ">" : "<";
        o["raise"]    = round2(alarmToDisplay(k, r.raise, c));
        o["clear"]    = round2(alarmToDisplay(k, r.clear, c));
        o["unit"]     = alarmUnitLabel(k, r.src, r.rfField, c);
        o["minDurationMin"] = r.minDurationMin;
        o["repeatMin"] = r.repeatMin;
        o["priority"] = r.priority;
        JsonArray chs = o["channels"].to<JsonArray>();
        for (uint8_t b = 0; b < AC_COUNT; b++) if (r.channels & (1 << b)) chs.add(alarmChannelKey(b));
        o["allClear"] = r.allClear != 0;
        o["active"]   = st[i].active;
        if (st[i].active) {
            o["activeSec"] = now - st[i].since;
            active.add(i + 1);
        }
        if (st[i].valid) o["current"] = round2(alarmToDisplay(k, st[i].value, c));
    }
    // configured channels (booleans only - never addresses or tokens)
    JsonObject cc = doc["channels"].to<JsonObject>();
    cc["ntfy"]     = ch.ntfyTopic[0] != '\0';
    cc["pushover"] = ch.poToken[0] && ch.poUser[0];
    cc["telegram"] = ch.tgToken[0] && ch.tgChat[0];
    cc["mqtt"]     = ch.mqttEn != 0;
    cc["webhook"]  = ch.whUrl[0] != '\0';
}
