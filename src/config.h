#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "oled_custom.h"
#include "vsensor.h"
#include "net_sensor.h"

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

// Custom PCBs for the Heltec V4. Config::pcbVersion is set at boot from the
// revision ID on GPIO15 (PCB_REV_V4_FULL, PCB_REV_V4_LIGHT, or 0 = no PCB:
// wireless station) and never stored. The numbers are the pcbRev of the API,
// MQTT and LoRaWAN payload (20 was "PCB v2.0", kept for the V4 Full). Rev 1.2 /
// 1.3 are V3-only boards (firmware 2.x).
constexpr int PCB_REV_V4_FULL  = 20;
constexpr int PCB_REV_V4_LIGHT = 21;

// ============================================================================
// CONFIGURATION (persistent via NVS / Preferences)
// ============================================================================
constexpr int RF_RADIO_OFF = 0, RF_RADIO_SX1262 = 1;
// Wireless protocols (gCfg.rfProto bit mask): Bresser 5/6/7-in-1 etc. (8.21 kbit/s),
// LaCrosse / TFA IT+ at 17.241 kbit/s (TX29-IT, TFA 30.3159.IT, ...) and at
// 9.579 kbit/s (TX35-IT, TFA 30.3155.WD, ...), Fine Offset / Ecowitt / Froggit
// (17.241 kbit/s, received in the same phase as IT+ 17.241 kbit/s). Several
// data rates = the receiver alternates.
constexpr int RF_PROTO_BRESSER = 1, RF_PROTO_ITPLUS17 = 2, RF_PROTO_ITPLUS9 = 4, RF_PROTO_FINEOFFSET = 8,
              RF_PROTO_ALL = 15;
// "Auto (search)": not a bit mask - the receiver cycles through all receive
// phases (Bresser, 17.241 kbit/s = IT+ 17 + Fine Offset, IT+ 9.579 kbit/s)
// until the first valid packet (from a listed ID, if an ID list is set),
// then stores the matching type (1, 10 or 4) in place of this value.
constexpr int RF_PROTO_AUTO = 16;
// "Davis (exclusive)": not a bit mask either - the radio follows the hop
// sequence of one Davis Vantage Pro2 / Vue transmitter continuously
// (davis_rx.cpp): no other wireless protocols, LoRaWAN paused. Only when
// selected; Auto (search) never picks it.
constexpr int RF_PROTO_DAVIS = 17;
// Valid stored / parameter-50 values: a bit mask 1..15, RF_PROTO_AUTO or RF_PROTO_DAVIS.
inline bool rfProtoValid(int v) { return (v >= 1 && v <= RF_PROTO_ALL) || v == RF_PROTO_AUTO || v == RF_PROTO_DAVIS; }
inline bool rfDavisMode(int rfEn, int rfProto) { return rfEn == RF_RADIO_SX1262 && rfProto == RF_PROTO_DAVIS; }
// Davis receiver bandwidths offered (SX1262 FSK steps, kHz x 10)
inline bool davisRxBwValid(int v) { return v == 586 || v == 782 || v == 938 || v == 1173 || v == 1562 || v == 2343; }

struct Config {
    // Set once the config portal has been completed the first time. Used
    // instead of devEui==0 to detect "needs first-time setup", since a
    // WiFi/MQTT-only node (loraEn=0) may never set a non-zero DevEUI.
    int      provisioned        = 0;

