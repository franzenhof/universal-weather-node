// Home Assistant devices per wireless sensor (src/ha_rf.cpp): only ★ sensors
// (logical sensors), entities only for values the sensor sent, unique_id /
// device on the internal ID (stable across a key change and a re-bind),
// state topic on the key, un-★ clears the retained configs (also after a
// restart), configs of a build before A5c (by radio ID) are cleared once.
#include "ha_rf.h"
#include "ls_host.h"
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

extern int hostLsLockDepth;

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

std::string cfgTopic(const char* comp, const std::string& id, const char* key)
{
    return std::string("homeassistant/") + comp + "/garden-node/rf_" + id + "_" + key + "/config";
}

// the internal ID of the ★ sensor bound to radio ID id
std::string internalIdOf(uint32_t id)
{
    const int i = lsFindByRadioId(gLogicalSensors, id);
    assert(i >= 0);
    return gLogicalSensors.s[i].internalId;
}

void setKey(uint32_t id, const char* key)
{
    assert(lsSetCustomId(gLogicalSensors, lsFindByRadioId(gLogicalSensors, id), key));
}

void unStar(uint32_t id)
{
    const int i = lsFindByRadioId(gLogicalSensors, id);
    assert(i >= 0);
    lsRemove(gLogicalSensors, i);
}

