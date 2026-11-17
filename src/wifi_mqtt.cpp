#include "wifi_mqtt.h"
#include "globals.h"
#include "params.h"
#include "board_io.h"
#include "sources.h"
#include "json_payload.h"
#include "uv_calibration.h"
#include "units.h"
#include "generated_version.h"
#include "serial_log.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <ping/ping_sock.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <memory>
#include <new>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

enum class WifiState { Idle, Connecting, Up, Backoff, Testing };

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;
constexpr unsigned long WIFI_BACKOFF_MS         = 30000;
constexpr unsigned long WIFI_DROP_BACKOFF_MS    = 5000;
// MQTT reconnect: 15 s after a failure, doubling up to 5 min while the
// broker stays unreachable (each attempt can block loop() for a few seconds),
// back to 15 s after a successful connection.
constexpr unsigned long MQTT_RETRY_MIN_MS       = 15000;
constexpr unsigned long MQTT_RETRY_MAX_MS       = 300000;

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
constexpr uint32_t MQTT_CONNECT_TIMEOUT_S   = 3;
constexpr uint32_t MQTT_HANDSHAKE_TIMEOUT_S = 5;
constexpr uint16_t MQTT_SOCKET_TIMEOUT_S    = 4;

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
unsigned long mqttRetryMs        = 0;   // 0 = no failed attempt yet (connect right away)

// WiFi change request (web task -> pollWifi). changeMux guards the strings.
portMUX_TYPE  changeMux          = portMUX_INITIALIZER_UNLOCKED;
volatile bool changePending      = false;
volatile bool changeTesting      = false;
bool          changeNoTest       = false;   // pre-provisioning: store directly
bool          changeDhcp         = false;   // test/store the new network with DHCP
unsigned long changeRequestedMs  = 0;
char          changeSsid[33]     = {};
char          changePass[65]     = {};
wifi_config_t prevConf           = {};      // driver config of the previous network (rollback)
char          changeResult[256]  = {};
constexpr unsigned long WIFI_CHANGE_DELAY_MS = 1500;   // let the settings response go out first
// After the test network connected: time for the reachability probe
// (gateway ping / DNS lookup) before the change counts as failed.
constexpr unsigned long WIFI_PROBE_TIMEOUT_MS = 10000;
bool          probeStarted       = false;
unsigned long probeStartedMs     = 0;
// Probe results, set from the ping task / lwIP thread. probeGen tags each
// test so a late callback of an earlier test is ignored.
volatile uint32_t probeGen       = 0;
volatile bool     probeOk        = false;
char              probeHost[64]  = {};

char topicData[96]  = {};
char topicCmd[96]   = {};
char topicAvail[96] = {};

// '+' and '#' are MQTT wildcards and not allowed in a publish topic (the
// broker drops the connection) - replaced by '_'. Everything else (also
// spaces and '/') is valid in topics and kept, so existing topics stay.
const char* topicPrefix()
{
    static char prefix[sizeof(gCfg.mqttTopic)];
    const char* src = gCfg.mqttTopic[0]  != '\0' ? gCfg.mqttTopic
                    : gCfg.deviceName[0] != '\0' ? gCfg.deviceName
                    : "weathernode";
    strlcpy(prefix, src, sizeof(prefix));
    for (char* c = prefix; *c; c++) if (*c == '+' || *c == '#') *c = '_';
    return prefix;
}

void buildTopics()
{
    snprintf(topicData,  sizeof(topicData),  "%s/data",   topicPrefix());
    snprintf(topicCmd,   sizeof(topicCmd),   "%s/cmd",    topicPrefix());
    snprintf(topicAvail, sizeof(topicAvail), "%s/status", topicPrefix());
}

