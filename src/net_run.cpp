// Network sensors, device side (net_sensor.h): the hand-over of received
// values to the loop task, the MQTT topics and subscriptions. The REST poll
// runs in the upload task (wx_upload.cpp netRestService()).
//
// All writes to the sensor table happen in the loop task (like the radio's):
// the MQTT callback (loop task too) and the REST poll (upload task) hand their
// values over through a small box per slot; netLoop() stores them.
#include "net_json.h"
#include "config.h"
#include "globals.h"
#include "rf_sensors.h"
#include "serial_log.h"
#include <Arduino.h>
#include <mutex>
#include <vector>

volatile uint32_t gNetCfgGen = 0;

namespace {
std::mutex   cfgMutex;
portMUX_TYPE boxMux = portMUX_INITIALIZER_UNLOCKED;
struct Box { VsValues v; bool has = false; };
Box         box[NET_SLOTS];                // guarded by boxMux
NetWindMemo windMemo[NET_SLOTS];           // loop task only (plain topics)
std::vector<String> subscribed;            // loop task only (MQTT)
uint32_t    subscribedGen = 0;
bool        subscribedValid = false;
}

void netLock()   { cfgMutex.lock(); }
void netUnlock() { cfgMutex.unlock(); }

void netPost(int slot, const VsValues& v)
{
    if (slot < 0 || slot >= NET_SLOTS || !v.has) return;
    portENTER_CRITICAL(&boxMux);
    Box& b = box[slot];   // a value not yet stored is overwritten by a newer one
    for (int f = 0; f < VF_COUNT; f++)
        if (v.has & (1u << f)) { b.v.v[f] = v.v[f]; b.v.has |= (uint16_t)(1u << f); }
    b.has = true;
    portEXIT_CRITICAL(&boxMux);
}

void netLoop()
{
    for (int i = 0; i < NET_SLOTS; i++) {
        Box b;
        portENTER_CRITICAL(&boxMux);
        if (box[i].has) { b = box[i]; box[i] = Box(); }
        portEXIT_CRITICAL(&boxMux);
        if (!b.has) continue;
        const uint8_t type = gCfg.net[i].type;     // one byte each, no lock needed
        const uint8_t format = gCfg.net[i].format;
        RfValues rv;
        if (!netOn(gCfg.net[i]) || !netToRfValues(format, b.v, windMemo[i], rv)) continue;
        if (!netApply(i, type, rv, millis())) gNetStatus[i].lastErr = NET_ERR_REFUSED;
    }
}

bool netMqttWanted()
{
    for (int i = 0; i < NET_SLOTS; i++) if (gCfg.net[i].type == NET_MQTT) return true;
    return false;
}

bool netMqttMessage(const char* topic, const uint8_t* payload, unsigned int len)
{
    int field = -1;
    VsValues v;
    uint8_t err = NET_ERR_NO_VALUES;
    float x;
    netLock();   // parsed under the lock (no copy of the slot): the settings save waits a few ms at most
    const int slot = netMatchTopic(gCfg.net, topic, &field);
    if (slot >= 0 && field >= 0) {   // plain: the payload is the number
        if (netParseNumber((const char*)payload, len, x)) { v.has = (uint16_t)(1u << field); v.v[field] = x; err = NET_ERR_NONE; }
    } else if (slot >= 0) {
        err = netParseJson(gCfg.net[slot], payload, len, v);
    }
    netUnlock();
    if (slot < 0) return false;
    NetSlotStatus& st = gNetStatus[slot];
    st.lastErr = err;
    if (err == NET_ERR_NONE) { st.received++; st.lastOkMs = millis(); netPost(slot, v); }
    else                     { st.failed++; dbgSerial.printf("Network sensor %d (MQTT %s): %s\n", slot + 1, topic, netErrKey(err)); }
    return true;
}

void netMqttService(bool connected, bool (*sub)(const char*), bool (*unsub)(const char*))
{
    if (connected) { subscribed.clear(); subscribedValid = false; }
    const uint32_t gen = gNetCfgGen;
    if (subscribedValid && subscribedGen == gen) return;
    std::vector<String> want;
    char t[NET_ADDR_LEN + NET_KEY_LEN];
    netLock();
    for (int i = 0; i < NET_SLOTS; i++)
        for (int f = -1; f < VF_COUNT; f++)
            if (netTopicOf(gCfg.net[i], f, t, sizeof(t))) want.push_back(String(t));
    netUnlock();
    auto has = [](const std::vector<String>& l, const String& x) { for (const String& y : l) if (y == x) return true; return false; };
    for (const String& o : subscribed) if (!has(want, o)) unsub(o.c_str());
    for (const String& w : want)
        if (!has(subscribed, w)) dbgSerial.printf("MQTT: network sensor topic '%s' %s\n", w.c_str(), sub(w.c_str()) ? "subscribed" : "NOT subscribed");
    subscribed = want;
    subscribedGen = gen;
    subscribedValid = true;
}
