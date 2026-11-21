// Home Assistant discovery per wireless sensor (see ha_rf.h).
#include "ha_rf.h"
#include "rf_names.h"
#include "globals.h"
#include "units.h"
#include "rf_sensors.h"
#include "sources.h"
#include "json_payload.h"

#include <ArduinoJson.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#if __has_include("generated_version.h")
#include "generated_version.h"
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

static_assert(HA_RF_IDS_LEN >= RF_MAX_SENSORS * 9 + 1, "every listed ID must fit the stored list");

namespace {

enum Unit : uint8_t { U_NONE, U_TEMP, U_WIND, U_RAIN, U_PCT, U_DEG, U_LX, U_WM2, U_V, U_KM, U_DBM, U_S, U_UV,
                     U_PRESS, U_UGM3, U_PPM, U_USCM };

struct EntityDef {
    const char* key;          // unique_id / config topic suffix
    const char* name;         // entity name (HA prefixes the device name)
    bool        binary;
    Unit        unit;
    const char* deviceClass;
    const char* stateClass;
    const char* field;        // rfSensors[] key
    const char* staleGroup;   // rfSensors[].stale group; nullptr = none
    bool        diagnostic;   // entity_category diagnostic, node availability only
};

// Order = mask bit; never reorder (only appended), the keys are the unique IDs.
const EntityDef ENTITIES[HA_RF_ENTITY_COUNT] = {
    { "temp",               "Temperature",        false, U_TEMP, "temperature",     "measurement",      "temp",            "temp",     false },
    { "humidity",           "Humidity",           false, U_PCT,  "humidity",        "measurement",      "humidityPct",     "humidity", false },
    { "wind_speed",         "Wind Speed",         false, U_WIND, "wind_speed",      "measurement",      "windSpeed",       "wind",     false },
    { "wind_gust",          "Wind Gust",          false, U_WIND, "wind_speed",      "measurement",      "windGust",        "wind",     false },
    { "wind_dir",           "Wind Direction",     false, U_DEG,  nullptr,           nullptr,            "windDirDeg",      "wind",     false },
    // the sensor's own counter: a decrease (battery change) = new meter cycle
    { "rain_total",         "Rain Total",         false, U_RAIN, "precipitation",   "total_increasing", "rainTotal",       "rain",     false },
    { "uv_index",           "UV Index",           false, U_UV,   nullptr,           "measurement",      "uvIndex",         "uv",       false },
    { "light",              "Illuminance",        false, U_LX,   "illuminance",     "measurement",      "lightLux",        "light",    false },
    { "solar",              "Solar Radiation",    false, U_WM2,  "irradiance",      "measurement",      "solarWm2",        nullptr,    false },
    { "soil_moisture",      "Soil Moisture",      false, U_PCT,  "moisture",        "measurement",      "soilMoisturePct", nullptr,    false },
    // 8-bit counter of the sensor: wraps -> total_increasing sees a new cycle; diagnostic (strikes: lightning_today)
    { "lightning_count",    "Lightning Counter",  false, U_NONE, nullptr,           "total_increasing", "lightningCount",  nullptr,    true  },
    { "lightning_distance", "Lightning Distance", false, U_KM,   "distance",        "measurement",      "lightningDistKm", nullptr,    false },
    { "supercap",           "Supercap Voltage",   false, U_V,    "voltage",         "measurement",      "supercapV",       nullptr,    true  },
    { "battery",            "Battery",            true,  U_NONE, "battery",         nullptr,            "batteryOk",       nullptr,    true  },
    { "rssi",               "Signal Strength",    false, U_DBM,  "signal_strength", "measurement",      "rssi",            nullptr,    true  },
    { "age",                "Last Packet Age",    false, U_S,    "duration",        "measurement",      "ageSec",          nullptr,    true  },
    // Fine Offset air quality, leak, leaf wetness, EC, pressure sensors
    { "pressure",           "Pressure",           false, U_PRESS, "atmospheric_pressure", "measurement", "pressure",        "pressure", false },
    { "pm25",               "PM2.5",              false, U_UGM3, "pm25",            "measurement",      "pm25",            nullptr,    false },
    { "pm10",               "PM10",               false, U_UGM3, "pm10",            "measurement",      "pm10",            nullptr,    false },
    { "pm1",                "PM1",                false, U_UGM3, "pm1",             "measurement",      "pm1",             nullptr,    false },
    { "pm4",                "PM4",                false, U_UGM3, nullptr,           "measurement",      "pm4",             nullptr,    false },
    { "co2",                "CO2",                false, U_PPM,  "carbon_dioxide",  "measurement",      "co2Ppm",          nullptr,    false },
    { "leak",               "Leak",               true,  U_NONE, "moisture",        nullptr,            "leak",            nullptr,    false },
    { "leaf_wetness",       "Leaf Wetness",       false, U_PCT,  "moisture",        "measurement",      "leafWetnessPct",  nullptr,    false },
    { "conductivity",       "Soil Conductivity",  false, U_USCM, "conductivity",    "measurement",      "conductivityUsCm", nullptr,   false },
    // rf_stats.h: from the counters' increments, reset at local midnight (a decrease = new cycle)
    { "rain_today",         "Rain Today",         false, U_RAIN, "precipitation",   "total_increasing", "rainToday",       nullptr,    false },
    { "lightning_today",    "Lightning Strikes Today", false, U_NONE, nullptr,      "total_increasing", "lightningToday",  nullptr,    false },
    { "last_lightning",     "Last Lightning",     false, U_NONE, "timestamp",       nullptr,            "lastLightning",   nullptr,    false },
};
enum : int { E_TEMP, E_HUM, E_WSPD, E_WGUST, E_WDIR, E_RAIN, E_UV, E_LIGHT, E_SOLAR, E_SOIL, E_LCOUNT, E_LDIST,
             E_SUPERCAP, E_BATTERY, E_RSSI, E_AGE, E_PRESS, E_PM25, E_PM10, E_PM1, E_PM4, E_CO2, E_LEAK, E_LEAF, E_EC,
             E_RAINDAY, E_LTODAY, E_LLAST };
static_assert(E_LLAST + 1 == HA_RF_ENTITY_COUNT && HA_RF_ENTITY_COUNT <= 32, "one mask bit per entity");

const char* unitText(Unit u)
{
    switch (u) {
        case U_TEMP: return gCfg.unitSystem == 1 ? "\xC2\xB0" "F" : "\xC2\xB0" "C";
        case U_WIND: return windSpeedUnitLabel(gCfg);
        case U_RAIN: return rainUnitLabel(gCfg);
        case U_PCT:  return "%";
        case U_DEG:  return "\xC2\xB0";
        case U_LX:   return "lx";
        case U_WM2:  return "W/m\xC2\xB2";
        case U_V:    return "V";
        case U_KM:   return "km";
        case U_DBM:  return "dBm";
        case U_S:    return "s";
        case U_UV:   return "index";   // like the node's own UV entity
        case U_PRESS: return pressureUnitLabel(gCfg);
        case U_UGM3: return "\xC2\xB5g/m\xC2\xB3";
        case U_PPM:  return "ppm";
        case U_USCM: return "\xC2\xB5S/cm";
        default:     return nullptr;
    }
}

// Device name: "<model> <ID>", or a description of the Bresser / IT+ type.
void deviceName(char* out, size_t len, const RfSensorReading& r)
{
    const char* what = r.model;
    if (!what) {
        if (r.proto == RF_PROTO_ITPLUS17 || r.proto == RF_PROTO_ITPLUS9) what = "IT+ thermo-hygro";
        else switch (r.type) {
            case 0: case 1: case 12: case 13: what = "Bresser weather station"; break;
            case 2:  what = "Bresser thermo-hygro"; break;
            case 3:  what = "Bresser pool thermometer"; break;
            case 4:  what = "Bresser soil sensor"; break;
            case 5:  what = "Bresser leak sensor"; break;
            case 9:  what = "Bresser rain/lightning"; break;
            default: what = "Bresser sensor"; break;
        }
    }
    const char* nm = rfNameOf(r.id);
    if (nm[0]) snprintf(out, len, "%s (%s)", nm, what);   // user-defined name (rf_names.h)
    else       snprintf(out, len, "%s %08lX", what, (unsigned long)r.id);
}

const char* manufacturer(const RfSensorReading& r)
{
    switch (r.proto) {
        case RF_PROTO_FINEOFFSET: return "Fine Offset (Ecowitt / Froggit)";
        case RF_PROTO_ITPLUS17:
        case RF_PROTO_ITPLUS9:    return "LaCrosse / TFA (IT+)";
        case RF_PROTO_DAVIS:      return "Davis";
        default:                  return "Bresser";
    }
}

void configTopic(char* out, size_t len, const char* clientId, uint32_t id, int n)
{
    snprintf(out, len, "%s/%s/%s/rf_%08lX_%s/config", HA_DISCOVERY_PREFIX, haRfEntityComponent(n), clientId,
             (unsigned long)id, ENTITIES[n].key);
}

bool publishDoc(const HaRfOut& out, const char* topic, JsonDocument& doc, bool retain)
{
    if (doc.overflowed()) return false;
    size_t n = measureJson(doc);
    std::unique_ptr<char[]> buf(new (std::nothrow) char[n + 1]);
    if (!buf || serializeJson(doc, buf.get(), n + 1) != n) return false;
    return out.publish(topic, buf.get(), n, retain, out.ctx);
}

bool publishConfig(const HaRfOut& out, const RfSensorReading& r, int n)
{
    const EntityDef& e = ENTITIES[n];
    char topic[160], uid[96], state[112], tmpl[160], dev[64], devId[64];
    configTopic(topic, sizeof(topic), out.clientId, r.id, n);
    snprintf(uid, sizeof(uid), "%s_rf_%08lX_%s", out.clientId, (unsigned long)r.id, e.key);
    snprintf(devId, sizeof(devId), "%s_rf_%08lX", out.clientId, (unsigned long)r.id);
    haRfStateTopic(state, sizeof(state), out.prefix, r.id);
    if (n == E_BATTERY)    // battery: on = low
        snprintf(tmpl, sizeof(tmpl), "{{ 'OFF' if value_json.%s else 'ON' }}", e.field);
    else if (e.binary)     // leak: on = water detected
        snprintf(tmpl, sizeof(tmpl), "{{ 'ON' if value_json.%s else 'OFF' }}", e.field);
    else if (e.staleGroup) // a value not received for > 5 min reads as unknown
        snprintf(tmpl, sizeof(tmpl), "{{ None if '%s' in (value_json.stale | default([])) else value_json.%s | default(None) }}",
                 e.staleGroup, e.field);
    else
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.%s | default(None) }}", e.field);

    JsonDocument doc;
    doc["name"]           = e.name;
    doc["unique_id"]      = uid;
    doc["state_topic"]    = state;
    doc["value_template"] = tmpl;
    JsonArray avail = doc["availability"].to<JsonArray>();
    JsonObject node = avail.add<JsonObject>();
    node["topic"] = out.availTopic;
    node["payload_available"] = "online";
    node["payload_not_available"] = "offline";
    if (!e.diagnostic) {
        // the sensor itself: offline once not heard for its freshness window (per family, rf_sensors.h)
        char at[96];
        snprintf(at, sizeof(at), "{{ 'online' if (value_json.ageSec | int(999999)) <= %lu else 'offline' }}",
                 rfFreshMs(r) / 1000);
        JsonObject self = avail.add<JsonObject>();
        self["topic"] = state;
        self["value_template"] = at;
        doc["availability_mode"] = "all";
    }
    const char* unit = unitText(e.unit);
    if (unit)          doc["unit_of_measurement"] = unit;
    if (e.deviceClass) doc["device_class"] = e.deviceClass;
    if (e.stateClass)  doc["state_class"]  = e.stateClass;
    if (e.binary)      { doc["payload_on"] = "ON"; doc["payload_off"] = "OFF"; }
    if (e.diagnostic)  doc["entity_category"] = "diagnostic";

    JsonObject d = doc["device"].to<JsonObject>();
    d["identifiers"].to<JsonArray>().add(devId);
    deviceName(dev, sizeof(dev), r);
    d["name"]         = dev;
    d["manufacturer"] = manufacturer(r);
    d["model"]        = r.model ? r.model : rfProtoName(r.proto);
    d["via_device"]   = out.clientId;
    d["sw_version"]   = FIRMWARE_VERSION;   // the node's firmware decodes it
    return publishDoc(out, topic, doc, true);
}

