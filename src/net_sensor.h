#pragma once
// Network sensors (C2, TODO item 14, docs/network-sensors.md): values that
// arrive over the network - an MQTT topic on the node's broker or a polled
// JSON URL - enter the sensor table like a wireless sensor. Everything that
// reads the table (value sources, ★ / custom ID / name, placeholders, history,
// dashboard, /api/data, MQTT-out, Home Assistant, weather networks, LoRaWAN,
// virtual sensors) takes them as it is.
//
// A normal, opt-in source, labelled everywhere (protocol "MQTT" / "REST"
// instead of a radio protocol), with the normal staleness rule: a value not
// refreshed within the slot's freshness window (default 10 min) is stale like
// a silent radio sensor's, so a dropped VPN / broker never keeps an old value
// as current. Nothing else is policed: what a network sensor feeds is the
// user's choice (value sources), as with any other sensor.
//
// IDs: slot n (0-based) is the table ID NET_ID_BASE + n + 1 (E1000001 ..
// E1000004). No radio family uses that range (Fine Offset family bytes 0x20 -
// 0xA2, Davis DA0000ii, IT+ 17 / 09, virtual sensors = Fine Offset IDs;
// Bresser 5/7-in-1 and lightning IDs are 8 / 16 bit); a Bresser 6-in-1 or
// leakage sensor with exactly such a 32-bit ID would be dropped by the
// receiver (rf_sensors.cpp idAccepted()).
//
// This header has no JSON / network dependencies (config.h includes it);
// the JSON part is net_json.h, the device side net_run.cpp.
#include <stddef.h>
#include <stdint.h>
#include "vsensor.h"   // the value set VF_* (the same as the virtual sensors' - mapping in vs. mapping out)

constexpr int NET_SLOTS = 4;
enum NetType : uint8_t { NET_OFF = 0, NET_MQTT, NET_REST, NET_TYPE_COUNT };
// JSON: one message / reply, a key per value (dotted path, "main.temp");
// PLAIN (MQTT only): one topic per value, the payload a number;
// UWN: the <prefix>/data JSON (MQTT) or /api/data (REST) of another node of
// this firmware - keys and units taken from it, nothing to map.
enum NetFormat : uint8_t { NET_FMT_JSON = 0, NET_FMT_PLAIN, NET_FMT_UWN, NET_FMT_COUNT };

constexpr size_t   NET_ADDR_LEN = 160;   // topic (JSON / preset), topic prefix (plain) or URL
constexpr size_t   NET_KEY_LEN  = 32;    // JSON path, or the topic after the prefix (plain)
constexpr uint16_t NET_FRESH_MIN_DEFAULT = 10, NET_FRESH_MIN_MAX = 1440;    // minutes
constexpr uint16_t NET_POLL_SEC_MIN = 30, NET_POLL_SEC_DEFAULT = 60, NET_POLL_SEC_MAX = 3600;
// PubSubClient buffer while an MQTT network sensor is on: holds another node's
// <prefix>/data (about 5 KB with 32 wireless sensors). Blocks of 4 KB and more
// come from the PSRAM (psram.h).
constexpr uint16_t NET_MQTT_BUFFER = 8192;

struct NetSlotCfg {
    uint8_t  type     = NET_OFF;
    uint8_t  format   = NET_FMT_JSON;
    uint16_t freshMin = NET_FRESH_MIN_DEFAULT;   // values older than this are stale
    uint16_t pollSec  = NET_POLL_SEC_DEFAULT;    // REST only
    char     addr[NET_ADDR_LEN] = "";
    char     key[VF_COUNT][NET_KEY_LEN] = {};    // "" = value not taken
};

