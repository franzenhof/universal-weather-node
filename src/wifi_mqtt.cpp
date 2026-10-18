#include "wifi_mqtt.h"
#include "globals.h"
#include "params.h"
#include "json_payload.h"
#include "units.h"
#include "generated_version.h"
#include "serial_log.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

enum class WifiState { Idle, Connecting, Up, Backoff };

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;
constexpr unsigned long WIFI_BACKOFF_MS         = 30000;
constexpr unsigned long WIFI_DROP_BACKOFF_MS    = 5000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS  = 15000;

// WiFiClientSecure/PubSubClient default to a 30 s TCP connect timeout, a
// 120 s TLS handshake timeout, and a 15 s MQTT CONNACK wait respectively —
// fine individually, but stacked together a truly unreachable/blackholed
// broker (as opposed to one that actively refuses the connection, which
// fails fast) could block this whole function — and therefore loop() —
// for minutes, closer to the 120 s watchdog timeout than is comfortable.
// Tightened here so a hanging broker can't stall sampling/display/LoRaWAN
// for anywhere near that long. Applied to both clients (plain WiFiClient's
// own default connect timeout is already a short 3 s, but set explicitly
// here too so it doesn't silently drift if that default ever changes).
constexpr uint32_t MQTT_CONNECT_TIMEOUT_S   = 5;
constexpr uint32_t MQTT_HANDSHAKE_TIMEOUT_S = 5;
constexpr uint16_t MQTT_SOCKET_TIMEOUT_S    = 5;

constexpr const char* HA_DISCOVERY_PREFIX = "homeassistant";
constexpr const char* AVAIL_ONLINE        = "online";
constexpr const char* AVAIL_OFFLINE       = "offline";

WiFiClient       plainClient;   // MQTT_TLS_NONE
WiFiClientSecure secureClient;  // MQTT_TLS_INSECURE / MQTT_TLS_VERIFY
PubSubClient     mqttClient(secureClient);

WifiState     wifiState          = WifiState::Idle;
unsigned long wifiStateAtMs      = 0;
unsigned long wifiBackoffUntilMs = 0;
unsigned long lastMqttAttemptMs  = 0;

char topicData[96]  = {};
char topicCmd[96]   = {};
char topicAvail[96] = {};

const char* topicPrefix()
{
    if (gCfg.mqttTopic[0]  != '\0') return gCfg.mqttTopic;
    if (gCfg.deviceName[0] != '\0') return gCfg.deviceName;
    return "weathernode";
}

void buildTopics()
{
    snprintf(topicData,  sizeof(topicData),  "%s/data",   topicPrefix());
    snprintf(topicCmd,   sizeof(topicCmd),   "%s/cmd",    topicPrefix());
    snprintf(topicAvail, sizeof(topicAvail), "%s/status", topicPrefix());
}

const char* mqttClientId()
{
    static char id[40];
    if (gCfg.deviceName[0] != '\0') {
        strlcpy(id, gCfg.deviceName, sizeof(id));
    } else {
        snprintf(id, sizeof(id), "weathernode-%08X", (uint32_t)ESP.getEfuseMac());
    }
    return id;
}

// Publishes one Home Assistant MQTT-discovery config (retained) for a sensor
// entity that reads `valueTemplate` out of the shared telemetry JSON on
// topicData. All entities share one HA "device" so they group together.
void publishDiscoveryEntity(const char* key, const char* name, const char* unit,
                             const char* deviceClass, const char* valueTemplate)
{
    char configTopic[160];
    snprintf(configTopic, sizeof(configTopic), "%s/sensor/%s/%s/config",
             HA_DISCOVERY_PREFIX, mqttClientId(), key);

    char uniqueId[80];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", mqttClientId(), key);

    JsonDocument doc;
    doc["name"]                 = name;
    doc["unique_id"]            = uniqueId;
    doc["state_topic"]          = topicData;
    doc["value_template"]       = valueTemplate;
    doc["availability_topic"]   = topicAvail;
    doc["payload_available"]    = AVAIL_ONLINE;
    doc["payload_not_available"] = AVAIL_OFFLINE;
    if (unit && unit[0])        doc["unit_of_measurement"] = unit;
    if (deviceClass && deviceClass[0]) doc["device_class"] = deviceClass;

    JsonObject device = doc["device"].to<JsonObject>();
    JsonArray  ids     = device["identifiers"].to<JsonArray>();
    ids.add(mqttClientId());
    device["name"]         = gCfg.deviceName[0] != '\0' ? gCfg.deviceName : mqttClientId();
    device["manufacturer"] = "DIY";
    device["model"]        = "LoRaWAN Weather Node";
    device["sw_version"]   = FIRMWARE_VERSION;

    char payload[768];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(configTopic, (const uint8_t*)payload, n, /*retain=*/true);
}