// MQTT client id and Home Assistant node_id/unique_id prefix: the device
// name restricted to [A-Za-z0-9_-] (mqttSafeId), else "weathernode-" plus
// the full 48-bit MAC (unique per board; the lower 32 bits alone are the
// shared Espressif OUI + one byte).
const char* mqttClientId()
{
    static char id[40];
    if (gCfg.deviceName[0] != '\0') {
        mqttSafeId(gCfg.deviceName, id, sizeof(id));
    } else {
        uint64_t mac = ESP.getEfuseMac();   // byte 0 of the MAC in the lowest bits
        snprintf(id, sizeof(id), "weathernode-%02X%02X%02X%02X%02X%02X",
                 (unsigned)(mac & 0xFF), (unsigned)((mac >> 8) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
                 (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
    }
    return id;
}

// Publishes one Home Assistant MQTT-discovery config (retained) for a sensor
// entity that reads `valueTemplate` out of the shared telemetry JSON on
// topicData. All entities share one HA "device" so they group together.
void publishDiscoveryEntity(const char* key, const char* name, const char* unit,
                             const char* deviceClass, const char* valueTemplate, bool binary = false)
{
    char configTopic[160];
    snprintf(configTopic, sizeof(configTopic), "%s/%s/%s/%s/config",
             HA_DISCOVERY_PREFIX, binary ? "binary_sensor" : "sensor", mqttClientId(), key);

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
    if (binary) { doc["payload_on"] = "ON"; doc["payload_off"] = "OFF"; }
    if (deviceClass && deviceClass[0]) doc["device_class"] = deviceClass;

    JsonObject device = doc["device"].to<JsonObject>();
    JsonArray  ids     = device["identifiers"].to<JsonArray>();
    ids.add(mqttClientId());
    device["name"]         = gCfg.deviceName[0] != '\0' ? gCfg.deviceName : mqttClientId();
    device["manufacturer"] = "DIY";
    device["model"]        = "Universal Weather Node";
    device["sw_version"]   = FIRMWARE_VERSION;

    // measured first: serializeJson() into a too small buffer silently cuts
    // the JSON off, and HA would reject the truncated config.
    char payload[1024];
    size_t len = measureJson(doc);
    size_t packetSize = MQTT_MAX_HEADER_SIZE + 2 + strlen(configTopic) + len;
    if (doc.overflowed() || len >= sizeof(payload) ||
        (packetSize > mqttClient.getBufferSize() && !mqttClient.setBufferSize((uint16_t)packetSize))) {
        dbgSerial.printf("MQTT discovery '%s': config too large (%u bytes), skipped\n", key, (unsigned)len);
        return;
    }
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

    // Discovery runs once per connection, before wireless sensors may have
    // been received: with wireless sensors on, their groups are always
    // announced (values missing in the JSON just show as unavailable).
    const bool rf = gCfg.rfEn != 0;
    if (gCfg.windEn || rf) {
        publishDiscoveryEntity("wind_dir", "Wind Direction", "\xC2\xB0", nullptr,
                                "{{ value_json.windDirDeg | default(None) }}");
        publishDiscoveryEntity("wind_speed", "Wind Speed", windUnit, nullptr,
                                "{{ value_json.windSpeed | default(None) }}");
        publishDiscoveryEntity("wind_gust", "Wind Gust", windUnit, nullptr,
                                "{{ value_json.windGust | default(None) }}");
    }
    if (gCfg.rainEn || rf) {
        publishDiscoveryEntity("rain_rate", "Rain Rate", rainRateUnit, nullptr,
                                "{{ value_json.rainRate | default(None) }}");
        publishDiscoveryEntity("rain_mm", "Rain (cycle)", rainUnit, nullptr,
                                "{{ value_json.rain | default(None) }}");
        publishDiscoveryEntity("rain_since_start", "Rain Since Start", rainUnit, nullptr,
                                "{{ value_json.rainSinceStart | default(None) }}");
    }
    if (bme280Present || rf) {
        publishDiscoveryEntity("temp", "Temperature", tempUnit, "temperature",
                                "{{ value_json.temp | default(None) }}");
        publishDiscoveryEntity("humidity", "Humidity", "%", "humidity",
                                "{{ value_json.humidityPct | default(None) }}");
    }
    if (rf) {
        publishDiscoveryEntity("light", "Illuminance", "lx", "illuminance",
                                "{{ value_json.lightLux | default(None) }}");
        publishDiscoveryEntity("uv_index", "UV Index", "index", nullptr,
                                "{{ value_json.uvIndex | default(None) }}");
    }
    if (bme280Present) {
        publishDiscoveryEntity("pressure_abs", "Pressure (absolute)", pressureUnit, "pressure",
                                "{{ value_json.pressureAbs | default(None) }}");
        publishDiscoveryEntity("pressure_qnh", "Pressure (QNH)", pressureUnit, "pressure",
                                "{{ value_json.pressureQnh | default(None) }}");
        publishDiscoveryEntity("pressure_trend", "Pressure Change (3 h)", pressureUnit, nullptr,
                                "{{ value_json.pressureTrend3h | default(None) }}");
        publishDiscoveryEntity("pressure_tendency", "Pressure Tendency", nullptr, nullptr,
                                "{{ value_json.pressureTendency | default(None) }}");
        publishDiscoveryEntity("weather_trend", "Weather Trend", nullptr, nullptr,
                                "{{ value_json.weatherTrend | default(None) }}");
    }
    // Derived values (derived.h)
    if (bme280Present || rf) {
        publishDiscoveryEntity("dew_point", "Dew Point", tempUnit, "temperature",
                                "{{ value_json.dewPoint | default(None) }}");
        publishDiscoveryEntity("feels_like", "Feels Like", tempUnit, "temperature",
                                "{{ value_json.feelsLike | default(None) }}");
        publishDiscoveryEntity("temp_trend", "Temperature Change (1 h)", gCfg.unitSystem == 1 ? "\xC2\xB0" "F" : "K", nullptr,
                                "{{ value_json.tempTrend1h | default(None) }}");
        publishDiscoveryEntity("cloud_base", "Cloud Base", gCfg.unitSystem == 1 ? "ft" : "m", "distance",
                                "{{ value_json.cloudBase | default(None) }}");
        publishDiscoveryEntity("warn_frost", "Frost", nullptr, "cold",
                                "{{ 'ON' if value_json.warnings is defined and value_json.warnings.frost else 'OFF' }}", true);
        publishDiscoveryEntity("warn_fog", "Fog / Dew", nullptr, "moisture",
                                "{{ 'ON' if value_json.warnings is defined and value_json.warnings.fog else 'OFF' }}", true);
        publishDiscoveryEntity("warn_sultry", "Sultry", nullptr, "heat",
                                "{{ 'ON' if value_json.warnings is defined and value_json.warnings.sultry else 'OFF' }}", true);
    }
    if (gCfg.windEn || rf)
        publishDiscoveryEntity("wind_dir_text", "Wind Direction (compass)", nullptr, nullptr,
                                "{{ value_json.windDirText | default(None) }}");
    if (gCfg.rainEn || rf) {
        publishDiscoveryEntity("rain_1h", "Rain Last Hour", rainUnit, "precipitation",
                                "{{ value_json.rain1h | default(None) }}");
        publishDiscoveryEntity("rain_24h", "Rain Last 24 h", rainUnit, "precipitation",
                                "{{ value_json.rain24h | default(None) }}");
        publishDiscoveryEntity("rain_today", "Rain Today", rainUnit, "precipitation",
                                "{{ value_json.rainToday | default(None) }}");
        publishDiscoveryEntity("rain_yesterday", "Rain Yesterday", rainUnit, "precipitation",
                                "{{ value_json.rainYesterday | default(None) }}");
        publishDiscoveryEntity("last_rain", "Last Rain", nullptr, "timestamp",
                                "{{ value_json.lastRain | default(None) }}");
    }
    if (rf) {
        publishDiscoveryEntity("sunshine_today", "Sunshine Today", "min", "duration",
                                "{{ value_json.sunshineTodayMin | default(None) }}");
        publishDiscoveryEntity("sunshine_yesterday", "Sunshine Yesterday", "min", "duration",
                                "{{ value_json.sunshineYesterdayMin | default(None) }}");
    }
    if (gCfg.updateCheckEn)
        publishDiscoveryEntity("firmware_update", "Firmware Update", nullptr, nullptr,
                                "{{ value_json.firmwareUpdate | default('none') }}");
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
    // PCB v2.0 I/O block
    if (ioV2()) {
        if (ioUvActive()) {
            publishDiscoveryEntity("uv_sensor_volts", "UV Sensor Voltage", "V", "voltage",
                                    "{{ value_json.uvSensorVolts | default(None) }}");
            if (!rf && gCfg.uvIndexEn == 1 &&
                uvCalibrationValid(gCfg.uvCalibrated == 1, gCfg.uvIndexSlope, gCfg.uvIndexOffset))
                publishDiscoveryEntity("uv_index", "UV Index", "index", nullptr,
                                        "{{ value_json.uvIndex | default(None) }}");
        }
        if (gCfg.wind2Mode) {
            publishDiscoveryEntity("wind2_speed", "Wind 2 Speed", windUnit, nullptr,
                                    "{{ value_json.wind2.speed | default(None) }}");
            publishDiscoveryEntity("wind2_gust", "Wind 2 Gust", windUnit, nullptr,
                                    "{{ value_json.wind2.gust | default(None) }}");
            if (ioWind2DirActive())
                publishDiscoveryEntity("wind2_dir", "Wind 2 Direction", "\xC2\xB0", nullptr,
                                        "{{ value_json.wind2.dirDeg | default(None) }}");
        }
        if (ioAuxAinActive())
            publishDiscoveryEntity("aux_analog", "AUX Analog Input", "V", "voltage",
                                    "{{ value_json.auxAnalogV | default(None) }}");
        if (gCfg.auxPulseEn && !gCfg.wind2Mode) {
            publishDiscoveryEntity("aux_pulses", "AUX Pulses (cycle)", nullptr, nullptr,
                                    "{{ value_json.auxPulses | default(None) }}");
            publishDiscoveryEntity("aux_pulses_total", "AUX Pulses Total", nullptr, nullptr,
                                    "{{ value_json.auxPulsesTotal | default(None) }}");
        }
        if (gCfg.heaterMode)
            publishDiscoveryEntity("heater_duty", "Heater", "%", nullptr,
                                    "{{ value_json.heater.dutyPct | default(None) }}");
        if (gCfg.fanMode)
            publishDiscoveryEntity("fan_duty", "Fan", "%", nullptr,
                                    "{{ value_json.fan.dutyPct | default(None) }}");
        if (gCfg.tachoEn)
            publishDiscoveryEntity("fan_rpm", "Fan Speed", "rpm", nullptr,
                                    "{{ value_json.fan.rpm | default(None) }}");
        if (gCfg.veDirectEn) {
            publishDiscoveryEntity("victron_battery_v", "Victron Battery Voltage", "V", "voltage",
                                    "{{ value_json.victron.batteryV | default(None) }}");
            publishDiscoveryEntity("victron_battery_a", "Victron Battery Current", "A", "current",
                                    "{{ value_json.victron.batteryA | default(None) }}");
            publishDiscoveryEntity("victron_pv_w", "Victron PV Power", "W", "power",
                                    "{{ value_json.victron.pvW | default(None) }}");
            publishDiscoveryEntity("victron_yield_today", "Victron Yield Today", "kWh", "energy",
                                    "{{ value_json.victron.yieldTodayKWh | default(None) }}");
            publishDiscoveryEntity("victron_soc", "Victron State of Charge", "%", "battery",
                                    "{{ value_json.victron.socPct | default(None) }}");
            publishDiscoveryEntity("victron_load_a", "Victron Load Current", "A", "current",
                                    "{{ value_json.victron.loadA | default(None) }}");
        }
    }
    publishDiscoveryEntity("cpu_temp", "CPU Temperature", tempUnit, "temperature",
                            "{{ value_json.cpuTemp | default(None) }}");
    publishDiscoveryEntity("reset_reason", "Last Restart", nullptr, nullptr,
                            "{{ value_json.resetReason | default(None) }}");
    if (gCfg.debugMode) {
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
//   {"cmd":"resetRain"|"resetAuxPulses"|"restart"|"debugOn"|"debugOff"|"portal"|"factoryReset"}
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
        else if (!strcmp(cmd, "resetAuxPulses")) cmdResetAuxPulses();
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

// Station mode, hostname and (optional) static IP before every WiFi.begin().
// forceDhcp: ignore the configured static IP (a WiFi change to another
// network - the static address belongs to the current one) and switch the
// DHCP client back on if a static address was set before.
bool prepareSta(bool forceDhcp = false)
{
    WiFi.mode(WIFI_STA);
    if (gCfg.deviceName[0] != '\0') WiFi.setHostname(gCfg.deviceName);
    bool useStaticIp = gCfg.staticIpEn && gCfg.staticIp[0] != '\0';
    if (useStaticIp && forceDhcp) {
        const IPAddress none(0, 0, 0, 0);   // not INADDR_NONE: lwIP defines that as 255.255.255.255
        WiFi.config(none, none, none);      // 0.0.0.0 = DHCP client on
        return false;
    }
    if (useStaticIp) {
        IPAddress ip, gw, sn, dns;
        ip.fromString(gCfg.staticIp);
        gw.fromString(gCfg.staticGateway);
        sn.fromString(gCfg.staticSubnet);
        if (gCfg.staticDns[0] != '\0') dns.fromString(gCfg.staticDns);
        else                           dns = gw;
        WiFi.config(ip, gw, sn, dns);
    }
    return useStaticIp;
}

void setChangeResult(const char* fmt, const char* a, const char* b = "")
{
    char r[sizeof(changeResult)];
    snprintf(r, sizeof(r), fmt, a, b);
    portENTER_CRITICAL(&changeMux);
    memcpy(changeResult, r, sizeof(r));
    portEXIT_CRITICAL(&changeMux);
}

// prevConf's SSID as a C string (the driver field has no terminator at 32 chars).
void prevSsidText(char (&out)[33])
{
    memcpy(out, prevConf.sta.ssid, 32);
    out[32] = '\0';
}

// Puts the previous network's driver config back - in RAM and flash. Needed
// on every rollback: WiFi.begin() without arguments re-applies whatever
// config is active, which would otherwise persist the tested network.
void restorePrevWifiConfig()
{
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_config(WIFI_IF_STA, &prevConf);
}

// --- Reachability probe of the tested network: ping the gateway (3 echo
// requests) and look up the NTP server name in parallel; either answer
// counts. Both run asynchronously (ping task / lwIP thread), pollWifi()
// only checks probeOk, so loop() never blocks on it. ---
void onProbePingOk(esp_ping_handle_t, void* gen)
{
    if ((uint32_t)(uintptr_t)gen == probeGen) probeOk = true;
}

void onProbePingEnd(esp_ping_handle_t hdl, void*)
{
    esp_ping_delete_session(hdl);
}

void onProbeDnsFound(const char*, const ip_addr_t* addr, void* gen)
{
    if (addr && (uint32_t)(uintptr_t)gen == probeGen) probeOk = true;
}

void probeDnsLookup(void* gen)   // runs in the lwIP thread (tcpip_callback)
{
    ip_addr_t addr;
    // ERR_OK = answered from the cache (possibly from the previous network),
    // proves nothing - only an answer from the network (callback) counts.
    dns_gethostbyname(probeHost, &addr, onProbeDnsFound, gen);
}

void startProbe()
{
    uint32_t gen = probeGen;
    probeOk = false;

    IPAddress gw = WiFi.gatewayIP();
    if ((uint32_t)gw != 0) {
        esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
        cfg.count       = 3;
        cfg.interval_ms = 500;
        cfg.timeout_ms  = 1000;
        IP_ADDR4(&cfg.target_addr, gw[0], gw[1], gw[2], gw[3]);
        esp_ping_callbacks_t cbs = {};
        cbs.cb_args         = (void*)(uintptr_t)gen;
        cbs.on_ping_success = onProbePingOk;
        cbs.on_ping_end     = onProbePingEnd;
        esp_ping_handle_t hdl;
        if (esp_ping_new_session(&cfg, &cbs, &hdl) == ESP_OK && esp_ping_start(hdl) != ESP_OK)
            esp_ping_delete_session(hdl);
    }
    strlcpy(probeHost, gCfg.ntpServer[0] ? gCfg.ntpServer : "pool.ntp.org", sizeof(probeHost));
    tcpip_callback(probeDnsLookup, (void*)(uintptr_t)gen);
}

// Starts testing the requested network: the driver config goes to RAM only,
// so the stored (flash) credentials stay those of the previous network.
void startWifiTest(unsigned long now)
{
    char ssid[33], pass[65];
    portENTER_CRITICAL(&changeMux);
    memcpy(ssid, changeSsid, sizeof(ssid));
    memcpy(pass, changePass, sizeof(pass));
    bool noTest   = changeNoTest;
    bool dhcp     = changeDhcp;
    changePending = false;
    changeTesting = !noTest;
    portEXIT_CRITICAL(&changeMux);

    if (noTest) {
        // Pre-provisioning: store right away (flash) and switch; the normal
        // connect/backoff loop keeps trying until the network is in range.
        if (mqttClient.connected()) mqttClient.disconnect();
        WiFi.disconnect();
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        prepareSta(dhcp);
        WiFi.begin(ssid, pass[0] ? pass : nullptr);
        dbgSerial.printf("WiFi: stored '%s' without a test (pre-provisioning), connecting...\n", ssid);
        setChangeResult("Stored the WiFi network \u201c%s\u201d without a test%s.", ssid, "");
        wifiState     = WifiState::Connecting;
        wifiStateAtMs = now;
        return;
    }

    // The previous network straight from the driver (= flash, nothing is
    // in RAM-only mode here): WiFi.SSID() is empty while not associated,
    // and a rollback to an empty config could never connect again.
    if (esp_wifi_get_config(WIFI_IF_STA, &prevConf) != ESP_OK || prevConf.sta.ssid[0] == 0) {
        dbgSerial.println("WiFi: change aborted - could not read the current network's config");
        setChangeResult("Could not start the WiFi change to \u201c%s\u201d: the current network's settings could "
                        "not be read, so there would be nothing to return to. Nothing was changed%s.", ssid, "");
        changeTesting = false;
        return;
    }

    if (mqttClient.connected()) mqttClient.disconnect();
    WiFi.disconnect();
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    prepareSta(dhcp);
    WiFi.begin(ssid, pass[0] ? pass : nullptr);
    char prevSsid[33];
    prevSsidText(prevSsid);
    dbgSerial.printf("WiFi: testing new network '%s' for %lus (previous: '%s')\n",
                     ssid, WIFI_CHANGE_TEST_MS / 1000, prevSsid);
    probeGen      = probeGen + 1;
    probeStarted  = false;
    probeOk       = false;
    wifiState     = WifiState::Testing;
    wifiStateAtMs = now;
}

// ok: connected and reachable. reason: why not (log/result text).
void finishWifiTest(bool ok, unsigned long now, const char* reason = nullptr)
{
    char ssid[33];
    portENTER_CRITICAL(&changeMux);
    memcpy(ssid, changeSsid, sizeof(ssid));
    portEXIT_CRITICAL(&changeMux);
    probeGen = probeGen + 1;   // ignore late probe answers

    if (ok) {
        // Store the working config (flash) - it is the one in use right now.
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        wifi_config_t conf;
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) esp_wifi_set_config(WIFI_IF_STA, &conf);
        dbgSerial.printf("WiFi: connected to '%s', IP %s - stored\n", ssid, WiFi.localIP().toString().c_str());
        setChangeResult("Switched to the WiFi network \u201c%s\u201d (IP %s).", ssid, WiFi.localIP().toString().c_str());
        wifiState = WifiState::Up;
    } else {
        char prevSsid[33];
        prevSsidText(prevSsid);
        WiFi.disconnect();
        restorePrevWifiConfig();
        prepareSta();
        WiFi.begin();
        dbgSerial.printf("WiFi: '%s' %s - back to '%s'\n", ssid, reason, prevSsid);
        char msg[sizeof(changeResult)];
        snprintf(msg, sizeof(msg), "Could not switch to \u201c%%s\u201d: %s \u2014 back on the previous network \u201c%%s\u201d.", reason);
        setChangeResult(msg, ssid, prevSsid);
        wifiState     = WifiState::Connecting;
        wifiStateAtMs = now;
    }
    changeTesting = false;
}

void pollWifi(unsigned long now)
{
    if (!gCfg.wifiEn) {
        changePending = false;
        if (changeTesting) {
            // test cut short: the tested network must not end up in flash
            probeGen = probeGen + 1;
            restorePrevWifiConfig();
            changeTesting = false;
        }
        if (wifiState != WifiState::Idle) {
            if (mqttClient.connected()) mqttClient.disconnect();
            WiFi.disconnect(true);
            wifiState = WifiState::Idle;
        }
        return;
    }

    if (changePending && wifiState != WifiState::Testing && now - changeRequestedMs >= WIFI_CHANGE_DELAY_MS)
        startWifiTest(now);

    switch (wifiState) {
        case WifiState::Idle: {
            bool useStaticIp = prepareSta();
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
            if ((long)(now - wifiBackoffUntilMs) >= 0) wifiState = WifiState::Idle;   // wrap-safe
            break;

        case WifiState::Testing:
            if (probeOk) {
                finishWifiTest(true, now);
            } else if (now - wifiStateAtMs > WIFI_CHANGE_TEST_MS) {
                finishWifiTest(false, now, probeStarted
                    ? "connected, but neither the gateway nor DNS answered (wrong network settings or no internet?)"
                    : "not reachable within 60 s (check the network name and password)");
            } else if (WiFi.status() == WL_CONNECTED) {
                if (!probeStarted) {
                    dbgSerial.printf("WiFi: test network connected, IP %s - checking gateway/DNS\n",
                                     WiFi.localIP().toString().c_str());
                    probeStarted   = true;
                    probeStartedMs = now;
                    startProbe();
                } else if (now - probeStartedMs > WIFI_PROBE_TIMEOUT_MS) {
                    finishWifiTest(false, now,
                        "connected, but neither the gateway nor DNS answered (wrong network settings or no internet?)");
                }
            }
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

    if (mqttRetryMs && now - lastMqttAttemptMs < mqttRetryMs) return;
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
        mqttRetryMs = 0;
        dbgSerial.printf("MQTT connected as '%s', topic prefix '%s'\n", mqttClientId(), topicPrefix());
        mqttClient.publish(topicAvail, AVAIL_ONLINE, /*retain=*/true);
        if (gCfg.mqttCtrlEn) mqttClient.subscribe(topicCmd);
        publishHomeAssistantDiscovery();
    } else {
        mqttRetryMs = mqttRetryMs ? min(mqttRetryMs * 2, MQTT_RETRY_MAX_MS) : MQTT_RETRY_MIN_MS;
        dbgSerial.printf("MQTT connect failed, rc=%d (retry in %lus)\n",
                      mqttClient.state(), mqttRetryMs / 1000);
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

bool wifiRequestChange(const char* ssid, const char* pass, bool test, bool dhcp)
{
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl < 1 || sl > 32 || (pl != 0 && (pl < 8 || pl > 63))) return false;
    portENTER_CRITICAL(&changeMux);
    strlcpy(changeSsid, ssid, sizeof(changeSsid));
    strlcpy(changePass, pass, sizeof(changePass));
    changeNoTest      = !test;
    changeDhcp        = dhcp;
    changeResult[0]   = '\0';
    changeRequestedMs = millis();
    changePending     = true;
    portEXIT_CRITICAL(&changeMux);
    return true;
}

bool wifiChangeBusy() { return changePending || changeTesting; }

String wifiChangeResult()
{
    char r[sizeof(changeResult)];
    portENTER_CRITICAL(&changeMux);
    memcpy(r, changeResult, sizeof(r));
    portEXIT_CRITICAL(&changeMux);
    return String(r);
}

String wifiCurrentPassword()
{
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return String();
    char pass[sizeof(conf.sta.password) + 1];
    memcpy(pass, conf.sta.password, sizeof(conf.sta.password));
    pass[sizeof(conf.sta.password)] = '\0';   // a 64-char PSK has no terminator
    return String(pass);
}
bool mqttIsConnected() { return gCfg.mqttEn && mqttClient.connected(); }

void wifiMqttPoll()
{
    const unsigned long now = millis();
    pollWifi(now);
    pollMqtt(now);
}

void mqttPublishTelemetry(const PathValues* pv)
{
    if (!mqttIsConnected()) return;

    JsonDocument doc;
    buildJsonPayload(doc, pv);
    size_t n = measureJson(doc);
    size_t packetSize = MQTT_MAX_HEADER_SIZE + 2 + strlen(topicData) + n;
    if (doc.overflowed() || packetSize > UINT16_MAX ||
        (packetSize > mqttClient.getBufferSize() && !mqttClient.setBufferSize((uint16_t)packetSize))) {
        dbgSerial.println("MQTT telemetry: JSON/packet allocation failed or packet too large; not published.");
        return;
    }
    std::unique_ptr<char[]> buf(new (std::nothrow) char[n + 1]);
    if (!buf || serializeJson(doc, buf.get(), n + 1) != n) {
        dbgSerial.println("MQTT telemetry: serialization/allocation failed; not published.");
        return;
    }
    bool ok = mqttClient.publish(topicData, (const uint8_t*)buf.get(), n, false);
    dbgSerial.printf("MQTT publish to '%s': %s\n", topicData, ok ? "OK" : "FAILED");
}