// Announced IDs (persisted) and what was published in this connection.
struct Published { uint32_t id; uint32_t mask; };
std::vector<uint32_t>  announced;
bool                   announcedLoaded = false;
std::vector<Published> published;

bool contains(const std::vector<uint32_t>& v, uint32_t id)
{
    for (uint32_t x : v) if (x == id) return true;
    return false;
}

void loadAnnounced()
{
    if (announcedLoaded) return;
    char buf[HA_RF_IDS_LEN] = "";
    loadHaRfIds(buf, sizeof(buf));
    announced.clear();
    for (const char* p = buf; *p;) {
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) { p++; continue; }
        if (v && !contains(announced, (uint32_t)v)) announced.push_back((uint32_t)v);
        p = end;
    }
    announcedLoaded = true;
}

void saveAnnounced()
{
    char buf[HA_RF_IDS_LEN] = "";
    size_t n = 0;
    for (uint32_t id : announced) {
        if (n + 10 > sizeof(buf)) break;
        n += snprintf(buf + n, sizeof(buf) - n, "%s%08lX", n ? "," : "", (unsigned long)id);
    }
    saveHaRfIds(buf);
}

Published& publishedFor(uint32_t id)
{
    for (Published& p : published) if (p.id == id) return p;
    published.push_back(Published{ id, 0 });
    return published.back();
}

} // namespace