    uint64_t joinEui           = 0;
    uint64_t devEui            = 0;
    uint8_t  appKey[16]        = {};
    char     region[8]         = "EU868"; // LoRaWAN region (default)
    int      pcbVersion        = 0;   // runtime only: PCB_REV_V4_FULL / PCB_REV_V4_LIGHT from the revision ID on GPIO15, else 0 (no PCB)
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
    volatile int uplinkIntervalSec = 300;  // LoRaWAN uplink interval (s); the measurement cycle is measureSec
    volatile int sampleSec         = 1;    // wind sample interval (s): 1 or 3 (see wind_window.h)
    volatile int mqttIntervalSec   = 0;    // MQTT publish interval (s); 0 = same as uplinkIntervalSec
    volatile int loraWindAgg       = 0;    // LoRaWAN wind values: 0 = interval since last uplink, 1 = 10-min (WMO)
    volatile int mqttWindAgg       = 0;    // MQTT wind values: 0 = interval since last publish, 1 = 10-min (WMO)
    // Defaults for a new (unconfigured) node: a plain Heltec with a wireless
    // outdoor sensor and Wi-Fi - see ConfigLegacyDefaults below for nodes
    // configured by an older firmware.
    volatile int rfProto           = RF_PROTO_AUTO;  // wireless protocols, bit mask RF_PROTO_* or RF_PROTO_AUTO (rf_sensors.cpp); several = time-shared
    volatile int rfEn              = RF_RADIO_SX1262; // 868 MHz wireless sensors (rf_sensors.cpp): 0 = off, RF_RADIO_SX1262 = shared LoRa chip
    volatile int windDirOffsetDeg  = 0;   // north offset (added to raw value, mod 360)
    volatile int windUnitMs        = 1;   // 1 = transmit wind in m/s, 0 = transmit in km/h
    volatile int windEn            = 0;   // 1 = Davis wind sensor on the PCB enabled (direction + speed); off: no PCB = floating inputs
    volatile int rainEn            = 0;   // 1 = Davis rain sensor on the PCB enabled
    volatile int rainPersist       = 1;   // 1 = rain statistics (daily + week/month/year) saved to flash (cfgdata, "rainstat") at most hourly + before restarts
    volatile int lipoEn            = 0;   // 1 = LiPo measurement enabled (saves 4 LPP bytes when 0)
    volatile int debugMode         = 0;   // 1 = include debug telemetry in payload
    volatile int cpuFreqMhz        = 240; // 80 or 240 MHz (240: the web UI is noticeably faster; 80 saves power); below 80 not allowed (PCNT-APB)
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
    // Calibration of the main values (calibration.h), applied after the value
    // source was chosen - for the board sensors and wireless sensors alike.
    volatile int calTempDc     = 0;     // temperature offset, 0.1 K (-100..100)
    volatile int calHumDpct    = 0;     // humidity offset, 0.1 % (-200..200)
    volatile int calPressDhpa  = 0;     // station pressure offset before QNH, 0.1 hPa (-200..200)
    volatile int calRainPermil = 1000;  // rain factor x1000 (500..2000)
    volatile int calWindPermil = 1000;  // wind speed / gust factor x1000 (500..2000)
    // Per wireless sensor: "ID:t:h,..." offsets in 0.1 K / 0.1 % (calibration.h)
    // Per received-only wireless sensor: "ID:name|..." (rf_names.h); a ★ sensor's
    // name and minimum signal are in its logical sensor entry (lsensors.h)
    char     rfNames[1024]     = "";
    // Per wireless sensor minimum signal: "ID:dBm|..." (rf_minrssi.h), "" = every packet
    char     rfMinRssi[256]    = "";
    char     rfCal[1024]       = "";      // stored "A1B2C3D4:5:-20[:gains]" (calibration.h), ~15-40 bytes per sensor
    // Sunshine detection (derived.h): 0 = illuminance threshold, 1 = solar
    // radiation >= 0.4 x clear-sky maximum (needs a W/m2 source and lat/lon)
    volatile int sunMethod     = 0;
    // Built-in OLED (oled_screens.h): 0 = on for 20 s after a PRG press (as
    // before), 1 = always on, 2 = on for dispMinutes after a PRG press
    volatile int dispMode      = 0;
    volatile int dispMinutes   = 5;     // 1-120
    volatile int dispScreens   = 1 | (1 << 8);   // bit n = screen n (0 classic, 1 temperature, 2 wind, 3 rain, 4 pressure, 5 status, 6-7 custom, 8 Wi-Fi QR while the own AP runs)
    volatile int dispRotateSec = 5;     // 2-60
    volatile int dispContrast  = 200;   // 1-255
    volatile int dispNightFrom = 0;     // night off [from, to) local hours; equal = never
    volatile int dispNightTo   = 0;
    volatile int measureSec    = 30;    // measurement cycle (s), 10-600: values, display, chart, records; the outputs send on their own intervals
    volatile int dispFlip      = 0;     // 1 = rotated by 180° (0 = the orientation of older firmware)
    volatile int dispBattery   = 0;     // 1 = battery voltage top right on every screen (measures even with lipoEn off)
    volatile int saverMode     = 0;     // screensaver (screensaver.h): 0 off, 1 blank, 2 dot, 3 walking figure, 4 weather icons
    volatile int saverMin      = 5;     // minutes between takeovers, 1-60
    volatile int saverSec      = 8;     // duration of a takeover, 3-30 s
    volatile int dispLed       = 0;     // status LED (status_led.h): 0 off, 1 heartbeat (2 s), 2 packets of my sensors, 3 every packet
    OledCustom   dispCustom[OLED_CUSTOM_COUNT];
    volatile int windDirSensorType = 0;     // 0 = continuous potentiometer (linear map), 1 = calibrated lookup table (e.g. reed-switch/resistor-ladder vanes)
    int      windDirCalCount   = 0;         // number of valid entries in the calibration table below (0 = not calibrated)
    uint16_t windDirCalAdc[16] = {};        // raw ADC reading captured per calibration position
    uint16_t windDirCalDeg[16] = {};        // corresponding direction in degrees per calibration position

