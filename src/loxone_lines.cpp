// Loxone UDP lines from the telemetry JsonDocument (see loxone.h). Pure: no
// Arduino/network dependency, host-tested (test/firmware_loxone.cpp).
#include "loxone.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

bool loxPrefixValid(const char* s)
{
    if (!s) return true;
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return n <= (size_t)LOX_PREFIX_MAX;
}

void loxSanitizePrefix(char* s)
{
    if (!s) return;
    size_t w = 0;
    for (size_t r = 0; s[r] && w < (size_t)LOX_PREFIX_MAX; r++) {
        char one[2] = { s[r], 0 };
        if (loxPrefixValid(one)) s[w++] = s[r];
    }
    s[w] = '\0';
}

namespace {

struct Walk {
    const char* prefix;
    LoxLineFn   fn;
    void*       ctx;
    bool        imperial;
    int         count;
};

bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
bool endsWith(const char* s, const char* p)
{
    size_t a = strlen(s), b = strlen(p);
    return a >= b && strcmp(s + a - b, p) == 0;
}

// Decimals of a float value by its JSON key (the leaf key; for flattened
// names the nested key): where buildJsonPayload() rounds, the same.
int decimalsFor(const char* key, bool imperial)
{
    if (startsWith(key, "rain"))       return imperial ? 2 : 1;   // rain, rainRate, rain1h.., rainTotal
    if (!strcmp(key, "pressureTrend3h")) return 2;
    if (startsWith(key, "pressure"))   return imperial ? 2 : 1;   // inHg / hPa
    if (!strcmp(key, "supercapV"))     return 1;
    if (!strcmp(key, "lightLux") || !strcmp(key, "pvW")) return 0;
    if (!strcmp(key, "vpd") || !strcmp(key, "uptimeH") || endsWith(key, "V") || endsWith(key, "Volt") ||
        endsWith(key, "Volts") || endsWith(key, "A") || endsWith(key, "KWh")) return 2;
    return 1;   // temperatures, wind, humidity, UV index, ...
}

// Formats v; false = not a sendable value (string, null, NaN, object, ...).
bool formatValue(JsonVariantConst v, const char* key, bool imperial, char* out, size_t len)
{
    if (v.is<bool>()) { snprintf(out, len, "%d", v.as<bool>() ? 1 : 0); return true; }
    if (v.is<long>())          { snprintf(out, len, "%ld", v.as<long>()); return true; }
    if (v.is<unsigned long>()) { snprintf(out, len, "%lu", v.as<unsigned long>()); return true; }
    if (!v.is<double>()) return false;
    double d = v.as<double>();
    if (!isfinite(d)) return false;
    snprintf(out, len, "%.*f", decimalsFor(key, imperial), d);
    // "-0.0" -> "0.0"
    if (out[0] == '-') {
        bool zero = true;
        for (const char* p = out + 1; *p; p++) if (*p != '0' && *p != '.') { zero = false; break; }
        if (zero) memmove(out, out + 1, strlen(out));
    }
    return true;
}

void emit(Walk& w, const char* name, JsonVariantConst v, const char* key)
{
    char val[32];
    if (!formatValue(v, key, w.imperial, val, sizeof(val))) return;
    char line[128];
    int n = snprintf(line, sizeof(line), "%s%s=%s", w.prefix, name, val);
    if (n <= 0 || n >= (int)sizeof(line)) return;
    w.fn(line, (size_t)n, w.ctx);
    w.count++;
}

// parent + Key (first letter upper case): wind2 + speed -> wind2Speed
void joinName(char* out, size_t len, const char* parent, const char* child)
{
    snprintf(out, len, "%s%s", parent, child);
    size_t p = strlen(parent);
    if (p < len && out[p] >= 'a' && out[p] <= 'z') out[p] = (char)(out[p] - 'a' + 'A');
}

// Flattened object: warnings -> warnFrost, wind2 -> wind2Speed, ...
void emitObject(Walk& w, const char* parent, JsonObjectConst o)
{
    for (JsonPairConst kv : o) {
        char name[64];
        joinName(name, sizeof(name), parent, kv.key().c_str());
        emit(w, name, kv.value(), kv.key().c_str());
    }
}

// Stale group ("stale" array in rfSensors) of a wireless sensor value key.
const char* staleGroup(const char* key)
{
    if (!strcmp(key, "temp"))        return "temp";
    if (!strcmp(key, "humidityPct")) return "humidity";
    if (startsWith(key, "wind"))     return "wind";
    if (!strcmp(key, "rainTotal"))   return "rain";
    if (!strcmp(key, "uvIndex"))     return "uv";
    if (!strcmp(key, "lightLux"))    return "light";
    return nullptr;
}

// One wireless sensor: rf<slot>_temp, rf<slot>_humidity, rf<slot>_battery,
// rf<slot>_rssi, rf<slot>_age, and its other numeric values under their keys.
void emitRfSensor(Walk& w, JsonObjectConst o)
{
    long slot = o["slot"] | 0L;
    if (slot < 1 || slot > 9) return;   // not one of the value-source slots 1..4
    JsonArrayConst stale = o["stale"];
    for (JsonPairConst kv : o) {
        const char* key = kv.key().c_str();
        if (!strcmp(key, "slot") || !strcmp(key, "type") || !strcmp(key, "channel")) continue;
        const char* g = staleGroup(key);
        bool isStale = false;
        if (g) for (JsonVariantConst s : stale) if (s.is<const char*>() && !strcmp(s.as<const char*>(), g)) isStale = true;
        if (isStale) continue;
        const char* out = !strcmp(key, "humidityPct") ? "humidity" : !strcmp(key, "batteryOk") ? "battery"
                        : !strcmp(key, "ageSec") ? "age" : key;
        char name[64];
        snprintf(name, sizeof(name), "rf%ld_%s", slot, out);
        emit(w, name, kv.value(), key);
    }
}

} // namespace

int loxoneLines(const JsonDocument& doc, const char* prefix, LoxLineFn fn, void* ctx)
{
    Walk w{ prefix ? prefix : "", fn, ctx, false, 0 };
    if (!fn) return 0;
    const char* cpuUnit = doc["cpuTempUnit"] | "";
    const char* tUnit   = doc["tempUnit"] | "";
    w.imperial = !strcmp(cpuUnit, "F") || !strcmp(tUnit, "F") || !strcmp(doc["rainUnit"] | "", "in");

    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        const char* key = kv.key().c_str();
        JsonVariantConst v = kv.value();
        if (v.is<JsonObjectConst>()) {
            if      (!strcmp(key, "warnings")) emitObject(w, "warn", v.as<JsonObjectConst>());
            else if (!strcmp(key, "wind2") || !strcmp(key, "heater") || !strcmp(key, "fan") ||
                     !strcmp(key, "victron"))  emitObject(w, key, v.as<JsonObjectConst>());
            // status, sources: not sent
        } else if (v.is<JsonArrayConst>()) {
            if (!strcmp(key, "ds18b20")) {
                int i = 0;
                for (JsonVariantConst t : v.as<JsonArrayConst>()) {
                    char name[24];
                    snprintf(name, sizeof(name), "ds18b20_%d", ++i);
                    emit(w, name, t, "ds18b20");   // null (probe gone) is skipped
                }
            } else if (!strcmp(key, "rfSensors")) {
                for (JsonVariantConst s : v.as<JsonArrayConst>())
                    if (s.is<JsonObjectConst>()) emitRfSensor(w, s.as<JsonObjectConst>());
            }
        } else {
            emit(w, key, v, key);
        }
    }
    return w.count;
}
