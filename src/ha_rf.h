#pragma once
// Home Assistant MQTT discovery for the individual wireless sensors: one HA
// device per ★ sensor (logical sensor table gLogicalSensors, lsensors.h) - never
// for sensors merely received (a neighbour's station must not appear in HA);
// with an empty list there are no per-sensor devices.
//
//  - State topic <prefix>/rf/<custom ID> for a ★ sensor (its logical sensor's custom ID,
//    lsensors.h), <prefix>/rf/<radio ID> (8 hex digits) for a received-only
//    one: that sensor's rfSensors[] object (addRfSensorJson()) plus radioId,
//    published every MQTT cycle for every received sensor, not retained.
//  - Device = the logical sensor: identifiers <node>_rf_<internal ID>, name =
//    its name (else "<model> <custom ID>"), via_device = the node; entities only for
//    values the sensor has actually sent (the set only grows while the node
//    runs; announced as they appear). Unique IDs <node>_rf_<internal ID>_<entity>,
//    config topics homeassistant/<sensor|binary_sensor>/<node>/rf_<internal ID>_<entity>/config.
//    The internal ID never changes: a new custom ID, name or radio ID (battery
//    change) re-announces the same entities with the new state topic / name
//    (HA keeps them and their history); the old state topic gets an empty
//    retained payload.
//  - Availability: the node's LWT topic, and for the measured values also
//    the sensor itself (offline once its last packet is older than 5 min);
//    a single stale value (rfSensors[].stale) reads as unknown.
//  - Internal IDs whose configs were published are stored (NVS "harfids",
//    config.cpp); a sensor taken off the ★ list gets its retained configs
//    cleared (empty retained payloads) - also when the list changed while MQTT
//    was down or across a restart. Radio IDs in that list (builds before A5c,
//    one device per radio ID) get their old configs cleared once.
//
// No WiFi/PubSubClient here (host-tested, test/firmware_harf.cpp): the caller
// passes a publish function.
#include <stddef.h>
#include <stdint.h>

constexpr const char* HA_DISCOVERY_PREFIX = "homeassistant";

struct HaRfOut {
    const char* clientId;     // node_id / identifier of the node's HA device (mqttClientId())
    const char* prefix;       // MQTT topic prefix (topicPrefix())
    const char* availTopic;   // the node's availability topic (LWT)
    // Sends one message; false = not sent (retried on a later call).
    bool (*publish)(const char* topic, const char* payload, size_t len, bool retain, void* ctx);
    void* ctx;
};

// Entities per sensor (bit n of a mask = entity n).
constexpr int HA_RF_ENTITY_COUNT = 31;   // fits the 32-bit masks

// Bit mask of the entities gRfSensors[idx] has values for (0 = unused entry).
uint32_t haRfEntityMask(int idx);
// Entity key ("temp", "battery", ...) and HA component ("sensor" /
// "binary_sensor") of entity n; nullptr for n out of range.
const char* haRfEntityKey(int n);
const char* haRfEntityComponent(int n);

// "<prefix>/rf/<sensor>" (sensor = a custom ID, or "rf" + the radio ID)
void haRfStateTopic(char* out, size_t len, const char* prefix, const char* sensor);

// A new MQTT connection: every config is announced again.
void haRfConnected();

// Clears the configs of sensors no longer ★, then announces the entities of
// the ★, received sensors not announced yet in this connection, with at
// most `budget` config publishes (removals are not limited: small payloads).
// Returns the number of messages published. Call regularly while connected.
int haRfPoll(const HaRfOut& out, int budget);

// Publishes <prefix>/rf/<custom ID or rf+radio ID> for every received sensor; returns the count.
int haRfPublishStates(const HaRfOut& out);

// Persistence of the announced internal IDs (comma-separated), config.cpp.
void loadHaRfIds(char* out, size_t len);
void saveHaRfIds(const char* ids);
constexpr size_t HA_RF_IDS_LEN = 32 * 9 + 1;   // 32 radio IDs "XXXXXXXX," of an earlier build + terminator
// (32 internal IDs "L3F9A2C," need 256)

// The Home Assistant device name of r (its name, or "<model> <custom ID>"), for tests
struct RfSensorReading;
void haRfDeviceNameForTest(char* out, size_t len, const RfSensorReading& r);