    // ------------------------------------------------------------------
    // Parallel WiFi + MQTT transport (all independent, default disabled)
    // ------------------------------------------------------------------
    int      loraEn            = 0;   // 1 = LoRaWAN transport enabled (optional; needs keys)
    volatile int lowPowerEn    = 0;   // 1 = light sleep between sub-samples (LoRa-only operation, see low_power.cpp)
    volatile int vanePowerSave = 1;   // 1 = low-power mode also switches the wind-vane supply off between readings (V4 Full / V4 Light)
    int      wifiEn            = 1;   // 1 = WiFi STA enabled (master switch for mqttEn/webApiEn)
    int      mqttEn            = 0;   // 1 = MQTT publish/control enabled (requires wifiEn; needs a broker)
    int      webApiEn          = 1;   // 1 = REST API + settings page enabled (requires wifiEn)
    int      mqttCtrlEn        = 0;   // 1 = MQTT command topic enabled (requires mqttEn)
    // HTTPS on port 443 with the admin's certificate or a generated
    // self-signed one (https_cert.h; the pairs are the NVS blobs "https" /
    // "httpsgen", not in Config). Read at the server start.
    int      httpsEn           = 0;
    int      httpPortMode      = 0;   // port 80 while HTTPS runs: 0 stays available, 1 redirects browser pages, 2 HTTPS only (HttpPortMode)
    char     deviceName[33]    = "";  // freely chosen; WiFi hostname, MQTT client id, default topic prefix
    int      staticIpEn        = 0;   // 0 = DHCP (default), 1 = static IP below
    char     staticIp[16]      = "";
    char     staticGateway[16] = "";
    char     staticSubnet[16]  = "255.255.255.0";
    char     staticDns[16]     = "";  // optional; falls back to staticGateway if empty
    // Own access point (wifi_ap.h): 0 off, 1 fallback (station unreachable for 2 min), 2 always
    int      apMode            = 0;
    char     apSsid[33]        = "";  // empty = "WeatherNode-<last 4 MAC hex digits>"
    char     apPass[64]        = "";  // WPA2, 8-63 characters; the AP does not start without one
    int      apChannel         = 0;   // 0 = auto (the station's channel, else 1), 1-13
    char     mqttHost[65]      = "";
    int      mqttPort          = 1883;  // matches the mqttTlsMode default (plain); TLS brokers usually listen on 8883
    char     mqttUser[33]      = "";
    char     mqttPass[33]      = "";
    char     mqttTopic[65]     = "";  // optional topic prefix override; falls back to deviceName
    int      mqttTlsMode       = 0;   // MqttTlsMode (see above)
    char     mqttCaCert[2048]  = "";  // PEM root CA, only used when mqttTlsMode == MQTT_TLS_VERIFY
    // Davis (exclusive) receiver, web settings "Davis (diagnostics)", all live (davis_rx.cpp)
    volatile int davisTxId     = 0;      // transmitter ID 1-8 (DIP switch); 0 = the first one heard
    volatile int davisOffsetHz = 0;      // frequency correction, -100000..100000 Hz
    volatile int davisWindowMs = 25;     // a packet may arrive this late (5-500 ms) ...
    volatile int davisGuardMs  = 40;     // ... or this early (5-500 ms) relative to the prediction
    volatile int davisRainFilter = 1;    // phantom-rain filter for the tip counter (DavisRainFilter)
    volatile int davisRainMaxPkt = 8;    // tips per counter packet accepted without confirmation (1-127)
    volatile int davisRainMaxMin = 50;   // tips per minute a confirmed jump may have (1-127), else counter reset
    volatile int davisBucket   = 0;      // rain collector: 0 = 0.2 mm (EU), 1 = 0.01 in (US)
    volatile int davisModel    = 0;      // wind vane: 0 = Vantage Pro2 (potentiometer), 1 = Vantage Vue (Hall)
    volatile int davisSniffer  = 0;      // 1 = every packet to the web log
    volatile int davisRaw      = 0;      // 1 = raw capture: park on davisRawCh, no hopping
    volatile int davisRawCh    = 0;      // 0-4 (hop-sequence index)
    volatile int davisScanKhz  = 30;     // acquisition offset scan +-0..60 kHz (0 = off)
    volatile int davisAfc      = 0;      // 1 = AFC by shoulder probing while tracking
    volatile int davisTable    = 0;      // channel table: 0 = measured (2026), 1 = classic DavisRFM69
    volatile int davisRxBw     = 938;    // receiver bandwidth, kHz x 10 (davisRxBwValid())
    uint8_t  srcSel[6]         = { 0, 0, 0, 0, 0, 0 };   // value sources (sources.h SrcGroup): air, wind, rain, UV, light, pressure; 0 = auto
                                                      // (keys src0..src5; a config without src5 = Auto, i.e. the BME280 as before)
    // The logical sensor (lsensors.h, internal ID "L3F9A2C") a wireless source is
    // bound to ("" = auto/board/off, or a "Wireless n" set by number for a
    // received-only sensor: then by slot). Keeps the source on the chosen
    // sensor when the ★ order changes or its radio ID is re-bound (keys srciid0..srciid5).
    char     sourceInternalId[6][8]      = { "", "", "", "", "", "" };
    // The ★ sensors ("Mine") are the logical sensor table (lsensors.h, gLogicalSensors,
    // NVS blob "lsens"); their order = wireless 1..n, HA device per sensor.
    // Hidden sensors (settings: cross): never stored in the sensor table, no
    // slot, not output anywhere. Hex IDs, comma-separated (max RF_MAX_SENSORS). NVS "rfignore".
    char     rfIgnore[320]     = "";
    // 1 = accept only the ★ sensors (no effect while there is none); 0 = also
    // the other received sensors (they follow the ★ ones in reception order). NVS "rfonly".
    int      rfOnlyMine        = 0;
    // Web interface / portal language (i18n.h): 0 = Auto (browser), 1 en, 2 de,
    // 3 fr, 4 es, 5 pt. NVS "uilang"; missing key = Auto.
    int      uiLang            = 0;
    // Setup wizard (/setup) finished or dismissed. 0 = the dashboard opens the
    // wizard once per browser session and it may be used without an admin
    // password while none is set. NVS "setupdone"; missing key: 1 for a
    // config stored by an older firmware (no wizard for existing nodes), 0
    // for a new node / after a factory reset.
    int      setupDone         = 0;
    char     webAdminPass[33]  = "";  // gates write access to the web settings page

