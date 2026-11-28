// MQTT session helpers (src/mqtt_session.h): reconnect backoff and the
// RETAIN flag of a received message.
#include "mqtt_session.h"
#include <cassert>
#include <cstring>
#include <vector>

// A PUBLISH packet laid out the way PubSubClient 2.8 hands it to the callback:
// topic length LSB overwritten by the topic moved one byte forward.
static std::vector<uint8_t> packetAsCallbackSeesIt(uint8_t header, uint32_t remainingLength, const char* topic)
{
    std::vector<uint8_t> b;
    b.push_back(header);
    do {
        uint8_t digit = remainingLength % 128;
        remainingLength /= 128;
        if (remainingLength) digit |= 0x80;
        b.push_back(digit);
    } while (remainingLength);
    const size_t tl = strlen(topic);
    b.push_back((uint8_t)(tl >> 8));
    for (size_t i = 0; i < tl; i++) b.push_back((uint8_t)topic[i]);
    b.push_back(0);
    b.push_back('{');
    return b;
}

static const char* topicIn(const std::vector<uint8_t>& b, const char* topic)
{
    const size_t tl = strlen(topic);
    for (size_t i = 0; i + tl <= b.size(); i++)
        if (!memcmp(&b[i], topic, tl)) return (const char*)&b[i];
    return nullptr;
}

int main()
{
    // ---- RETAIN flag ----
    const char* t = "node/cmd";
    const uint32_t lengths[] = { 20, 127, 128, 300, 16383, 16384, 70000 };   // 1, 2 and 3 length bytes
    for (uint32_t len : lengths) {
        auto live = packetAsCallbackSeesIt(0x30, len, t);
        auto kept = packetAsCallbackSeesIt(0x31, len, t);
        auto keptQos1 = packetAsCallbackSeesIt(0x33, len, t);
        assert(!mqttReceivedRetained(topicIn(live, t)));
        assert(mqttReceivedRetained(topicIn(kept, t)));
        assert(mqttReceivedRetained(topicIn(keptQos1, t)));
    }

    // ---- backoff ----
    MqttReconnectBackoff b;
    assert(b.attemptAllowed(0));
    b.attemptStarted(1000);
    b.attemptFailed();
    assert(b.retryMs == MQTT_RETRY_MIN_MS && !b.attemptAllowed(1000 + MQTT_RETRY_MIN_MS - 1) && b.attemptAllowed(1000 + MQTT_RETRY_MIN_MS));
    b.attemptFailed();
    assert(b.retryMs == 2 * MQTT_RETRY_MIN_MS);
    for (int i = 0; i < 10; i++) b.attemptFailed();
    assert(b.retryMs == MQTT_RETRY_MAX_MS);

    // a broker that accepts and drops the session right away: the backoff keeps growing
    MqttReconnectBackoff kicked;
    unsigned long now = 5000;
    for (int i = 0; i < 4; i++) {
        assert(kicked.attemptAllowed(now));
        kicked.attemptStarted(now);
        kicked.connected(now);
        kicked.stillConnected(now + 500);
        kicked.sessionLost(now + 1000);
        assert(!kicked.attemptAllowed(now + 1000));
        now += kicked.retryMs;
    }
    assert(kicked.retryMs == 8 * MQTT_RETRY_MIN_MS);

    // a session that stayed up resets it; losing it then reconnects at once
    kicked.attemptStarted(now);
    kicked.connected(now);
    kicked.stillConnected(now + MQTT_STABLE_SESSION_MS - 1);
    assert(kicked.retryMs != 0);
    kicked.stillConnected(now + MQTT_STABLE_SESSION_MS);
    assert(kicked.retryMs == 0);
    kicked.sessionLost(now + MQTT_STABLE_SESSION_MS + 10);
    assert(kicked.retryMs == 0 && kicked.attemptAllowed(now + MQTT_STABLE_SESSION_MS + 10));
    kicked.sessionLost(now + MQTT_STABLE_SESSION_MS + 20);   // no session: nothing changes
    assert(kicked.retryMs == 0);

    // across the millis() wrap
    MqttReconnectBackoff wrap;
    wrap.attemptStarted(0xFFFFF000UL);
    wrap.attemptFailed();
    assert(!wrap.attemptAllowed(0xFFFFF000UL + 1000) && wrap.attemptAllowed(0xFFFFF000UL + MQTT_RETRY_MIN_MS));
    wrap.connected(0xFFFFFF00UL);
    wrap.stillConnected(0xFFFFFF00UL + MQTT_STABLE_SESSION_MS);   // wraps to a small value
    assert(wrap.retryMs == 0);
    return 0;
}
