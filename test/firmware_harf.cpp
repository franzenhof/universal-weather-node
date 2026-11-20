// Home Assistant devices per wireless sensor (src/ha_rf.cpp): only listed
// IDs, entities only for values the sensor sent, per-sensor state topic,
// removal of an ID clears its retained configs (also after a restart).
#include "ha_rf.h"
#include "json_payload.h"
#include "globals.h"
#include "rf_sensors.h"
#include "sources.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// NVS stand-in for the announced IDs (config.cpp on the device)
static std::string hostHaRfIds;
static int hostHaRfSaves = 0;
void loadHaRfIds(char* out, size_t len) { snprintf(out, len, "%s", hostHaRfIds.c_str()); }
void saveHaRfIds(const char* ids) { hostHaRfIds = ids; hostHaRfSaves++; }

namespace {

struct Msg { std::string topic, payload; bool retain; };
std::vector<Msg> sent;
std::map<std::string, std::string> retained;   // what the broker keeps
bool failPublish = false;

bool capture(const char* topic, const char* payload, size_t len, bool retain, void*)
{
    if (failPublish) return false;
    sent.push_back(Msg{ topic, std::string(payload, len), retain });
    if (retain) {
        if (len) retained[topic] = std::string(payload, len);
        else     retained.erase(topic);
    }
    return true;
}

const HaRfOut OUT{ "garden-node", "garden", "garden/status", capture, nullptr };

std::string cfgTopic(const char* comp, const char* id, const char* key)
{
    return std::string("homeassistant/") + comp + "/garden-node/rf_" + id + "_" + key + "/config";
}

int configsOf(const char* id)
{
    int n = 0;
    std::string mark = std::string("/garden-node/rf_") + id + "_";
    for (const auto& kv : retained) if (kv.first.find(mark) != std::string::npos) n++;
    return n;
}

JsonDocument parse(const std::string& s)
{
    JsonDocument d;
    assert(!deserializeJson(d, s));
    return d;
}

RfSensorReading& add(int i, uint32_t id, uint8_t proto, const char* model, uint8_t type)
{
    RfSensorReading& r = gRfSensors[i];
    r = RfSensorReading();
    r.used = true; r.id = id; r.proto = proto; r.model = model; r.type = type;
    r.rssi = -77.0f; r.lastMs = r.tempMs = r.humMs = r.windMs = r.rainMs = r.uvMs = r.lightMs = millis();
    return r;
}

int pollAll()
{
    int n = 0;
    for (int k = 0; k < 100; k++) {
        int s = haRfPoll(OUT, 4);
        if (!s) break;
        n += s;
    }
    return n;
}

} // namespace