    // Loxone UDP output (loxone.h): one datagram "name=value" per value.
    // Live (no restart); loxHost/loxPrefix are written under loxoneLock().
    volatile int loxEn         = 0;      // 1 = send to a Loxone Miniserver (requires wifiEn)
    char     loxHost[65]       = "";     // Miniserver IP address or host name
    volatile int loxPort       = 7000;   // UDP port of the Virtual UDP Input (1..65535)
    char     loxPrefix[17]     = "";     // optional name prefix, [A-Za-z0-9_.-], max 16 (e.g. "g75_")
    volatile int loxSec        = 0;      // Loxone interval (s): 0 = every base cycle, else 2..3600

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
    // GNSS module on a V4 without PCB (gnss.h): on / off, rate (0 = search
    // 9600 / 115200 / 38400), and what it takes over: the clock, latitude /
    // longitude, the station altitude (sea-level pressure)
    int      gnssEn            = 0;
    int      gnssBaud          = 0;
    int      gnssTime          = 1;
    int      gnssPos           = 1;
    int      gnssAlt           = 1;
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
    char     windyId[33]       = "";   // Windy station ID (stations.windy.com, API v2)
    char     windyPass[81]     = "";   // Windy station password
    int      windyIntervalSec  = 300;
    char     osemBoxId[33]     = "";   // openSenseMap senseBox ID
    char     osemToken[81]     = "";   // box access token (Authorization header)
    char     osemSensors[321]  = "";   // "temp=<sensorId>,hum=<id>,..." (see wx_upload.cpp)
    int      osemIntervalSec   = 300;
    char     wowSiteId[24]     = "";   // Met Office WOW site ID (wx_formats.h)
    char     wowPin[24]        = "";   // WOW site authentication key (6-digit PIN)
    int      wowIntervalSec    = 300;
    char     cwopCall[12]      = "";   // CWOP ID (CW1234) or a ham callsign (APRS-IS)
    int      cwopPasscode      = -1;   // -1 for CWOP IDs, the APRS-IS passcode for a callsign
    int      cwopIntervalSec   = 600;
    bool     updateCheckEn     = false;   // daily check for newer firmware (tags/releases API)
    // Web UI (dashboard, include/chart_page.h; read via /api/info "ui"): both off by default
    bool     uiFlashEn         = false;   // a dashboard tile flashes briefly when its value changes
    bool     uiAgeEn           = false;   // a dashboard tile shows how old its value is (3 s ago)
    char     updateUrl[161]    = "https://api.github.com/repos/franzenhof/universal-weather-node/releases";

