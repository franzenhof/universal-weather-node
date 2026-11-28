// Network sensors (src/net_sensor.cpp, net_json.h): ID namespace, plain and
// JSON payloads, the dotted key paths, the "Universal Weather Node" preset
// against the real /api/data examples, REST replies, the table write and the
// staleness guard.
#include "globals.h"
#include "net_json.h"
#include "rf_sensors.h"
#include "sources.h"
#include "lsensors.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }
static bool has(const VsValues& v, int f) { return v.has & (1u << f); }

static std::string readFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    assert(f && "run from the repository root");
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static uint8_t parse(const NetSlotCfg& c, const std::string& json, VsValues& v)
{
    return netParseJson(c, (const uint8_t*)json.data(), json.size(), v);
}

int main()
{
    // ---- ID namespace: E1000001 .. E1000004, no radio family there ----
    static_assert((NET_ID_BASE >> 24) == 0xE1, "network sensor IDs: family byte E1");
    assert(netSensorId(0) == 0xE1000001UL && netSensorId(3) == 0xE1000004UL);
    for (int i = 0; i < NET_SLOTS; i++) assert(netSlotOfId(netSensorId(i)) == i);
    assert(netSlotOfId(0xE1000000UL) == -1 && netSlotOfId(0xE1000005UL) == -1 && netSlotOfId(0xE1010001UL) == -1);
    uint32_t eco;
    assert(!rfEcowittId(netSensorId(0), eco));                           // not a Fine Offset family
    const uint32_t radio[] = { 0x900181A2UL, 0x3000020FUL, 0x25000042UL, RF_ID_DAVIS | 1, rfItPlusId(true, 5, true),
                               rfItPlusId(false, 0x45, false), 0x0000A1F3UL, 0x000000FFUL };
    for (uint32_t id : radio) assert(netSlotOfId(id) == -1);
    assert(rfIsNetProto(RF_PROTO_MQTT) && rfIsNetProto(RF_PROTO_REST) && !rfIsNetProto(RF_PROTO_FINEOFFSET) && !rfIsNetProto(RF_PROTO_DAVIS));
    assert(!rfProtoValid(RF_PROTO_MQTT) && !rfProtoValid(RF_PROTO_REST));   // never a receiver setting
    assert(!strcmp(rfProtoName(RF_PROTO_MQTT), "MQTT") && !strcmp(rfProtoName(RF_PROTO_REST), "REST"));

    // ---- plain payloads ----
    float x = 0;
    assert(netParseNumber(" 21.5\r\n", 7, x) && near(x, 21.5f));
    assert(netParseNumber("-3", 2, x) && near(x, -3.0f));
    assert(netParseNumber("1e3", 3, x) && near(x, 1000.0f));
    assert(!netParseNumber("", 0, x) && !netParseNumber("  ", 2, x));
    assert(!netParseNumber("21.5 C", 6, x) && !netParseNumber("abc", 3, x) && !netParseNumber("nan", 3, x) && !netParseNumber("inf", 3, x));
    assert(!netParseNumber("{\"t\":1}", 7, x));

    // ---- topics ----
    NetSlotCfg cfg[NET_SLOTS];
    cfg[0].type = NET_MQTT; cfg[0].format = NET_FMT_UWN;   strcpy(cfg[0].addr, "rannungen/data");
    cfg[1].type = NET_MQTT; cfg[1].format = NET_FMT_PLAIN; strcpy(cfg[1].addr, "esphome/garden/");
    strcpy(cfg[1].key[VF_TEMP], "temperature/state"); strcpy(cfg[1].key[VF_HUM], "humidity/state");
    cfg[2].type = NET_REST; strcpy(cfg[2].addr, "http://192.168.1.5/api/data");
    char t[200];
    assert(netTopicOf(cfg[0], -1, t, sizeof(t)) && !strcmp(t, "rannungen/data"));
    assert(!netTopicOf(cfg[0], VF_TEMP, t, sizeof(t)));
    assert(netTopicOf(cfg[1], VF_HUM, t, sizeof(t)) && !strcmp(t, "esphome/garden/humidity/state"));
    assert(!netTopicOf(cfg[1], -1, t, sizeof(t)) && !netTopicOf(cfg[1], VF_PRESS, t, sizeof(t)));
    assert(!netTopicOf(cfg[2], -1, t, sizeof(t)));                       // REST: no topic
    int field = 99;
    assert(netMatchTopic(cfg, "rannungen/data", &field) == 0 && field == -1);
    assert(netMatchTopic(cfg, "esphome/garden/temperature/state", &field) == 1 && field == VF_TEMP);
    assert(netMatchTopic(cfg, "esphome/garden/pressure/state", &field) == -1);
    assert(netMatchTopic(cfg, "rannungen/data/x", &field) == -1 && netMatchTopic(cfg, "http://192.168.1.5/api/data", &field) == -1);
    // exact topics only: an MQTT wildcard in the address or a plain key is refused (and never subscribed)
    assert(netSlotTopicsValid(cfg[0]) && netSlotTopicsValid(cfg[1]) && netSlotTopicsValid(cfg[2]));
    NetSlotCfg wild = cfg[1];
    strcpy(wild.key[VF_PRESS], "#");
    assert(!netSlotTopicsValid(wild) && !netTopicOf(wild, VF_PRESS, t, sizeof(t)));
    assert(netTopicOf(wild, VF_TEMP, t, sizeof(t)));                       // the other values stay subscribed
    strcpy(wild.key[VF_PRESS], "pressure/+");
    assert(!netSlotTopicsValid(wild));
    wild = cfg[0]; strcpy(wild.addr, "rannungen/#");
    assert(!netSlotTopicsValid(wild) && !netTopicOf(wild, -1, t, sizeof(t)));
    wild = cfg[0]; wild.addr[0] = '\0';
    assert(!netSlotTopicsValid(wild));                                     // JSON / preset: the topic is needed
    NetSlotCfg jsonKeys = cfg[0]; jsonKeys.format = NET_FMT_JSON; strcpy(jsonKeys.key[VF_TEMP], "a+b");
    assert(netSlotTopicsValid(jsonKeys));                                  // a JSON key is no topic

    // ---- JSON keys: dotted paths, arrays, numbers as text ----
    NetSlotCfg j;
    j.type = NET_MQTT;
    strcpy(j.key[VF_TEMP], "main.temp");
    strcpy(j.key[VF_HUM], "main.humidity");          // a second key under "main": the filter keeps both
    strcpy(j.key[VF_WIND], "wind.speed");
    strcpy(j.key[VF_DIR], "wind.deg");
    strcpy(j.key[VF_PRESS], "list.1.p");
    strcpy(j.key[VF_UV], "uv");
    VsValues v;
    assert(parse(j, "{\"main\":{\"temp\":12.5,\"humidity\":\"80\"},\"wind\":{\"speed\":3.2,\"deg\":270},"
                    "\"list\":[{\"p\":1},{\"p\":1001.5}],\"uv\":true,\"other\":[1,2,3]}", v) == NET_ERR_NONE);
    assert(has(v, VF_TEMP) && near(v.v[VF_TEMP], 12.5f));
    assert(has(v, VF_HUM) && near(v.v[VF_HUM], 80.0f));
    assert(has(v, VF_WIND) && near(v.v[VF_WIND], 3.2f) && has(v, VF_DIR) && near(v.v[VF_DIR], 270.0f));
    assert(has(v, VF_PRESS) && near(v.v[VF_PRESS], 1001.5f));
    assert(!has(v, VF_UV));                           // true is no number
    assert(!has(v, VF_GUST) && !has(v, VF_RAIN));     // not mapped
    assert(parse(j, "{\"main\":{\"temp\":", v) == NET_ERR_JSON);
    assert(parse(j, "not json", v) == NET_ERR_JSON);
    assert(parse(j, "{\"foo\":1}", v) == NET_ERR_NO_VALUES && !v.has);
    assert(parse(j, "{\"main\":{\"temp\":{\"x\":1}}}", v) == NET_ERR_NO_VALUES);
    {
        JsonDocument d;
        deserializeJson(d, "{\"a\":{\"b\":[0,{\"c\":\"-4.5\"}]}}");
        assert(netPath(d.as<JsonVariantConst>(), "a.b.1.c", x) && near(x, -4.5f));
        assert(!netPath(d.as<JsonVariantConst>(), "a.b.2.c", x) && !netPath(d.as<JsonVariantConst>(), "a.", x) &&
               !netPath(d.as<JsonVariantConst>(), "", x) && !netPath(d.as<JsonVariantConst>(), "a.b", x));
    }

    // ---- the preset against real <prefix>/data examples (km/h; imperial) ----
    NetSlotCfg u;
    u.type = NET_MQTT; u.format = NET_FMT_UWN; strcpy(u.addr, "x/data");
    assert(parse(u, readFile("docs/api/examples/heltec-v4-wireless/data.json"), v) == NET_ERR_NONE);
    assert(near(v.v[VF_TEMP], 18.34162f) && near(v.v[VF_HUM], 49.64927f) && near(v.v[VF_PRESS], 946.8036f));
    assert(near(v.v[VF_WIND], 14.19032f / 3.6f) && near(v.v[VF_GUST], 25.75831f / 3.6f) && near(v.v[VF_DIR], 289.0f));
    assert(has(v, VF_RAIN) && near(v.v[VF_RAIN], 0.0f) && !has(v, VF_UV) && !has(v, VF_LUX));
    assert(parse(u, readFile("docs/api/examples/pcb-v2-imperial/data.json"), v) == NET_ERR_NONE);
    assert(near(v.v[VF_TEMP], (65.00186f - 32.0f) / 1.8f) && near(v.v[VF_PRESS], 27.95938f * 33.8639f, 0.05f));
    assert(near(v.v[VF_WIND], 9.20782f / 2.23694f) && near(v.v[VF_RAIN], 0.527559f * 25.4f));
    assert(parse(u, readFile("docs/api/examples/bresser-7in1-bme280/data.json"), v) == NET_ERR_NONE);
    assert(near(v.v[VF_UV], 4.1f) && near(v.v[VF_LUX], 49902.0f, 1.0f) && near(v.v[VF_RAIN], 16.99999f));   // m/s: as it is
    assert(parse(u, "{\"apiVersion\":1,\"status\":{}}", v) == NET_ERR_NO_VALUES);

    // ---- REST replies ----
    {
        JsonDocument d;
        DeserializationError ok = deserializeJson(d, "{\"temp\":5,\"tempUnit\":\"C\"}");
        NetSlotCfg r = u;
        r.type = NET_REST;
        assert(netRestResult(r, 200, ok, d.as<JsonVariantConst>(), v) == NET_ERR_NONE && near(v.v[VF_TEMP], 5.0f));
        assert(netRestResult(r, 404, ok, d.as<JsonVariantConst>(), v) == NET_ERR_HTTP && !v.has);
        assert(netRestResult(r, -1, ok, d.as<JsonVariantConst>(), v) == NET_ERR_HTTP);   // connection refused / timeout
        assert(netRestResult(r, 200, DeserializationError::InvalidInput, d.as<JsonVariantConst>(), v) == NET_ERR_JSON);
        JsonDocument e;
        deserializeJson(e, "[1,2]");
        assert(netRestResult(r, 200, DeserializationError::Ok, e.as<JsonVariantConst>(), v) == NET_ERR_NO_VALUES);
        JsonDocument filter;
        netFilter(j, filter);                         // only the mapped keys are kept
        assert(filter["main"]["temp"] == true && filter["main"]["humidity"] == true && filter["list"][0]["p"] == true);
        assert(filter["other"].isNull());
    }

    // ---- into the table's form; wind needs the mean speed ----
    NetWindMemo memo;
    RfValues rv;
    VsValues in;
    in.has = 1u << VF_GUST; in.v[VF_GUST] = 9.0f;          // plain topics: the gust alone
    assert(!netToRfValues(NET_FMT_PLAIN, in, memo, rv));
    in.has = 1u << VF_DIR; in.v[VF_DIR] = -90.0f;
    assert(!netToRfValues(NET_FMT_PLAIN, in, memo, rv));
    in.has = (1u << VF_WIND) | (1u << VF_HUM); in.v[VF_WIND] = 4.0f; in.v[VF_HUM] = 104.6f;
    assert(netToRfValues(NET_FMT_PLAIN, in, memo, rv) && (rv.ok & RF_VAL_WIND) && (rv.ok & RF_VAL_HUM));
    assert(near(rv.windAvgMs, 4.0f) && near(rv.windGustMs, 9.0f) && near(rv.windDirDeg, 270.0f) && rv.humidity == 100);
    NetWindMemo fresh;
    in.has = 1u << VF_WIND;
    assert(netToRfValues(NET_FMT_PLAIN, in, fresh, rv) && near(rv.windGustMs, 4.0f) && near(rv.windDirDeg, 0.0f));   // nothing known: gust = mean
    in.has = 1u << VF_TEMP; in.v[VF_TEMP] = NAN;
    assert(!netToRfValues(NET_FMT_PLAIN, in, fresh, rv));
    // JSON / REST / preset: one message is complete - a gust or direction of an
    // earlier message is never carried into a later one that has none
    for (uint8_t fmt : { (uint8_t)NET_FMT_JSON, (uint8_t)NET_FMT_UWN }) {
        NetWindMemo perSlot;
        in.has = (1u << VF_WIND) | (1u << VF_GUST) | (1u << VF_DIR); in.v[VF_WIND] = 8.0f; in.v[VF_GUST] = 15.0f; in.v[VF_DIR] = 90.0f;
        assert(netToRfValues(fmt, in, perSlot, rv) && near(rv.windGustMs, 15.0f) && near(rv.windDirDeg, 90.0f));
        in.has = 1u << VF_WIND; in.v[VF_WIND] = 2.0f;        // calm: the source leaves gust / direction out
        assert(netToRfValues(fmt, in, perSlot, rv) && near(rv.windGustMs, 2.0f) && near(rv.windDirDeg, 0.0f));
    }

    // ---- the table: protocol label, ★ binding, staleness ----
    gCfg.rfEn = RF_RADIO_OFF;
    for (int i = 0; i < NET_SLOTS; i++) gCfg.net[i] = NetSlotCfg();
    assert(!rfTableOn());
    gCfg.net[0].type = NET_MQTT; gCfg.net[0].format = NET_FMT_UWN; strcpy(gCfg.net[0].addr, "rannungen/data");
    gCfg.net[0].freshMin = 10;
    gCfg.net[1].type = NET_REST; gCfg.net[1].freshMin = 2;
    assert(rfTableOn());                                   // network sensors alone: the table is in use
    hostNow = 1000000;
    RfValues a;
    a.ok = RF_VAL_TEMP | RF_VAL_RAIN; a.tempC = 21.0f; a.rainTotalMm = 3.0f;
    assert(netApply(0, NET_MQTT, a, hostNow));
    int idx = -1;
    for (int i = 0; i < RF_MAX_SENSORS; i++) if (gRfSensors[i].used && gRfSensors[i].id == netSensorId(0)) idx = i;
    assert(idx >= 0 && gRfSensors[idx].proto == RF_PROTO_MQTT && gRfSensors[idx].rssi == 0.0f && gRfSensors[idx].hasTemp);
    assert(rfFreshMs(gRfSensors[idx]) == 600000UL);       // the slot's window, not the radio's 5 min
    assert(srcRfValueFresh(idx, "temp"));
    hostNow += 400000;                                     // 6.7 min: a radio sensor would be stale, this one not
    rfExpireValues(hostNow);
    assert(srcRfValueFresh(idx, "temp") && !(gRfSensors[idx].staleMask & RF_VAL_TEMP));
    hostNow += 201000;                                     // > 10 min without a message (VPN down)
    rfExpireValues(hostNow);
    assert(!srcRfValueFresh(idx, "temp") && (gRfSensors[idx].staleMask & RF_VAL_TEMP) && (gRfSensors[idx].staleMask & RF_VAL_ANY));
    assert(!rfSensorFresh(gRfSensors[idx]));
    a.rainTotalMm = 3.5f;
    assert(netApply(0, NET_MQTT, a, hostNow));             // back: fresh again
    assert(srcRfValueFresh(idx, "temp") && !(gRfSensors[idx].staleMask & RF_VAL_ANY));
    assert(netApply(1, NET_REST, a, hostNow));
    int ridx = -1;
    for (int i = 0; i < RF_MAX_SENSORS; i++) if (gRfSensors[i].used && gRfSensors[i].id == netSensorId(1)) ridx = i;
    assert(ridx >= 0 && gRfSensors[ridx].proto == RF_PROTO_REST && rfFreshMs(gRfSensors[ridx]) == 120000UL);
    assert(!rfSameKind(gRfSensors[idx], gRfSensors[ridx]) && !rfSameKind(gRfSensors[idx], gRfSensors[idx]));   // no battery change offers
    // a ★ network sensor, then "only my sensors": still taken; hidden: not
    assert(rfSetState(netSensorId(0), RF_STATE_MINE) && rfStateOf(netSensorId(0)) == RF_STATE_MINE);
    gCfg.rfOnlyMine = 1;
    assert(netApply(0, NET_MQTT, a, hostNow));
    assert(!netApply(2, NET_MQTT, a, hostNow));            // not ★ while only the ★ ones are accepted
    gCfg.rfOnlyMine = 0;
    assert(rfSetState(netSensorId(3), RF_STATE_HIDDEN));
    assert(!netApply(3, NET_MQTT, a, hostNow));
    RfValues none;
    assert(!netApply(0, NET_MQTT, none, hostNow) && !netApply(-1, NET_MQTT, a, hostNow) && !netApply(NET_SLOTS, NET_MQTT, a, hostNow));

    // ---- sanitizing a loaded / restored slot ----
    NetSlotCfg s;
    s.type = 9; s.format = 7; s.freshMin = 0; s.pollSec = 5;
    memset(s.addr, 'x', sizeof(s.addr));
    netSanitize(s);
    assert(s.type == NET_OFF && s.format == NET_FMT_JSON && s.freshMin == NET_FRESH_MIN_DEFAULT && s.pollSec == NET_POLL_SEC_DEFAULT);
    assert(strlen(s.addr) == NET_ADDR_LEN - 1);
    s.type = NET_REST; s.format = NET_FMT_PLAIN;
    netSanitize(s);
    assert(s.format == NET_FMT_JSON);                      // plain numbers only over MQTT
    assert(netFreshMs(s) == 600000UL);

    printf("Firmware netsensor regressions passed.\n");
    return 0;
}