uint32_t haRfEntityMask(int idx)
{
    if (idx < 0 || idx >= RF_MAX_SENSORS || !gRfSensors[idx].used) return 0;
    const RfSensorReading& r = gRfSensors[idx];
    uint32_t m = (1UL << E_BATTERY) | (1UL << E_RSSI) | (1UL << E_AGE);
    if (r.hasTemp)      m |= 1UL << E_TEMP;
    if (r.hasHumidity)  m |= 1UL << E_HUM;
    if (r.hasWind)      m |= (1UL << E_WSPD) | (1UL << E_WGUST) | (1UL << E_WDIR);
    if (r.hasRain)      m |= (1UL << E_RAIN) | (1UL << E_RAINDAY);
    if (r.hasUv)        m |= 1UL << E_UV;
    if (r.hasLight)     m |= 1UL << E_LIGHT;
    if (r.hasSolar)     m |= 1UL << E_SOLAR;
    if (r.hasMoisture)  m |= 1UL << E_SOIL;
    if (r.hasLightning) m |= (1UL << E_LCOUNT) | (1UL << E_LDIST) | (1UL << E_LTODAY) | (1UL << E_LLAST);
    if (r.hasSupercap)  m |= 1UL << E_SUPERCAP;
    if (r.hasPressure)  m |= 1UL << E_PRESS;
    if (r.hasPm25)      m |= 1UL << E_PM25;
    if (r.hasPm10)      m |= 1UL << E_PM10;
    if (r.hasPm1)       m |= 1UL << E_PM1;
    if (r.hasPm4)       m |= 1UL << E_PM4;
    if (r.hasCo2)       m |= 1UL << E_CO2;
    if (r.hasLeak)      m |= 1UL << E_LEAK;
    if (r.hasLeafWetness)  m |= 1UL << E_LEAF;
    if (r.hasConductivity) m |= 1UL << E_EC;
    return m;
}