int main()
{
    gCfg.rfEn = 1;
    gCfg.unitSystem = 0;
    gCfg.windUnitMs = 0;   // km/h
    gCfg.rfIds[0] = '\0';
    rfSensorsClear();

    RfSensorReading& ws = add(0, 0x9000342B, RF_PROTO_FINEOFFSET, "WS90", 1);
    ws.hasTemp = ws.hasHumidity = ws.hasWind = ws.hasRain = ws.hasUv = ws.hasLight = ws.hasSupercap = true;
    ws.tempC = 12.5f; ws.humidity = 70; ws.windAvgMs = 2.0f; ws.windGustMs = 4.0f; ws.windDirDeg = 225.0f;
    ws.rainTotalMm = 12.3f; ws.uvIndex = 2.0f; ws.lightLux = 30000.0f; ws.supercapV = 3.9f;
    RfSensorReading& probe = add(1, 0x3400A1B2, RF_PROTO_FINEOFFSET, "WN34", 3);
    probe.hasTemp = true; probe.tempC = 21.5f;
    RfSensorReading& neighbour = add(2, 0x30000301, RF_PROTO_FINEOFFSET, "WH31", 2);
    neighbour.hasTemp = neighbour.hasHumidity = true; neighbour.channel = 3;

    // ---- entity masks: only what the sensor sends (+ battery, RSSI, age) ----
    auto has = [](uint32_t mask, const char* key) -> bool {
        for (int n = 0; n < HA_RF_ENTITY_COUNT; n++)
            if (!strcmp(haRfEntityKey(n), key)) return (mask >> n) & 1;
        assert(false);
        return false;
    };
    uint32_t m = haRfEntityMask(1);
    assert(has(m, "temp") && !has(m, "humidity") && !has(m, "wind_speed") && has(m, "battery") && has(m, "rssi") && has(m, "age"));
    m = haRfEntityMask(0);
    assert(has(m, "wind_dir") && has(m, "rain_total") && has(m, "supercap") && !has(m, "soil_moisture") && !has(m, "lightning_count"));
    assert(haRfEntityMask(5) == 0 && haRfEntityMask(-1) == 0 && haRfEntityMask(RF_MAX_SENSORS) == 0);
    assert(!strcmp(haRfEntityComponent(13), "binary_sensor") && haRfEntityKey(HA_RF_ENTITY_COUNT) == nullptr);

    // ---- empty ID list: no per-sensor device, but every sensor has its state topic ----
    haRfConnected();
    assert(haRfPoll(OUT, 100) == 0 && retained.empty() && hostHaRfSaves == 0);
    assert(haRfPublishStates(OUT) == 3);
    assert(sent.size() == 3 && sent[0].topic == "garden/rf/9000342B" && !sent[0].retain);
    {
        JsonDocument d = parse(sent[1].payload);
        assert(d["id"] == "3400A1B2" && d["slot"] == 2 && d["model"] == "WN34" && d["temp"].as<float>() == 21.5f);
        JsonDocument w = parse(sent[0].payload);
        assert(w["windSpeed"].as<float>() > 7.19f && w["windSpeed"].as<float>() < 7.21f);   // km/h, like rfSensors[]
    }
    sent.clear();

    // ---- listed: WS90 and WN34 get devices, the neighbour's WH31 does not ----
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "9000342B, 3400A1B2");
    assert(haRfPoll(OUT, 4) == 4);                              // budget: 4 configs per call
    assert(hostHaRfIds == "9000342B");                          // stored before its first config
    pollAll();
    assert(configsOf("9000342B") == 12 && configsOf("3400A1B2") == 4 && configsOf("30000301") == 0);
    assert(hostHaRfIds == "9000342B,3400A1B2");
    {
        JsonDocument c = parse(retained.at(cfgTopic("sensor", "9000342B", "temp")));
        assert(c["unique_id"] == "garden-node_rf_9000342B_temp" && c["state_topic"] == "garden/rf/9000342B");
        assert(c["unit_of_measurement"] == "\xC2\xB0" "C" && c["device_class"] == "temperature" && c["state_class"] == "measurement");
        assert(std::string(c["value_template"].as<const char*>()).find("'temp' in (value_json.stale") != std::string::npos);
        assert(c["availability_mode"] == "all" && c["availability"].size() == 2);
        assert(c["availability"][0]["topic"] == "garden/status" && c["availability"][1]["topic"] == "garden/rf/9000342B");
        assert(std::string(c["availability"][1]["value_template"].as<const char*>()).find("<= 300") != std::string::npos);
        JsonObject dev = c["device"];
        assert(dev["identifiers"][0] == "garden-node_rf_9000342B" && dev["via_device"] == "garden-node");
        assert(dev["name"] == "WS90 9000342B" && dev["model"] == "WS90");
        assert(c["entity_category"].isNull());

        JsonDocument r = parse(retained.at(cfgTopic("sensor", "9000342B", "rain_total")));
        assert(r["state_class"] == "total_increasing" && r["unit_of_measurement"] == "mm" && r["device_class"] == "precipitation");
        JsonDocument w = parse(retained.at(cfgTopic("sensor", "9000342B", "wind_gust")));
        assert(w["unit_of_measurement"] == "km/h" && w["device_class"] == "wind_speed");
        JsonDocument b = parse(retained.at(cfgTopic("binary_sensor", "3400A1B2", "battery")));
        assert(b["device_class"] == "battery" && b["entity_category"] == "diagnostic" && b["availability"].size() == 1);
        assert(b["value_template"] == "{{ 'OFF' if value_json.batteryOk else 'ON' }}");
        JsonDocument a = parse(retained.at(cfgTopic("sensor", "3400A1B2", "age")));
        assert(a["unit_of_measurement"] == "s" && a["entity_category"] == "diagnostic");
        JsonDocument t = parse(retained.at(cfgTopic("sensor", "3400A1B2", "temp")));
        assert(t["device"]["name"] == "WN34 3400A1B2");
    }
    // nothing new: nothing sent; a newly reported value adds just its entity
    sent.clear();
    assert(haRfPoll(OUT, 100) == 0 && sent.empty());
    probe.hasHumidity = true;
    assert(haRfPoll(OUT, 100) == 1 && sent.size() == 1 && sent[0].topic == cfgTopic("sensor", "3400A1B2", "humidity"));
    // a failed publish is retried on the next call
    probe.hasMoisture = true;
    failPublish = true;
    assert(haRfPoll(OUT, 100) == 0);
    failPublish = false;
    assert(haRfPoll(OUT, 100) == 1 && configsOf("3400A1B2") == 6);
    // a reconnect announces everything again (retained anyway; HA may have restarted)
    sent.clear();
    haRfConnected();
    assert(pollAll() == 12 + 6 && sent.size() == 18);
    int saves = hostHaRfSaves;

    // ---- ID removed from the list: its retained configs are cleared ----
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "9000342B");
    sent.clear();
    assert(haRfPoll(OUT, 4) == HA_RF_ENTITY_COUNT);             // every possible entity, not limited by the budget
    for (const Msg& x : sent) assert(x.retain && x.payload.empty() && x.topic.find("rf_3400A1B2_") != std::string::npos);
    assert(configsOf("3400A1B2") == 0 && configsOf("9000342B") == 12);
    assert(hostHaRfIds == "9000342B" && hostHaRfSaves == saves + 1);
    assert(haRfPoll(OUT, 100) == 0);                            // once

    // ---- removed while the node was off (stored list knows more than the
    // setting): cleared after the next connection ----
    hostHaRfIds = "9000342B,3400A1B2,30000301";
    haRfConnected();
    sent.clear();
    pollAll();
    int cleared = 0;
    for (const Msg& x : sent) if (x.payload.empty()) cleared++;
    assert(cleared == 2 * HA_RF_ENTITY_COUNT && hostHaRfIds == "9000342B");

    // ---- imperial units, Bresser / IT+ device names ----
    gCfg.unitSystem = 1;
    RfSensorReading& br = add(3, 0x39582376, RF_PROTO_BRESSER, nullptr, 1);
    br.hasWind = br.hasRain = true;
    RfSensorReading& it = add(4, 0x1700002C, RF_PROTO_ITPLUS17, nullptr, 2);
    it.hasTemp = true;
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "39582376,1700002C");
    pollAll();
    {
        JsonDocument w = parse(retained.at(cfgTopic("sensor", "39582376", "wind_speed")));
        assert(w["unit_of_measurement"] == "mph" && w["device"]["name"] == "Bresser weather station 39582376");
        assert(w["device"]["model"] == "Bresser" && w["device"]["manufacturer"] == "Bresser");
        JsonDocument r = parse(retained.at(cfgTopic("sensor", "39582376", "rain_total")));
        assert(r["unit_of_measurement"] == "in");
        JsonDocument t = parse(retained.at(cfgTopic("sensor", "1700002C", "temp")));
        assert(t["unit_of_measurement"] == "\xC2\xB0" "F" && t["device"]["name"] == "IT+ thermo-hygro 1700002C");
    }
    assert(configsOf("9000342B") == 0 && hostHaRfIds == "39582376,1700002C");

    // ---- Fine Offset air quality, leak, pressure, leaf wetness, EC entities ----
    {
        RfSensorReading& aq = add(5, 0x45003660, RF_PROTO_FINEOFFSET, "WH46", 8);
        aq.hasTemp = aq.hasHumidity = aq.hasPm25 = aq.hasPm10 = aq.hasPm1 = aq.hasPm4 = aq.hasCo2 = true;
        aq.pm25 = 5.0f; aq.pm10 = 5.7f; aq.pm1 = 4.2f; aq.pm4 = 5.4f; aq.co2Ppm = 779;
        RfSensorReading& lk = add(6, 0x5530CF55, RF_PROTO_FINEOFFSET, "WH55", 5);
        lk.hasLeak = lk.leak = true; lk.leakRaw = 267; lk.channel = 4;
        RfSensorReading& th = add(7, 0x25000050, RF_PROTO_FINEOFFSET, "WN32P", 2);
        th.hasTemp = th.hasHumidity = th.hasPressure = true; th.pressureHpa = 1001.7f;
        RfSensorReading& so = add(8, 0xA2005D8D, RF_PROTO_FINEOFFSET, "WH52", 4);
        so.hasTemp = so.hasMoisture = so.hasConductivity = true; so.conductivityUsCm = 643.4f;
        RfSensorReading& lw = add(9, 0x3500305E, RF_PROTO_FINEOFFSET, "WN35", 4);
        lw.hasLeafWetness = true; lw.leafWetnessPct = 7;
        uint32_t mk = haRfEntityMask(5);
        assert(has(mk, "pm25") && has(mk, "pm10") && has(mk, "pm1") && has(mk, "pm4") && has(mk, "co2") && !has(mk, "leak"));
        snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "45003660,5530CF55,25000050,A2005D8D,3500305E");
        pollAll();
        assert(configsOf("45003660") == 3 + 2 + 5 && configsOf("5530CF55") == 3 + 1 && configsOf("25000050") == 3 + 3);
        assert(configsOf("A2005D8D") == 3 + 3 && configsOf("3500305E") == 3 + 1);
        JsonDocument pm = parse(retained.at(cfgTopic("sensor", "45003660", "pm25")));
        assert(pm["device_class"] == "pm25" && pm["unit_of_measurement"] == "\xC2\xB5g/m\xC2\xB3" && pm["state_class"] == "measurement");
        assert(pm["value_template"] == "{{ value_json.pm25 | default(None) }}" && pm["availability"].size() == 2);
        assert(parse(retained.at(cfgTopic("sensor", "45003660", "pm10")))["device_class"] == "pm10");
        assert(parse(retained.at(cfgTopic("sensor", "45003660", "pm1")))["device_class"] == "pm1");
        assert(parse(retained.at(cfgTopic("sensor", "45003660", "pm4")))["device_class"].isNull());
        JsonDocument co = parse(retained.at(cfgTopic("sensor", "45003660", "co2")));
        assert(co["device_class"] == "carbon_dioxide" && co["unit_of_measurement"] == "ppm");
        JsonDocument l = parse(retained.at(cfgTopic("binary_sensor", "5530CF55", "leak")));
        assert(l["device_class"] == "moisture" && l["value_template"] == "{{ 'ON' if value_json.leak else 'OFF' }}");
        assert(l["entity_category"].isNull() && l["availability"].size() == 2 && l["device"]["name"] == "WH55 5530CF55");
        JsonDocument p = parse(retained.at(cfgTopic("sensor", "25000050", "pressure")));
        assert(p["device_class"] == "atmospheric_pressure" && p["unit_of_measurement"] == "inHg");   // imperial here
        JsonDocument ec = parse(retained.at(cfgTopic("sensor", "A2005D8D", "conductivity")));
        assert(ec["device_class"] == "conductivity" && ec["unit_of_measurement"] == "\xC2\xB5S/cm");
        JsonDocument w = parse(retained.at(cfgTopic("sensor", "3500305E", "leaf_wetness")));
        assert(w["unit_of_measurement"] == "%" && w["device_class"] == "moisture");
        // state topic: the rfSensors[] object with the new keys
        sent.clear();
        haRfPublishStates(OUT);
        for (const Msg& x : sent) {
            if (x.topic == "garden/rf/5530CF55") {
                JsonDocument d = parse(x.payload);
                assert(d["leak"] == true && d["leakRaw"] == 267 && d["leakHighSensitivity"] == false && d["channel"] == 4);
            } else if (x.topic == "garden/rf/25000050") {
                JsonDocument d = parse(x.payload);
                assert(fabs(d["pressure"].as<float>() - 29.58f) < 0.006f);   // 1001.7 hPa in inHg, 2 decimals
            }
        }
        snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "39582376,1700002C");
        pollAll();
        assert(configsOf("45003660") == 0 && configsOf("5530CF55") == 0 && configsOf("A2005D8D") == 0);   // all cleared
    }

    // ---- empty list again: no per-sensor device left ----
    gCfg.rfIds[0] = '\0';
    haRfPoll(OUT, 100);
    assert(retained.empty() && hostHaRfIds.empty());

    // ---- a stored list of 16 IDs (written by the 16-sensor build) loads in full ----
    std::string old16;
    for (int k = 0; k < 16; k++) {
        char e[16];
        snprintf(e, sizeof(e), "%s%08X", k ? "," : "", 0x40000000u + k);
        old16 += e;
    }
    hostHaRfIds = old16;
    haRfConnected();
    sent.clear();
    haRfPoll(OUT, 0);
    assert((int)sent.size() == 16 * HA_RF_ENTITY_COUNT && hostHaRfIds.empty());

    // ---- 32 listed sensors: 32 devices, all 32 IDs stored ----
    rfSensorsClear();
    std::string list;
    for (int k = 0; k < RF_MAX_SENSORS; k++) {
        RfSensorReading& r = add(k, 0x30000000u + k, RF_PROTO_FINEOFFSET, "WH31", 2);
        r.hasTemp = r.hasHumidity = true;
        char e[16];
        snprintf(e, sizeof(e), "%s%08X", k ? ", " : "", 0x30000000u + k);
        list += e;
    }
    assert(list.size() < sizeof(gCfg.rfIds));
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "%s", list.c_str());
    haRfConnected();
    sent.clear();
    assert(pollAll() == RF_MAX_SENSORS * 5);                     // temp, humidity, battery, rssi, age
    assert(configsOf("30000000") == 5 && configsOf("3000001F") == 5);
    assert(hostHaRfIds.size() == 32 * 9 - 1 && hostHaRfIds.size() < HA_RF_IDS_LEN);
    assert(haRfPublishStates(OUT) == RF_MAX_SENSORS);
    {
        JsonDocument d = parse(sent.back().payload);
        assert(sent.back().topic == "garden/rf/3000001F" && d["slot"] == 32);
    }
    gCfg.rfIds[0] = '\0';
    haRfPoll(OUT, 0);
    assert(retained.empty() && hostHaRfIds.empty());
    return 0;
}