int configsOf(const std::string& id)
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
    gLogicalSensors = LogicalSensorTable();
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
    assert(has(m, "temp") && !has(m, "humidity") && !has(m, "wind_speed") && has(m, "battery") && has(m, "rssi") && has(m, "age") && has(m, "reception"));
    m = haRfEntityMask(0);
    assert(has(m, "wind_dir") && has(m, "rain_total") && has(m, "rain_today") && has(m, "supercap") && !has(m, "soil_moisture") && !has(m, "lightning_count"));
    assert(haRfEntityMask(5) == 0 && haRfEntityMask(-1) == 0 && haRfEntityMask(RF_MAX_SENSORS) == 0);
    {   // a network sensor has no reception to watch, no signal strength and no battery report
        const uint8_t proto = neighbour.proto;
        neighbour.proto = RF_PROTO_MQTT;
        assert(!has(haRfEntityMask(2), "reception") && has(haRfEntityMask(2), "age"));
        assert(!has(haRfEntityMask(2), "rssi") && !has(haRfEntityMask(2), "battery") && has(haRfEntityMask(2), "temp"));
        neighbour.proto = RF_PROTO_REST;
        assert(!has(haRfEntityMask(2), "rssi") && !has(haRfEntityMask(2), "battery"));
        neighbour.proto = proto;
    }
    assert(!strcmp(haRfEntityComponent(13), "binary_sensor") && haRfEntityKey(HA_RF_ENTITY_COUNT) == nullptr);

    // ---- no ★ sensor: no per-sensor device, but every sensor has its state topic (radio ID) ----
    haRfConnected();
    assert(haRfPoll(OUT, 100) == 0 && retained.empty() && hostHaRfSaves == 0);
    assert(haRfPublishStates(OUT) == 3);
    assert(sent.size() == 3 && sent[0].topic == "garden/rf/rf9000342B" && !sent[0].retain);
    {
        JsonDocument d = parse(sent[1].payload);
        assert(d["id"] == "3400A1B2" && d["radioId"] == "3400A1B2" && d["slot"] == 2 && d["model"] == "WN34" && d["temp"].as<float>() == 21.5f);
        assert(d["customId"].isNull() && d["internalId"].isNull());
        JsonDocument w = parse(sent[0].payload);
        assert(w["windSpeed"].as<float>() > 7.19f && w["windSpeed"].as<float>() < 7.21f);   // km/h, like rfSensors[]
    }
    sent.clear();

    // ---- ★: WS90 and WN34 get devices, the neighbour's WH31 does not ----
    hostSetMine("9000342B, 3400A1B2");
    setKey(0x9000342B, "wetter");
    setKey(0x3400A1B2, "pool");
    const std::string wsInternalId = internalIdOf(0x9000342B), prInternalId = internalIdOf(0x3400A1B2);
    assert(wsInternalId != "wetter" && prInternalId != "pool" && wsInternalId != prInternalId);
    assert(haRfPoll(OUT, 4) == 4);                              // budget: 4 configs per call
    assert(hostHaRfIds == wsInternalId);                               // stored before its first config
    pollAll();
    assert(configsOf(wsInternalId) == 14 && configsOf(prInternalId) == 5 && configsOf("30000301") == 0);
    assert(configsOf("9000342B") == 0 && configsOf("wetter") == 0);   // neither radio ID nor key in a config topic
    assert(hostHaRfIds == wsInternalId + "," + prInternalId);
    {
        JsonDocument c = parse(retained.at(cfgTopic("sensor", wsInternalId, "temp")));
        assert(c["unique_id"] == "garden-node_rf_" + wsInternalId + "_temp" && c["state_topic"] == "garden/rf/wetter");
        assert(c["unit_of_measurement"] == "\xC2\xB0" "C" && c["device_class"] == "temperature" && c["state_class"] == "measurement");
        assert(std::string(c["value_template"].as<const char*>()).find("'temp' in (value_json.stale") != std::string::npos);
        assert(c["availability_mode"] == "all" && c["availability"].size() == 2);
        assert(c["availability"][0]["topic"] == "garden/status" && c["availability"][1]["topic"] == "garden/rf/wetter");
        assert(std::string(c["availability"][1]["value_template"].as<const char*>()).find("<= 300") != std::string::npos);
        // reception watchdog: a diagnostic percentage, unknown while the node has no rate (null in the state)
        JsonDocument rx = parse(retained.at(cfgTopic("sensor", wsInternalId, "reception")));
        assert(rx["entity_category"] == "diagnostic" && rx["unit_of_measurement"] == "%" && rx["state_class"] == "measurement");
        assert(rx["value_template"] == "{{ value_json.reception | default(None) }}" && rx["availability"].size() == 1);
        assert(rx["json_attributes_topic"] == "garden/rf/wetter");
        assert(rx["json_attributes_template"] == "{{ {'id_collision': value_json.idCollision | default(false)} | tojson }}");
        JsonObject dev = c["device"];
        assert(dev["identifiers"][0] == "garden-node_rf_" + wsInternalId && dev["via_device"] == "garden-node");
        assert(dev["name"] == "WS90 wetter" && dev["model"] == "WS90");   // no name: model + key
        assert(c["entity_category"].isNull());

        JsonDocument r = parse(retained.at(cfgTopic("sensor", wsInternalId, "rain_total")));
        assert(r["state_class"] == "total_increasing" && r["unit_of_measurement"] == "mm" && r["device_class"] == "precipitation");
        JsonDocument w = parse(retained.at(cfgTopic("sensor", wsInternalId, "wind_gust")));
        assert(w["unit_of_measurement"] == "km/h" && w["device_class"] == "wind_speed");
        JsonDocument b = parse(retained.at(cfgTopic("binary_sensor", prInternalId, "battery")));
        assert(b["device_class"] == "battery" && b["entity_category"] == "diagnostic" && b["availability"].size() == 1);
        assert(b["value_template"] == "{{ 'OFF' if value_json.batteryOk else 'ON' }}");
        JsonDocument a = parse(retained.at(cfgTopic("sensor", prInternalId, "age")));
        assert(a["unit_of_measurement"] == "s" && a["entity_category"] == "diagnostic");
        JsonDocument t = parse(retained.at(cfgTopic("sensor", prInternalId, "temp")));
        assert(t["device"]["name"] == "WN34 pool");
        // a user-defined name is the device name; an rfNames entry of the same ID is not used then
        snprintf(gCfg.rfNames, sizeof(gCfg.rfNames), "3400A1B2:Old");
        const int ls = lsFindByRadioId(gLogicalSensors, 0x3400A1B2);
        snprintf(gLogicalSensors.s[ls].displayName, sizeof(gLogicalSensors.s[ls].displayName), "Pool");
        char dn[64];
        haRfDeviceNameForTest(dn, sizeof(dn), probe);
        assert(std::string(dn) == "Pool");
        gLogicalSensors.s[ls].displayName[0] = '\0';
        gCfg.rfNames[0] = '\0';
    }
    // nothing new: nothing sent; a newly reported value adds just its entity
    sent.clear();
    assert(haRfPoll(OUT, 100) == 0 && sent.empty());
    probe.hasHumidity = true;
    assert(haRfPoll(OUT, 100) == 1 && sent.size() == 1 && sent[0].topic == cfgTopic("sensor", prInternalId, "humidity"));
    // a failed publish is retried on the next call
    probe.hasMoisture = true;
    failPublish = true;
    assert(haRfPoll(OUT, 100) == 0);
    failPublish = false;
    assert(haRfPoll(OUT, 100) == 1 && configsOf(prInternalId) == 7);
    // a reconnect announces everything again (retained anyway; HA may have restarted)
    sent.clear();
    haRfConnected();
    assert(pollAll() == 14 + 7 && sent.size() == 21);

    // ---- state topics: a ★ sensor's key, a received-only sensor's radio ID ----
    sent.clear();
    assert(haRfPublishStates(OUT) == 3);
    {
        assert(sent[0].topic == "garden/rf/wetter" && sent[1].topic == "garden/rf/pool" && sent[2].topic == "garden/rf/rf30000301");
        JsonDocument d = parse(sent[1].payload);
        assert(d["customId"] == "pool" && d["internalId"] == prInternalId && d["id"] == "3400A1B2" && d["radioId"] == "3400A1B2");
    }
    {   // collision case: a ★ sensor whose custom ID equals the radio ID of the received-only sensor - separate topics
        setKey(0x3400A1B2, "30000301");
        sent.clear();
        assert(haRfPublishStates(OUT) == 3);
        assert(sent[1].topic == "garden/rf/30000301" && sent[2].topic == "garden/rf/rf30000301" && sent[1].payload != sent[2].payload);
        setKey(0x3400A1B2, "pool");
    }

    // ---- a new key: the same entities (unique_id), the state topic follows,
    // the old state topic is cleared ----
    setKey(0x3400A1B2, "teich");
    sent.clear();
    pollAll();
    {
        int configs = 0;
        bool oldCleared = false;
        for (const Msg& x : sent) {
            if (x.topic == "garden/rf/pool") { assert(x.retain && x.payload.empty()); oldCleared = true; continue; }
            assert(x.topic.find("rf_" + prInternalId + "_") != std::string::npos && !x.payload.empty());
            configs++;
        }
        assert(oldCleared && configs == 7 && configsOf(prInternalId) == 7);   // only the WN34's, all of them, nothing deleted
        JsonDocument t = parse(retained.at(cfgTopic("sensor", prInternalId, "temp")));
        assert(t["unique_id"] == "garden-node_rf_" + prInternalId + "_temp" && t["state_topic"] == "garden/rf/teich");
        assert(t["availability"][1]["topic"] == "garden/rf/teich" && t["device"]["name"] == "WN34 teich");
        assert(t["device"]["identifiers"][0] == "garden-node_rf_" + prInternalId);
    }
    assert(haRfPoll(OUT, 100) == 0);   // once
    // ---- a new name: re-announced with the new device name, nothing else changes ----
    snprintf(gLogicalSensors.s[lsFindByRadioId(gLogicalSensors, 0x3400A1B2)].displayName, LS_DISPLAY_NAME_MAX + 1, "Teich S\xC3\xBC" "d");
    sent.clear();
    assert(pollAll() == 7 && parse(retained.at(cfgTopic("sensor", prInternalId, "temp")))["device"]["name"] == "Teich S\xC3\xBC" "d");
    for (const Msg& x : sent) assert(!x.payload.empty());

    // ---- battery change (re-bind to a new radio ID): the same device and entities ----
    RfSensorReading& probe2 = add(3, 0x3400A1C9, RF_PROTO_FINEOFFSET, "WN34", 3);
    probe2.hasTemp = true; probe2.tempC = 20.0f;
    probe.used = false;   // the old ID is gone
    assert(lsRebindRadioId(gLogicalSensors, lsFindByRadioId(gLogicalSensors, 0x3400A1B2), 0x3400A1C9));
    sent.clear();
    pollAll();
    assert(sent.size() == 5 && configsOf(prInternalId) == 7);   // temp, battery, rssi, age, reception of the new reading; nothing cleared
    for (const Msg& x : sent) assert(!x.payload.empty() && x.topic.find("rf_" + prInternalId + "_") != std::string::npos);
    {
        JsonDocument t = parse(retained.at(cfgTopic("sensor", prInternalId, "temp")));
        assert(t["unique_id"] == "garden-node_rf_" + prInternalId + "_temp" && t["state_topic"] == "garden/rf/teich");
    }
    sent.clear();
    haRfPublishStates(OUT);
    {
        bool found = false;
        for (const Msg& x : sent) if (x.topic == "garden/rf/teich") { found = true; assert(parse(x.payload)["radioId"] == "3400A1C9"); }
        assert(found);
    }
    assert(hostHaRfIds == wsInternalId + "," + prInternalId);   // unchanged
    int saves = hostHaRfSaves;

    // ---- un-★: its retained configs are cleared ----
    unStar(0x3400A1C9);
    sent.clear();
    assert(haRfPoll(OUT, 4) == HA_RF_ENTITY_COUNT);             // every possible entity, not limited by the budget
    for (const Msg& x : sent) assert(x.retain && x.payload.empty() && x.topic.find("rf_" + prInternalId + "_") != std::string::npos);
    assert(configsOf(prInternalId) == 0 && configsOf(wsInternalId) == 14);
    assert(hostHaRfIds == wsInternalId && hostHaRfSaves == saves + 1);
    assert(haRfPoll(OUT, 100) == 0);                            // once
    probe2.used = false;

    // ---- un-★ while the node was off (stored list knows more than the table): cleared after the next connection ----
    hostHaRfIds = wsInternalId + ",L0000AA," + prInternalId;
    haRfConnected();
    sent.clear();
    pollAll();
    int cleared = 0;
    for (const Msg& x : sent) if (x.payload.empty()) cleared++;
    assert(cleared == 2 * HA_RF_ENTITY_COUNT && hostHaRfIds == wsInternalId);

    // ---- configs of a build before A5c (one device per radio ID): cleared once ----
    hostHaRfIds = "9000342B,3400A1B2," + wsInternalId;
    retained[cfgTopic("sensor", "9000342B", "temp")] = "{}";
    haRfConnected();
    sent.clear();
    pollAll();
    cleared = 0;
    for (const Msg& x : sent) if (x.payload.empty()) { cleared++; assert(x.topic.find("rf_9000342B_") != std::string::npos || x.topic.find("rf_3400A1B2_") != std::string::npos); }
    assert(cleared == 2 * HA_RF_ENTITY_COUNT && configsOf("9000342B") == 0 && configsOf(wsInternalId) == 14 && hostHaRfIds == wsInternalId);

    // ---- imperial units, Bresser / IT+ device names ----
    gCfg.unitSystem = 1;
    RfSensorReading& br = add(3, 0x39582376, RF_PROTO_BRESSER, nullptr, 1);
    br.hasWind = br.hasRain = true;
    RfSensorReading& it = add(4, 0x1700002C, RF_PROTO_ITPLUS17, nullptr, 2);
    it.hasTemp = true;
    hostSetMine("39582376,1700002C");
    setKey(0x1700002C, "kuehlschrank");
    const std::string brInternalId = internalIdOf(0x39582376), itInternalId = internalIdOf(0x1700002C);
    pollAll();
    {
        JsonDocument w = parse(retained.at(cfgTopic("sensor", brInternalId, "wind_speed")));
        assert(w["unit_of_measurement"] == "mph" && w["device"]["name"] == "Bresser weather station 39582376");   // key = proposed radio ID
        assert(w["device"]["model"] == "Bresser" && w["device"]["manufacturer"] == "Bresser");
        JsonDocument r = parse(retained.at(cfgTopic("sensor", brInternalId, "rain_total")));
        assert(r["unit_of_measurement"] == "in");
        JsonDocument t = parse(retained.at(cfgTopic("sensor", itInternalId, "temp")));
        assert(t["unit_of_measurement"] == "\xC2\xB0" "F" && t["device"]["name"] == "IT+ thermo-hygro kuehlschrank");
    }
    assert(configsOf(wsInternalId) == 0 && hostHaRfIds == brInternalId + "," + itInternalId);   // a new table: the WS90's old entry is gone

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
        const LogicalSensorTable keep = gLogicalSensors;
        hostSetMine("45003660,5530CF55,25000050,A2005D8D,3500305E");
        setKey(0x5530CF55, "keller-leck");
        setKey(0x25000050, "druck");
        const std::string aqI = internalIdOf(0x45003660), lkI = internalIdOf(0x5530CF55), thI = internalIdOf(0x25000050), soI = internalIdOf(0xA2005D8D), lwI = internalIdOf(0x3500305E);
        pollAll();
        assert(configsOf(aqI) == 4 + 2 + 5 && configsOf(lkI) == 4 + 1 && configsOf(thI) == 4 + 3);
        assert(configsOf(soI) == 4 + 3 && configsOf(lwI) == 4 + 1);
        JsonDocument pm = parse(retained.at(cfgTopic("sensor", aqI, "pm25")));
        assert(pm["device_class"] == "pm25" && pm["unit_of_measurement"] == "\xC2\xB5g/m\xC2\xB3" && pm["state_class"] == "measurement");
        assert(pm["value_template"] == "{{ value_json.pm25 | default(None) }}" && pm["availability"].size() == 2);
        assert(parse(retained.at(cfgTopic("sensor", aqI, "pm10")))["device_class"] == "pm10");
        assert(parse(retained.at(cfgTopic("sensor", aqI, "pm1")))["device_class"] == "pm1");
        assert(parse(retained.at(cfgTopic("sensor", aqI, "pm4")))["device_class"].isNull());
        JsonDocument co = parse(retained.at(cfgTopic("sensor", aqI, "co2")));
        assert(co["device_class"] == "carbon_dioxide" && co["unit_of_measurement"] == "ppm");
        JsonDocument l = parse(retained.at(cfgTopic("binary_sensor", lkI, "leak")));
        assert(l["device_class"] == "moisture" && l["value_template"] == "{{ 'ON' if value_json.leak else 'OFF' }}");
        assert(l["entity_category"].isNull() && l["availability"].size() == 2 && l["device"]["name"] == "WH55 keller-leck");
        assert(l["state_topic"] == "garden/rf/keller-leck");
        JsonDocument p = parse(retained.at(cfgTopic("sensor", thI, "pressure")));
        assert(p["device_class"] == "atmospheric_pressure" && p["unit_of_measurement"] == "inHg");   // imperial here
        JsonDocument ec = parse(retained.at(cfgTopic("sensor", soI, "conductivity")));
        assert(ec["device_class"] == "conductivity" && ec["unit_of_measurement"] == "\xC2\xB5S/cm");
        JsonDocument w = parse(retained.at(cfgTopic("sensor", lwI, "leaf_wetness")));
        assert(w["unit_of_measurement"] == "%" && w["device_class"] == "moisture");
        // state topic: the rfSensors[] object with the new keys
        sent.clear();
        haRfPublishStates(OUT);
        bool seenLeak = false, seenPress = false;
        for (const Msg& x : sent) {
            if (x.topic == "garden/rf/keller-leck") {
                JsonDocument d = parse(x.payload);
                assert(d["leak"] == true && d["leakRaw"] == 267 && d["leakHighSensitivity"] == false && d["channel"] == 4);
                seenLeak = true;
            } else if (x.topic == "garden/rf/druck") {
                JsonDocument d = parse(x.payload);
                assert(fabs(d["pressure"].as<float>() - 29.58f) < 0.006f);   // 1001.7 hPa in inHg, 2 decimals
                seenPress = true;
            }
        }
        assert(seenLeak && seenPress);
        gLogicalSensors = keep;
        pollAll();
        assert(configsOf(aqI) == 0 && configsOf(lkI) == 0 && configsOf(soI) == 0);   // all cleared
    }

    // ---- empty table again: no per-sensor device left ----
    gLogicalSensors = LogicalSensorTable();
    haRfPoll(OUT, 100);
    assert(retained.empty() && hostHaRfIds.empty());

    // ---- a stored list of 32 radio IDs (written by a build before A5c) loads in full ----
    std::string old32;
    for (int k = 0; k < 32; k++) {
        char e[16];
        snprintf(e, sizeof(e), "%s%08X", k ? "," : "", 0x40000000u + k);
        old32 += e;
    }
    assert(old32.size() < HA_RF_IDS_LEN);
    hostHaRfIds = old32;
    haRfConnected();
    sent.clear();
    haRfPoll(OUT, 0);
    assert((int)sent.size() == 32 * HA_RF_ENTITY_COUNT && hostHaRfIds.empty());

    // ---- 32 ★ sensors: 32 devices, all 32 internal IDs stored ----
    rfSensorsClear();
    std::string list;
    for (int k = 0; k < RF_MAX_SENSORS; k++) {
        RfSensorReading& r = add(k, 0x30000000u + k, RF_PROTO_FINEOFFSET, "WH31", 2);
        r.hasTemp = r.hasHumidity = true;
        char e[16];
        snprintf(e, sizeof(e), "%s%08X", k ? ", " : "", 0x30000000u + k);
        list += e;
    }
    hostSetMine(list.c_str());
    haRfConnected();
    sent.clear();
    assert(pollAll() == RF_MAX_SENSORS * 6);                     // temp, humidity, battery, rssi, age, reception
    assert(configsOf(internalIdOf(0x30000000)) == 6 && configsOf(internalIdOf(0x3000001F)) == 6);
    assert(hostHaRfIds.size() == 32 * 8 - 1 && hostHaRfIds.size() < HA_RF_IDS_LEN);
    assert(haRfPublishStates(OUT) == RF_MAX_SENSORS);
    {
        JsonDocument d = parse(sent.back().payload);
        assert(sent.back().topic == "garden/rf/3000001F" && d["slot"] == 32);   // ★: custom ID = the proposed radio ID
    }
    gLogicalSensors = LogicalSensorTable();
    haRfPoll(OUT, 0);
    assert(retained.empty() && hostHaRfIds.empty());
    assert(hostLsLockDepth == 0);   // every locked section was left
    return 0;
}