    // InfluxDB / VictoriaMetrics push (influx.cpp, wx_upload.cpp): line protocol
    // over HTTP, timestamp in the packet. The full write URL is given so v1
    // (/write?db=), v2 (/api/v2/write?org=&bucket=) and VictoriaMetrics all work.
    int      influxEn          = 0;    // 1 = push enabled (requires wifiEn + a write URL)
    char     influxUrl[256]    = "";   // e.g. https://host:8086/api/v2/write?org=home&bucket=weather
    char     influxToken[256]  = "";   // Authorization header value (v2: "Token xx␣"); empty = no auth (e.g. VictoriaMetrics)
    char     influxMeasurement[33] = "weather";   // measurement name (per-sensor lines use <name>_rf)
    int      influxIntervalSec = 60;

    // Virtual sensors (vsensor.h): not here - own store (vs_store.h, NVS "vsens").

    // Network sensors (net_sensor.h, docs/network-sensors.md): values from an
    // MQTT topic on the broker above or a polled JSON URL, in the sensor table
    // as E1000001 .. E1000004. All off by default. Written under netLock().
    NetSlotCfg net[NET_SLOTS];

    // ------------------------------------------------------------------
    // V4 Full I/O block (board_io.cpp) - all off by default, ignored on
    // the V4 Light and without a PCB. Written by applyParam() like the fields above (volatile).
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
    volatile int   veDirectEn     = 0;     // 1 = Victron VE.Direct (GPIO33 RX / GPIO45 TX)
    volatile int   outMinBattDv   = 0;     // heater/fan locked out below this battery voltage
                                           // or without valid VE.Direct (0.1 V); 0 = no limit
    volatile int   uvEn           = 0;     // 1 = UV sensor analog voltage on GPIO3 (V4 Full only)
    volatile float uvAinScale     = 1.1f;     // terminal V per ADC V; JP10 1-2: 1.1, 2-3: 1.66534
    volatile int   uvIndexEn      = 0;     // 1 = publish calculated UV index; requires explicit calibration flag
    volatile int   uvCalibrated   = 0;     // explicit user confirmation that slope/offset are calibrated
    volatile float uvIndexSlope  = 1.0f;  // UV index per terminal volt
    volatile float uvIndexOffset = 0.0f;  // UV index offset
};