// Publishes discovery configs for every entity whose backing sensor is
// currently enabled, so HA doesn't show entities for hardware that isn't
// present. Safe/idempotent to call again on every reconnect (retained).
void publishHomeAssistantDiscovery()
{
    // HA's unit_of_measurement wants the degree sign for temperature; the
    // JSON payload's own unit fields (tempUnit etc., see json_payload.cpp)
    // stay plain ASCII ("C"/"F") since they're just machine-readable labels.
    const char* tempUnit = (gCfg.unitSystem == 1) ? "\xC2\xB0" "F" : "\xC2\xB0" "C";
    const char* windUnit = windSpeedUnitLabel(gCfg);
    const char* rainUnit = rainUnitLabel(gCfg);
    const char* rainRateUnit = rainRateUnitLabel(gCfg);
    const char* pressureUnit = pressureUnitLabel(gCfg);

    if (gCfg.windEn) {
        publishDiscoveryEntity("wind_dir", "Wind Direction", "\xC2\xB0", nullptr,
                                "{{ value_json.windDirDeg | default(None) }}");
        publishDiscoveryEntity("wind_speed", "Wind Speed", windUnit, nullptr,
                                "{{ value_json.windSpeed | default(None) }}");
        publishDiscoveryEntity("wind_gust", "Wind Gust", windUnit, nullptr,
                                "{{ value_json.windGust | default(None) }}");
    }
    if (gCfg.rainEn) {
        publishDiscoveryEntity("rain_rate", "Rain Rate", rainRateUnit, nullptr,
                                "{{ value_json.rainRate | default(None) }}");
        publishDiscoveryEntity("rain_mm", "Rain (cycle)", rainUnit, nullptr,
                                "{{ value_json.rain | default(None) }}");
        publishDiscoveryEntity("rain_since_start", "Rain Since Start", rainUnit, nullptr,
                                "{{ value_json.rainSinceStart | default(None) }}");
    }
    if (bme280Present) {
        publishDiscoveryEntity("temp", "Temperature", tempUnit, "temperature",
                                "{{ value_json.temp | default(None) }}");
        publishDiscoveryEntity("humidity", "Humidity", "%", "humidity",
                                "{{ value_json.humidityPct | default(None) }}");
        publishDiscoveryEntity("pressure_abs", "Pressure (absolute)", pressureUnit, "pressure",
                                "{{ value_json.pressureAbs | default(None) }}");
        publishDiscoveryEntity("pressure_qnh", "Pressure (QNH)", pressureUnit, "pressure",
                                "{{ value_json.pressureQnh | default(None) }}");
    }
    // DS18B20 probes are hot-pluggable and only auto-detected once per uplink
    // cycle, so all 3 slots are always published; an unconnected probe just
    // shows as unavailable in HA instead of flapping the entity list.
    // (3 matches MAX_SENSORS_PER_BUS in main.cpp.)
    constexpr int kMaxDs18b20 = 3;
    for (int i = 0; i < kMaxDs18b20; i++) {
        char key[16], name[24], tmpl[48];
        snprintf(key,  sizeof(key),  "ds18b20_%d", i);
        snprintf(name, sizeof(name), "Temperature Probe %d", i);
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.ds18b20[%d] | default(None) }}", i);
        publishDiscoveryEntity(key, name, tempUnit, "temperature", tmpl);
    }
    if (gCfg.lipoEn) {
        publishDiscoveryEntity("battery", "Battery Voltage", "V", "voltage",
                                "{{ value_json.batteryVolt | default(None) }}");
    }
    if (gCfg.debugMode) {
        publishDiscoveryEntity("cpu_temp", "CPU Temperature", tempUnit, "temperature",
                                "{{ value_json.cpuTemp | default(None) }}");
        publishDiscoveryEntity("free_heap", "Free Heap", "KiB", nullptr,
                                "{{ value_json.freeHeapKiB | default(None) }}");
        publishDiscoveryEntity("uptime", "Uptime", "h", nullptr,
                                "{{ value_json.uptimeH | default(None) }}");
        publishDiscoveryEntity("cycle", "Cycle Counter", nullptr, nullptr,
                                "{{ value_json.cycle | default(None) }}");
    }
    publishDiscoveryEntity("send_fail_count", "Send Fail Count", nullptr, nullptr,
                            "{{ value_json.status.sendFailCount | default(None) }}");
}

