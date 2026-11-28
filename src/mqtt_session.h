#pragma once
// MQTT session helpers (wifi_mqtt.cpp), host-tested in test/firmware_mqttsession.cpp.
#include <stdint.h>

// Reconnect backoff: 15 s after a failed attempt, doubling up to 5 min while
// the broker stays unreachable. A session that ends within
// MQTT_STABLE_SESSION_MS counts as a failure too (a broker that accepts and
// drops right away - e.g. a second client with the same client ID - must not
// cause a reconnect / discovery storm). Only a session that stayed up that
// long resets the backoff.
constexpr unsigned long MQTT_RETRY_MIN_MS      = 15000;
constexpr unsigned long MQTT_RETRY_MAX_MS      = 300000;
constexpr unsigned long MQTT_STABLE_SESSION_MS = 60000;

struct MqttReconnectBackoff {
    unsigned long retryMs = 0;          // 0 = connect right away
    unsigned long lastAttemptMs = 0;
    unsigned long sessionStartMs = 0;
    bool          sessionUp = false;

    // a connect attempt may start now (wrap-safe)
    bool attemptAllowed(unsigned long now) const { return retryMs == 0 || now - lastAttemptMs >= retryMs; }
    void attemptStarted(unsigned long now) { lastAttemptMs = now; }
    void attemptFailed() { lengthen(); }
    void connected(unsigned long now) { sessionUp = true; sessionStartMs = now; }
    // called on every poll while connected
    void stillConnected(unsigned long now) { if (sessionUp && now - sessionStartMs >= MQTT_STABLE_SESSION_MS) retryMs = 0; }
    // called on the first poll that finds the connection gone
    void sessionLost(unsigned long now)
    {
        if (!sessionUp) return;
        sessionUp = false;
        if (now - sessionStartMs < MQTT_STABLE_SESSION_MS) lengthen();
    }

private:
    void lengthen()
    {
        retryMs = retryMs == 0 ? MQTT_RETRY_MIN_MS : (retryMs * 2 < MQTT_RETRY_MAX_MS ? retryMs * 2 : MQTT_RETRY_MAX_MS);
    }
};

// The RETAIN flag of the PUBLISH packet whose topic PubSubClient (2.8) hands
// to the message callback. PubSubClient does not pass the flag, but the topic
// points into its packet buffer: [0] fixed header (0x3X, bit 0 = RETAIN),
// [1..n] remaining length (1-4 bytes, bit 7 set on all but the last), then the
// topic length MSB, then the topic (moved one byte forward over the LSB).
// A broker sets RETAIN only on a message it delivers because of a new
// subscription (a stored retained message, MQTT 3.1.1 section 3.3.1.3), never
// on a live one - so this tells "left on the broker" from "sent now".
inline bool mqttReceivedRetained(const char* topic)
{
    const uint8_t* lastLengthByte = (const uint8_t*)topic - 2;
    const uint8_t* p = lastLengthByte;
    for (int lengthBytes = 1; lengthBytes < 4 && (p[-1] & 0x80); lengthBytes++) p--;
    const uint8_t header = p[-1];
    return (header & 0xF0) == 0x30 && (header & 0x01);
}