const char* haRfEntityKey(int n)       { return n >= 0 && n < HA_RF_ENTITY_COUNT ? ENTITIES[n].key : nullptr; }
const char* haRfEntityComponent(int n)
{
    return n >= 0 && n < HA_RF_ENTITY_COUNT ? (ENTITIES[n].binary ? "binary_sensor" : "sensor") : nullptr;
}

void haRfStateTopic(char* out, size_t len, const char* prefix, uint32_t id)
{
    snprintf(out, len, "%s/rf/%08lX", prefix, (unsigned long)id);
}

void haRfConnected()
{
    published.clear();
    announcedLoaded = false;   // read again from NVS (one read per connection)
}

int haRfPoll(const HaRfOut& out, int budget)
{
    loadAnnounced();
    int sent = 0;
    const std::vector<uint32_t> ids = rfParseIds(gCfg.rfIds);

    // 1. IDs no longer listed: clear every possible config (an empty retained
    // payload deletes the entity; for one never announced it is a no-op).
    bool changed = false;
    for (size_t a = 0; a < announced.size();) {
        uint32_t id = announced[a];
        if (contains(ids, id)) { a++; continue; }
        bool ok = true;
        for (int n = 0; n < HA_RF_ENTITY_COUNT; n++) {
            char topic[160];
            configTopic(topic, sizeof(topic), out.clientId, id, n);
            if (out.publish(topic, "", 0, true, out.ctx)) sent++;
            else ok = false;
        }
        if (!ok) { a++; continue; }   // try again on the next call
        announced.erase(announced.begin() + a);
        for (size_t k = 0; k < published.size(); k++)
            if (published[k].id == id) { published.erase(published.begin() + k); break; }
        changed = true;
    }
    if (changed) saveAnnounced();

    // 2. Listed and received sensors: announce the entities they have values for.
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (!r.used || !contains(ids, r.id)) continue;
        uint32_t mask = haRfEntityMask(i);
        Published& p = publishedFor(r.id);
        uint32_t todo = mask & ~p.mask;
        if (!todo) continue;
        if (!contains(announced, r.id)) {   // stored before the first config goes out
            announced.push_back(r.id);
            saveAnnounced();
        }
        for (int n = 0; n < HA_RF_ENTITY_COUNT; n++) {
            if (!(todo & (1UL << n))) continue;
            if (budget <= 0) return sent;
            budget--;
            if (publishConfig(out, r, n)) { p.mask |= 1UL << n; sent++; }
        }
    }
    return sent;
}

int haRfPublishStates(const HaRfOut& out)
{
    int sent = 0;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        if (!gRfSensors[i].used) continue;
        char topic[112];
        haRfStateTopic(topic, sizeof(topic), out.prefix, gRfSensors[i].id);
        JsonDocument doc;
        addRfSensorJson(doc.to<JsonObject>(), i);
        if (publishDoc(out, topic, doc, false)) sent++;
    }
    return sent;
}

void haRfDeviceNameForTest(char* out, size_t len, const RfSensorReading& r) { deviceName(out, len, r); }