// Parses a command received on the MQTT command topic and dispatches it
// into the same transport-agnostic actions the LoRaWAN downlink uses
// (see params.h). Two accepted shapes:
//   {"cmd":"resetRain"|"restart"|"debugOn"|"debugOff"|"portal"|"factoryReset"}
//   {"params":[{"id":1,"value":300}, ...]}   (same paramId scheme as FPort1 0x10)
void handleCommandMessage(char* /*topic*/, byte* payload, unsigned int length)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        dbgSerial.printf("MQTT cmd: JSON parse error: %s\n", err.c_str());
        return;
    }

    if (doc["cmd"].is<const char*>()) {
        const char* cmd = doc["cmd"];
        if      (!strcmp(cmd, "resetRain"))    cmdResetRainAccumulator();
        else if (!strcmp(cmd, "restart"))      cmdRestart();
        else if (!strcmp(cmd, "debugOn"))      cmdSetDebugMode(true);
        else if (!strcmp(cmd, "debugOff"))     cmdSetDebugMode(false);
        else if (!strcmp(cmd, "portal"))       cmdRequestConfigPortal();
        else if (!strcmp(cmd, "factoryReset")) cmdFactoryReset();
        else dbgSerial.printf("MQTT cmd: unknown cmd '%s'\n", cmd);
        return;
    }

    bool anyApplied = false;
    for (JsonObject p : doc["params"].as<JsonArray>()) {
        uint8_t paramId = p["id"].as<uint8_t>();
        // as<long>() + clamp: as<int16_t>() returns 0 for out-of-range input,
        // which would be applied as a valid value for some parameters.
        int16_t value   = clampToParamValue(p["value"].as<long>());
        bool ok = applyParam(paramId, value);
        dbgSerial.printf("MQTT cmd: set param %u = %d -> %s\n", paramId, value, ok ? "OK" : "INVALID");
        anyApplied = anyApplied || ok;
    }
    if (anyApplied) saveConfig(gCfg);
}

void pollWifi(unsigned long now)
{
    if (!gCfg.wifiEn) {
        if (wifiState != WifiState::Idle) {
            if (mqttClient.connected()) mqttClient.disconnect();
            WiFi.disconnect(true);
            wifiState = WifiState::Idle;
        }
        return;
    }

    switch (wifiState) {
        case WifiState::Idle: {
            WiFi.mode(WIFI_STA);
            if (gCfg.deviceName[0] != '\0') WiFi.setHostname(gCfg.deviceName);
            bool useStaticIp = gCfg.staticIpEn && gCfg.staticIp[0] != '\0';
            if (useStaticIp) {
                IPAddress ip, gw, sn, dns;
                ip.fromString(gCfg.staticIp);
                gw.fromString(gCfg.staticGateway);
                sn.fromString(gCfg.staticSubnet);
                if (gCfg.staticDns[0] != '\0') dns.fromString(gCfg.staticDns);
                else                           dns = gw;
                WiFi.config(ip, gw, sn, dns);
            }
            WiFi.begin();
            dbgSerial.printf("WiFi: connecting (%s)...\n",
                              useStaticIp ? ("static IP " + String(gCfg.staticIp)).c_str() : "DHCP");
            wifiState     = WifiState::Connecting;
            wifiStateAtMs = now;
            break;
        }

        case WifiState::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                dbgSerial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
                wifiState = WifiState::Up;
            } else if (now - wifiStateAtMs > WIFI_CONNECT_TIMEOUT_MS) {
                dbgSerial.printf("WiFi: connect attempt timed out, backing off %lus\n", WIFI_BACKOFF_MS / 1000);
                wifiBackoffUntilMs = now + WIFI_BACKOFF_MS;
                wifiState = WifiState::Backoff;
            }
            break;

        case WifiState::Up:
            if (WiFi.status() != WL_CONNECTED) {
                dbgSerial.printf("WiFi connection lost, backing off %lus\n", WIFI_DROP_BACKOFF_MS / 1000);
                if (mqttClient.connected()) mqttClient.disconnect();
                wifiBackoffUntilMs = now + WIFI_DROP_BACKOFF_MS;
                wifiState = WifiState::Backoff;
            }
            break;

        case WifiState::Backoff:
            if (now >= wifiBackoffUntilMs) wifiState = WifiState::Idle;
            break;
    }
}