constexpr uint32_t NET_ID_BASE = 0xE1000000UL;
inline uint32_t netSensorId(int slot) { return NET_ID_BASE | (uint32_t)(slot + 1); }
// slot of a table ID, -1 = not a network sensor's ID
inline int netSlotOfId(uint32_t id)
{
    const uint32_t n = id ^ NET_ID_BASE;
    return n >= 1 && n <= (uint32_t)NET_SLOTS ? (int)n - 1 : -1;
}
inline bool netOn(const NetSlotCfg& c) { return c.type == NET_MQTT || c.type == NET_REST; }
inline bool netAnyOn(const NetSlotCfg cfg[NET_SLOTS])
{
    for (int i = 0; i < NET_SLOTS; i++) if (netOn(cfg[i])) return true;
    return false;
}
inline unsigned long netFreshMs(const NetSlotCfg& c)
{
    const uint16_t m = c.freshMin >= 1 && c.freshMin <= NET_FRESH_MIN_MAX ? c.freshMin : NET_FRESH_MIN_DEFAULT;
    return (unsigned long)m * 60000UL;
}
// Brings a loaded / restored slot into range (unknown type = off, strings terminated).
void netSanitize(NetSlotCfg& c);

const char* netFormatKey(uint8_t f);   // "json", "plain", "uwn" (API)

// A plain MQTT payload: a number, surrounded by blanks at most. false = not a finite number.
bool netParseNumber(const char* s, size_t len, float& out);

// MQTT: exact topics only - no '+' / '#' in the address or (plain) a key. false = refused.
bool netSlotTopicsValid(const NetSlotCfg& c);
// The MQTT topics of a slot (a topic with a wildcard: none): field -1 = the slot's topic (JSON / preset),
// field f = the plain topic of value f (prefix + key). false = none.
bool netTopicOf(const NetSlotCfg& c, int field, char* out, size_t len);
// The slot (and *field: -1 = JSON / preset, else VF_*) an incoming topic belongs to; -1 = none.
int  netMatchTopic(const NetSlotCfg cfg[NET_SLOTS], const char* topic, int* field);

// The values of one message / reply (metric: °C, %, hPa, m/s, °, mm running total, UV index, lux)
// into the table's form. Wind needs the mean speed. Plain topics (one value per
// message): a gust / direction is remembered in the slot's memo for the next
// mean speed. JSON / REST / preset: a message is complete, the memo is not used.
// Without a gust: gust = mean; without a direction: 0. false = nothing to store.
struct RfValues;
struct NetWindMemo { float gust = 0.0f, dir = 0.0f; bool gustKnown = false, dirKnown = false; };
bool netToRfValues(uint8_t format, const VsValues& in, NetWindMemo& slotMemo, RfValues& out);

// Writes one message's values into the sensor table (loop task only): the
// slot's table ID, protocol MQTT / REST, receive time `now`, the normal value
// sources. false = not taken (hidden, "only my sensors" and not ★, table full).
bool netApply(int slot, uint8_t type, const RfValues& v, unsigned long now);

// Status per slot (status page, /api/info). Written by the loop / upload task.
enum NetErr : uint8_t { NET_ERR_NONE = 0, NET_ERR_HTTP, NET_ERR_JSON, NET_ERR_NO_VALUES, NET_ERR_REFUSED, NET_ERR_WAIT };
struct NetSlotStatus {
    uint32_t      received = 0;     // messages / replies with at least one value
    uint32_t      failed = 0;       // replies / messages without a usable value
    unsigned long lastOkMs = 0;     // millis() of the last one with values
    int16_t       lastHttp = 0;     // REST: last HTTP code (negative = connection error)
    uint8_t       lastErr = NET_ERR_NONE;
};
extern NetSlotStatus gNetStatus[NET_SLOTS];
const char* netErrKey(uint8_t e);   // "http", "json", "noValues", ... (API)

// ---- device side (net_run.cpp) ----
// gCfg.net is written by the web task (settings POST) and read by the MQTT
// callback (loop task) and the REST poll (upload task).
void netLock();
void netUnlock();
extern volatile uint32_t gNetCfgGen;   // bumped by every settings change of gCfg.net
// MQTT (wifi_mqtt.cpp, loop task): a message on a topic that is not the
// command topic; false = no network sensor's topic.
bool netMqttMessage(const char* topic, const uint8_t* payload, unsigned int len);
// MQTT, while connected: keeps the subscriptions in step with the settings.
// connected = the connection is new (nothing subscribed yet).
void netMqttService(bool connected, bool (*sub)(const char*), bool (*unsub)(const char*));
bool netMqttWanted();                  // an MQTT network sensor is on (buffer size)
// The values of a REST reply (upload task) for slot i: handed to the loop task.
void netPost(int slot, const VsValues& v);
// Loop task, every iteration: stores the values handed over since the last call.
void netLoop();
