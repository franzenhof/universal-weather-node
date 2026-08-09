#pragma once

#include <Arduino.h>
#include <stdint.h>

// MQTT transport security. Standard broker ports: 1883 = plain (no TLS),
// 8883 = TLS — but the broker's actual configuration is what matters, not
// the port number itself.
enum MqttTlsMode {
    MQTT_TLS_NONE     = 0,  // plain TCP, no TLS at all — required for brokers
                            // listening without TLS (classic port 1883)
    MQTT_TLS_INSECURE = 1,  // TLS, but the server certificate is not verified
                            // (e.g. self-signed, no CA configured) — encrypted,
                            // not authenticated
    MQTT_TLS_VERIFY   = 2,  // TLS, server certificate verified against mqttCaCert
};

// Custom PCB hardware revision, stored in Config::pcbVersion below — see
// README.md "PCB Revisions" for the full pin-mapping differences between
// them (getPins() in sensors.cpp, plus the Schmitt-trigger-related PCNT edge
// polarity in configureWindPcnt()/configureRainPcnt()).
constexpr int PCB_REV_1_2 = 12;
constexpr int PCB_REV_1_3 = 13;

// ============================================================================
// CONFIGURATION (persistent via NVS / Preferences)
// ============================================================================
struct Config {
    // Set once the config portal has been completed the first time. Used
    // instead of devEui==0 to detect "needs first-time setup", since a
    // WiFi/MQTT-only node (loraEn=0) may never set a non-zero DevEUI.
    int      provisioned        = 0;

    uint64_t joinEui           = 0;
    uint64_t devEui            = 0;
    uint8_t  appKey[16]        = {};
    char     region[8]         = "EU868"; // LoRaWAN region (default)
    int      pcbVersion        = PCB_REV_1_3;
    int      altitudeM         = 560; // default altitude above sea level (m)
    // volatile: written by applyDownlinkParam() (params.cpp), which now runs
    // from either the main loop task (LoRaWAN downlink, MQTT command) or the
    // dedicated web-server task on core 0 (/api/settings POST, see
    // web_api.cpp) — volatile stops the compiler from caching a stale value
    // in a register across a loop iteration on one task while the other
    // writes it. A plain aligned int load/store is already atomic on this
    // hardware, so this is about cross-task *visibility*, not tearing; it
    // deliberately doesn't use std::atomic, since Config is copied by value
    // throughout (loadConfig() return, portal.cpp's `Config c = cur`), and
    // atomics aren't copyable.
    volatile int uplinkIntervalSec = 300;  // target uplink interval (s)
    volatile int sampleSec         = 3;    // sub-sample interval (s); <= uplinkIntervalSec
    volatile int windDirOffsetDeg  = 0;   // north offset (added to raw value, mod 360)
    volatile int windUnitMs        = 1;   // 1 = transmit wind in m/s, 0 = transmit in km/h
    volatile int windEn            = 1;   // 1 = Davis wind sensor enabled (direction + speed)
    volatile int rainEn            = 1;   // 1 = Davis rain sensor enabled
    volatile int lipoEn            = 0;   // 1 = LiPo measurement enabled (saves 4 LPP bytes when 0)
    volatile int debugMode         = 0;   // 1 = include debug telemetry in payload
    volatile int cpuFreqMhz        = 80;  // 80 or 240 MHz; below 80 not allowed (PCNT-APB)
    volatile int payloadFormat     = 0;   // 0 = CayenneLPP (FPort 1), 1 = compact binary (FPort 3, ~50% smaller)
    volatile int unitSystem        = 0;   // 0 = Metric (°C, mm, hPa, wind per windUnitMs), 1 = Imperial (°F, in, inHg, mph)

    // ------------------------------------------------------------------
    // Generic (non-Davis) sensor calibration
    // ------------------------------------------------------------------
    // Same cross-task write path as the volatile int fields above
    // (applyDownlinkParam() cases 12/13, params.cpp) — same rationale, just
    // float instead of int. Unlike those fields, neither of these is ever
    // passed into std::min/max/constrain anywhere in the codebase, so no
    // portal.cpp-style "can't deduce a common type" fixup was needed here.
    volatile float rainMmPerTip = 0.2f;      // mm of rain per reed-switch pulse (Davis default: 0.2mm)
    volatile float windMsPerPps = 1.00584f;  // wind speed (m/s) per pulse-per-second (Davis default)
    volatile int windDirSensorType = 0;     // 0 = continuous potentiometer (linear map), 1 = calibrated lookup table (e.g. reed-switch/resistor-ladder vanes)
    int      windDirCalCount   = 0;         // number of valid entries in the calibration table below (0 = not calibrated)
    uint16_t windDirCalAdc[16] = {};        // raw ADC reading captured per calibration position
    uint16_t windDirCalDeg[16] = {};        // corresponding direction in degrees per calibration position

    // ------------------------------------------------------------------
    // Parallel WiFi + MQTT transport (all independent, default disabled)
    // ------------------------------------------------------------------
    int      loraEn            = 1;   // 1 = LoRaWAN transport enabled
    int      wifiEn            = 0;   // 1 = WiFi STA enabled (master switch for mqttEn/webApiEn)
    int      mqttEn            = 0;   // 1 = MQTT publish/control enabled (requires wifiEn)
    int      webApiEn          = 0;   // 1 = REST API + settings page enabled (requires wifiEn)
    int      mqttCtrlEn        = 0;   // 1 = MQTT command topic enabled (requires mqttEn)
    char     deviceName[33]    = "";  // freely chosen; WiFi hostname, MQTT client id, default topic prefix
    int      staticIpEn        = 0;   // 0 = DHCP (default), 1 = static IP below
    char     staticIp[16]      = "";
    char     staticGateway[16] = "";
    char     staticSubnet[16]  = "255.255.255.0";
    char     staticDns[16]     = "";  // optional; falls back to staticGateway if empty
    char     mqttHost[65]      = "";
    int      mqttPort          = 8883;  // standard ports: 1883 = plain/no TLS, 8883 = TLS
    char     mqttUser[33]      = "";
    char     mqttPass[33]      = "";
    char     mqttTopic[65]     = "";  // optional topic prefix override; falls back to deviceName
    int      mqttTlsMode       = 0;   // MqttTlsMode (see above)
    char     mqttCaCert[2048]  = "";  // PEM root CA, only used when mqttTlsMode == MQTT_TLS_VERIFY
    char     webAdminPass[33]  = "";  // gates write access to the web settings page
};

Config loadConfig();
void   saveConfig(const Config& c);

// ============================================================================
// NVS UTILITY FUNCTIONS (shared with the Wi-Fi config portal)
// ============================================================================
uint64_t hexToUint64(const char* s);
void     hexToBytes(const char* s, uint8_t* out, size_t maxLen);
void     uint64ToHex(uint64_t value, char* out, size_t outSize);
void     bytesToHex(const uint8_t* bytes, size_t byteCount, char* out, size_t outSize);
bool     isSupportedRegion(const String& region);
void     normalizeRegionInput(const char* raw, char* out, size_t outSize);