// Defaults before 2.0.0-rc.2 of the fields whose default changed. loadConfig()
// uses these for a key missing from a config stored by an older firmware, so
// an existing node never switches e.g. LoRaWAN off or its Davis sensors off
// just because the new default differs. A new node (or one after a factory
// reset: no stored config) gets the Config defaults above.
struct ConfigLegacyDefaults {
    int loraEn = 1, wifiEn = 0, webApiEn = 0, rfEn = RF_RADIO_OFF, rfProto = RF_PROTO_BRESSER, windEn = 1, rainEn = 1;
};

Config loadConfig();
// The same into an existing Config that holds the defaults (a fresh Config,
// e.g. on the heap): Config is ~7 KB, too big for the 8 KB web task stack.
void loadConfigInto(Config& c);
// Loaded values outside what the settings form / applyParam() accept (a
// damaged NVS entry, another firmware's limits) back into range: choices to
// their default, ranges clamped. Called by loadConfigInto().
void sanitizeConfig(Config& c);
// A decimal-degree coordinate typed in a form (latitude: limit 90, longitude
// 180): "49.88", also with the decimal comma "49,88"; empty = not set. Writes
// the value with a point to out; false (out untouched) for anything else.
bool coordParse(const char* in, float limit, char* out, size_t outLen);
// Every key of c to NVS. Skipped (logged) once configHoldForRestart() ran.
void   saveConfig(const Config& c);
// A save stored settings that take effect only at the restart that follows
// (the web settings' restart fields, which gCfg does not take until then).
// From now until that restart saveConfig() writes nothing: another caller
// (★ button, downlink, MQTT command) would save gCfg and so put the old
// restart fields back. Its change stays in RAM until the restart - the
// restart fields win over a small change made in the last moments.
void   configHoldForRestart();
bool   configHeldForRestart();
// The whole key set of saveConfig() (with the ★ table, series and virtual
// sensors from RAM) into a namespace "config" the caller opened for writing:
// the settings reset writes the kept settings this way after sealing the
// storage (settings_reset.h). false = a write failed.
class Preferences;
bool   saveConfigKeys(Preferences& prefs, const Config& c);
// Writes only the "rfproto" key (wireless auto search locked: one NVS write).
void   saveRfProto(int proto);
// Writes only "lat" / "lon" (pos) and "altitude" (alt): GNSS take-over (gnss.cpp).
void   saveStationPosition(const Config& c, bool pos, bool alt);
// The logical sensor table gLogicalSensors (lsensors.h) from NVS ("lsens"); empty when
// none is stored. saveConfig() writes it back (only when it changed).
void   lsLoad();
void   seriesLoad();   // the custom series (series.h), not part of Config
// The virtual sensor settings (vs_store.h) from NVS ("vsens"); defaults (off)
// when none are stored. saveConfig() writes them back (only when changed).
void   vsSettingsLoad();

// ============================================================================
// NVS UTILITY FUNCTIONS (shared with the Wi-Fi config portal)
// ============================================================================
uint64_t hexToUint64(const char* s);
void     hexToBytes(const char* s, uint8_t* out, size_t maxLen);
void     uint64ToHex(uint64_t value, char* out, size_t outSize);
void     bytesToHex(const uint8_t* bytes, size_t byteCount, char* out, size_t outSize);
bool     isSupportedRegion(const String& region);
void     normalizeRegionInput(const char* raw, char* out, size_t outSize);
