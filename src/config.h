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
// v2.0 (KiCad/SMD): same pins as Rev 1.3 plus the I/O block (board_io.cpp).
// Detected at boot via the revision ID divider on GPIO20, so the stored value
// only matters as a manual override when the ID can't be read.
constexpr int PCB_REV_2_0 = 20;

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
    // volatile: written by applyParam() (params.cpp), which now runs
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
    volatile int payloadFormat     = 1;   // 1 = compact binary (FPort 3, default), 0 = CayenneLPP (FPort 1, ~2x larger)
    volatile int unitSystem        = 0;   // 0 = Metric (°C, mm, hPa, wind per windUnitMs), 1 = Imperial (°F, in, inHg, mph)

    // ------------------------------------------------------------------
    // Generic (non-Davis) sensor calibration
    // ------------------------------------------------------------------
    // Same cross-task write path as the volatile int fields above
    // (applyParam() cases 12/13, params.cpp) — same rationale, just
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
    volatile int lowPowerEn    = 0;   // 1 = light sleep between sub-samples (LoRa-only operation, see low_power.cpp)
    volatile int vanePowerSave = 1;   // 1 = low-power mode also switches the wind-vane supply off between readings (PCB v2.0)
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
    int      mqttPort          = 1883;  // matches the mqttTlsMode default (plain); TLS brokers usually listen on 8883
    char     mqttUser[33]      = "";
    char     mqttPass[33]      = "";
    char     mqttTopic[65]     = "";  // optional topic prefix override; falls back to deviceName
    int      mqttTlsMode       = 0;   // MqttTlsMode (see above)
    char     mqttCaCert[2048]  = "";  // PEM root CA, only used when mqttTlsMode == MQTT_TLS_VERIFY
    char     webAdminPass[33]  = "";  // gates write access to the web settings page

    // ------------------------------------------------------------------
    // Wall clock (NTP) + direct weather-network uploads (all need wifiEn)
    // ------------------------------------------------------------------
    // POSIX TZ string (see time_sync.cpp). Default: Central Europe with DST.
    char     timeZone[48]      = "CET-1CEST,M3.5.0,M10.5.0/3";
    char     ntpServer[65]     = "pool.ntp.org";
    // Station position, decimal degrees as text ("47.1234") so an unset
    // value stays distinguishable from a real 0.0. Only sent to AWEKAS.
    char     latitude[12]      = "";
    char     longitude[12]     = "";
    // Each network is enabled by a non-empty station ID/user (same rule as
    // weather-data-bridge's env vars), interval in seconds per network.
    char     awekasUser[33]    = "";
    char     awekasPass[33]    = "";
    int      awekasIntervalSec = 300;
    char     wuStationId[33]   = "";
    char     wuStationKey[33]  = "";
    int      wuIntervalSec     = 300;
    char     wcloudId[33]      = "";
    char     wcloudKey[33]     = "";
    int      wcloudIntervalSec = 600;
    char     wnetproId[33]     = "";
    char     wnetproKey[33]    = "";
    int      wnetproIntervalSec = 60;

    // ------------------------------------------------------------------
    // PCB v2.0 I/O block (board_io.cpp) - all off by default, ignored on
    // Rev 1.2/1.3. Written by applyParam() like the fields above (volatile).
    // Temperatures in 0.1 °C, independent of unitSystem.
    // ------------------------------------------------------------------
    volatile int   auxAinEn       = 0;     // 1 = AUX analog input (GPIO4)
    volatile float auxAinScale    = 1.0f;  // V at terminal per V at pin (R25 15k fitted: 1.66667)
    volatile int   auxPulseEn     = 0;     // 1 = AUX pulse counter (GPIO48)
    volatile int   wind2Mode      = 0;     // 2nd anemometer on AUX: 0 = off, 1 = speed (PULSE),
                                           // 2 = speed + direction (PULSE + AIN); replaces the AUX functions
    volatile int   wind2DirOffsetDeg = 0;  // north alignment of the 2nd vane (-180..180)
    volatile int   heaterMode     = 0;     // 0 = off, 1 = on, 2 = auto (below heaterOnBelowDc)
    volatile int   heaterDutyPct  = 100;   // PWM duty when on (mode 1) / at heaterFullBelowDc and colder (auto)
    volatile int   heaterOnBelowDc = 40;   // auto: on below 4.0 °C, off 1 K above
    volatile int   heaterMinDutyPct = 10;  // auto: PWM duty at heaterOnBelowDc, rising linearly to
    volatile int   heaterFullBelowDc = -100; // heaterDutyPct at -10.0 °C
    volatile int   fanMode        = 0;     // 0 = off, 1 = on, 2 = auto (above fanOnAboveDc)
    volatile int   fanDutyPct     = 100;   // PWM duty while on (1-100 %)
    volatile int   fanOnAboveDc   = 150;   // auto: on from 15.0 °C, off 1 K below
    volatile int   fanMinPvW      = 0;     // auto: requires valid VE.Direct PV >= limit; 0 = ignore
    volatile int   tachoEn        = 0;     // 1 = fan tacho (GPIO40)
    volatile int   tachoPpr       = 2;     // tacho pulses per revolution (PC fans: 2)
    volatile int   veDirectEn     = 0;     // 1 = Victron VE.Direct (GPIO33 RX / GPIO34 TX)
    volatile int   outMinBattDv   = 0;     // heater/fan locked out below this battery voltage
                                           // or without valid VE.Direct (0.1 V); 0 = no limit
    volatile int   uvEn           = 0;     // 1 = UV sensor analog voltage on GPIO3 (PCB v2.0 only)
    volatile float uvAinScale     = 1.1f;     // terminal V per ADC V; JP10 1-2: 1.1, 2-3: 1.66534
    volatile int   uvIndexEn      = 0;     // 1 = publish calculated UV index; requires explicit calibration flag
    volatile int   uvCalibrated   = 0;     // explicit user confirmation that slope/offset are calibrated
    volatile float uvIndexSlope  = 1.0f;  // UV index per terminal volt
    volatile float uvIndexOffset = 0.0f;  // UV index offset
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