void pollMqtt(unsigned long now)
{
    if (!gCfg.wifiEn || !gCfg.mqttEn || wifiState != WifiState::Up) return;

    if (mqttClient.connected()) {
        mqttClient.loop();
        return;
    }

    if (now - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) return;
    lastMqttAttemptMs = now;

    switch (gCfg.mqttTlsMode) {
        case MQTT_TLS_VERIFY:
            if (gCfg.mqttCaCert[0] != '\0') {
                secureClient.setCACert(gCfg.mqttCaCert);
            } else {
                dbgSerial.println("MQTT: mqtttls=2 (verify) but no CA cert configured — falling back to insecure TLS.");
                secureClient.setInsecure();
            }
            mqttClient.setClient(secureClient);
            break;
        case MQTT_TLS_INSECURE:
            secureClient.setInsecure();
            mqttClient.setClient(secureClient);
            break;
        case MQTT_TLS_NONE:
        default:
            mqttClient.setClient(plainClient);
            break;
    }
    buildTopics();
    mqttClient.setServer(gCfg.mqttHost, (uint16_t)gCfg.mqttPort);

    // LWT: broker publishes "offline" (retained) on topicAvail if the
    // connection drops uncleanly, so HA shows entities unavailable instantly
    // instead of freezing on the last known value.
    const bool connected = (gCfg.mqttUser[0] != '\0')
        ? mqttClient.connect(mqttClientId(), gCfg.mqttUser, gCfg.mqttPass,
                              topicAvail, 0, true, AVAIL_OFFLINE)
        : mqttClient.connect(mqttClientId(),
                              topicAvail, 0, true, AVAIL_OFFLINE);

    if (connected) {
        dbgSerial.printf("MQTT connected as '%s', topic prefix '%s'\n", mqttClientId(), topicPrefix());
        mqttClient.publish(topicAvail, AVAIL_ONLINE, /*retain=*/true);
        if (gCfg.mqttCtrlEn) mqttClient.subscribe(topicCmd);
        publishHomeAssistantDiscovery();
    } else {
        dbgSerial.printf("MQTT connect failed, rc=%d (retry in %lus)\n",
                      mqttClient.state(), MQTT_RETRY_INTERVAL_MS / 1000);
    }
}

} // namespace

void wifiMqttSetup()
{
    mqttClient.setCallback(handleCommandMessage);
    plainClient.setTimeout(MQTT_CONNECT_TIMEOUT_S);
    secureClient.setTimeout(MQTT_CONNECT_TIMEOUT_S);
    secureClient.setHandshakeTimeout(MQTT_HANDSHAKE_TIMEOUT_S);
    mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
}

bool wifiIsConnected() { return wifiState == WifiState::Up; }
bool mqttIsConnected() { return gCfg.mqttEn && mqttClient.connected(); }

void wifiMqttPoll()
{
    const unsigned long now = millis();
    pollWifi(now);
    pollMqtt(now);
}

void mqttPublishTelemetry()
{
    if (!mqttIsConnected()) return;

    JsonDocument doc;
    buildJsonPayload(doc);
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    bool ok = mqttClient.publish(topicData, (const uint8_t*)buf, n, false);
    dbgSerial.printf("MQTT publish to '%s': %s\n", topicData, ok ? "OK" : "FAILED");
}
