#include "portal.h"
#include "globals.h"
#include "sensors.h"
#include "generated_version.h"
#include "decoder_js.h"
#include "readme_md.h"
#include "help_page.h"
#include "calibrate_page.h"
#include "serial_log.h"

#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// ============================================================================
// WI-FI CONFIGURATION PORTAL
// ============================================================================
Config runConfigPortal(const Config& cur)
{
    // Frozen copy of the config as it was when the portal was opened. cur
    // itself is a reference to gCfg (openPortalSaveAndRestart() calls this
    // as runConfigPortal(gCfg)), which the immediate-persist save in
    // setSaveParamsCallback below reassigns — buildConfig() uses this
    // snapshot as its baseline instead, so a second param-only save in the
    // same session doesn't build on top of the first save's result.
    const Config curSnapshot = cur;

    // Set by /calibrate/save below. The calibration is persisted (and copied
    // into gCfg) right there, but buildConfig() starts from curSnapshot and
    // the form's sensor-type field still shows the pre-calibration value -
    // without this, closing the portal would write the old table back.
    bool calibratedThisSession = false;

    // If the parallel WiFi/MQTT transport (wifi_mqtt.cpp) already has an
    // active STA connection at this point, WiFiManager's own "disconnect
    // STA before starting the AP" workaround (startConfigPortal() only runs
    // it when WiFi.isConnected() is false) gets skipped, leaving the old STA
    // link up alongside the new "WeatherNode" AP — this is what causes the
    // captive portal to work only partially (channel contention, auto-popup
    // detection wrongly seeing internet access, unreliable DNS hijack).
    // Disconnecting unconditionally here guarantees a clean AP-only start.
    //
    // Deliberately WiFi.disconnect() with wifioff left at its default
    // (false), NOT WiFi.disconnect(true): the `true` overload additionally
    // calls WiFi.enableSTA(false), toggling the STA mode bit ourselves right
    // before WiFiManager's own startConfigPortal() immediately does its own
    // disconnect+enableSTA(false)+AP-mode bring-up sequence — two back-to-back
    // STA mode transitions with no settling time in between, which can leave
    // the softAP's netif in a half-initialized state: the AP answers ARP/ping
    // (link layer up) but the web server never actually starts accepting
    // connections on port 80. Plain disconnect() already makes
    // WiFi.isConnected() false (which is all that check needs) without
    // touching STA-enable state, so WiFiManager's own transition runs once,
    // cleanly, instead of racing against ours.
    //
    // esp_wifi_disconnect() (behind WiFi.disconnect()) is asynchronous —
    // it returns before the driver has actually finished tearing the link
    // down. WiFiManager's own code elsewhere explicitly delays 200 ms after
    // a disconnect for exactly this reason ("need a delay for disconnect to
    // change status()"), but its STA-disable-before-AP-start branch (the one
    // that runs right after this, since WiFi.isConnected() is now false)
    // does NOT — it goes straight from WiFi_Disconnect()/WiFi_enableSTA(false)
    // into bringing up the softAP. Without giving our own disconnect call
    // above the same settling time, that race is still there; the softAP can
    // end up in a half-initialized state (answers ARP/ping, but the web
    // server never actually starts accepting connections on port 80).
    WiFi.disconnect();
    delay(200);

    setCpuFrequencyMhz(240);
    display.displayOn();
    display.clear();
    display.drawString(0, 0, "Starting config portal...");
    display.display();

    char b_dev[17], b_join[17], b_key[33], b_region[8], b_pcb[4], b_alt[8], b_uplink[8], b_sample[4], b_windoff[6], b_windunit[2], b_debug[2], b_cpu[4], b_payfmt[2], b_unitsys[2];
    char b_rainmmtip[12], b_windmspp[12], b_winddirtyp[2];
    char b_loraen[2];
    itoa(cur.loraEn,          b_loraen,  10);
    uint64ToHex(cur.devEui, b_dev, sizeof(b_dev));
    uint64ToHex(cur.joinEui, b_join, sizeof(b_join));
    bytesToHex(cur.appKey, 16, b_key, sizeof(b_key));
    strlcpy(b_region, cur.region, sizeof(b_region));
    itoa(cur.pcbVersion,       b_pcb,     10);
    itoa(cur.altitudeM,        b_alt,     10);
    itoa(cur.uplinkIntervalSec, b_uplink,  10);
    itoa(cur.sampleSec,        b_sample,  10);
    itoa(cur.windDirOffsetDeg, b_windoff, 10);
    itoa(cur.windUnitMs,       b_windunit, 10);
    itoa(cur.debugMode,        b_debug,   10);
    itoa(cur.cpuFreqMhz,       b_cpu,     10);
    itoa(cur.payloadFormat,    b_payfmt,  10);
    itoa(cur.unitSystem,       b_unitsys, 10);
    dtostrf(cur.rainMmPerTip,  1, 4, b_rainmmtip);
    dtostrf(cur.windMsPerPps,  1, 4, b_windmspp);
    itoa(cur.windDirSensorType, b_winddirtyp, 10);

    // Parallel WiFi + MQTT transport fields
    char b_wifien[2], b_mqtten[2], b_mqttctrlen[2], b_mqttport[8], b_mqtttls[2], b_webapien[2];
    itoa(cur.wifiEn,     b_wifien,     10);
    itoa(cur.mqttEn,     b_mqtten,     10);
    itoa(cur.mqttCtrlEn, b_mqttctrlen, 10);
    itoa(cur.mqttPort,   b_mqttport,   10);
    itoa(cur.mqttTlsMode, b_mqtttls,   10);
    itoa(cur.webApiEn,   b_webapien,   10);

    // The CA-cert field is rendered as a raw <textarea>, not a normal
    // WiFiManagerParameter input (2048 bytes doesn't fit that template well).
    // Its value is captured separately via setSaveParamsCallback() below.
    // Both buffers are `static` (~4.4 KB combined) to avoid growing this
    // function's stack frame — runConfigPortal() is never called reentrantly.
    static char b_cacert_html[sizeof(cur.mqttCaCert) + 300];
    snprintf(b_cacert_html, sizeof(b_cacert_html),
             "<label>MQTT Root CA certificate (PEM, only used if TLS mode = 1/CA-pinned)</label><br/>"
             "<textarea name='mqttcacert' id='mqttcacert' rows='8' cols='48' "
             "placeholder='-----BEGIN CERTIFICATE-----'>%s</textarea><br/>",
             cur.mqttCaCert);
    static char capturedCaCert[sizeof(cur.mqttCaCert)];
    bool capturedCaCertSet = false;

    // Static IP fields ("ip"/"gw"/"sn"/"dns") are WiFiManager's own built-in
    // form fields on the "Configure WiFi" page, not WiFiManagerParameters, so
    // they're captured the same way as the CA cert: read directly from
    // wm.server in a callback below.
    static char capturedStaticIp[16];
    static char capturedStaticGw[16];
    static char capturedStaticSn[16];
    static char capturedStaticDns[16];
    bool capturedStaticIpSet = false;

    WiFiManagerParameter p_lora_section("<div style='margin:0.5rem 0 0.5rem 0;'><strong>LoRaWAN</strong></div>");
    WiFiManagerParameter p_loraen("loraen", "LoRaWAN transport enabled (0=No, 1=Yes)", b_loraen, 2);
    WiFiManagerParameter p_dev    ("dev",      "DevEUI (16 hex chars)",                        b_dev,     17);
    WiFiManagerParameter p_join   ("join",     "JoinEUI (16 hex chars)",                       b_join,    17);
    WiFiManagerParameter p_key    ("key",      "AppKey (32 hex chars)",                        b_key,     33);
    WiFiManagerParameter p_region ("region",   "LoRaWAN region (EU868, US915, AU915, AS923, IN865, KR920, CN470, RU864)", b_region, 8);
    WiFiManagerParameter p_pcb    ("pcbver",   "PCB revision (12=Rev1.2, 13=Rev1.3)",          b_pcb,     3);
    WiFiManagerParameter p_alt    ("altitude", "Altitude above sea level in m (0-4000)",       b_alt,     6);
    WiFiManagerParameter p_uplink ("uplinksec","Uplink interval in s (5-3600 typical)",         b_uplink, 7);
    WiFiManagerParameter p_sample ("samplesec","Sample interval in s (1 to uplink interval)",  b_sample, 4);
    WiFiManagerParameter p_windoff("windoff",  "Wind direction offset in degrees (-180 to +180)", b_windoff, 5);

    // Grouped so the unit-related fields aren't scattered across the page:
    // one master switch (Metric/Imperial) plus the existing km/h-vs-m/s
    // refinement, which only applies when Units = Metric.
    WiFiManagerParameter p_units_section("<div style='margin:0.5rem 0 0.5rem 0;'><strong>Units</strong></div>");
    WiFiManagerParameter p_unitsys ("unitsys",  "Units (0=Metric: °C/mm/hPa, 1=Imperial: °F/in/inHg/mph)", b_unitsys, 2);
    WiFiManagerParameter p_windunit("windunit", "Wind speed unit if Metric (0=km/h, 1=m/s; ignored if Imperial)", b_windunit, 2);

    // Generic (non-Davis) sensor calibration. Rain/wind-speed are simple
    // scale factors; wind direction additionally supports a calibrated
    // lookup table for reed-switch/resistor-ladder vanes (which don't
    // output a continuous voltage) — see the "Calibrate Wind Direction"
    // button on the portal start page.
    WiFiManagerParameter p_cal_section("<div style='margin:0.5rem 0 0.5rem 0;'><strong>Sensor Calibration</strong> (defaults match the Davis 6410/6466M)</div>");
    WiFiManagerParameter p_rainmmtip("rainmmtip", "Rain: mm per tip/pulse",                     b_rainmmtip, 11);
    WiFiManagerParameter p_windmspp ("windmspp",  "Wind speed: m/s per pulse-per-second",       b_windmspp,  11);
    WiFiManagerParameter p_winddirtyp("winddirtyp", "Wind direction sensor (0=potentiometer, 1=calibrated table — see 'Calibrate Wind Direction' button)", b_winddirtyp, 2);

    WiFiManagerParameter p_general_section("<div style='margin:0.5rem 0 0.5rem 0;'><strong>General Parameters</strong></div>");
    WiFiManagerParameter p_winden ("winden",   "Wind sensor enabled (0=No, 1=Yes)",            cur.windEn ? "1" : "0", 2);
    WiFiManagerParameter p_rainen ("rainen",   "Rain sensor enabled (0=No, 1=Yes)",            cur.rainEn ? "1" : "0", 2);
    WiFiManagerParameter p_lipo   ("lipoen",   "LiPo measurement enabled (0=No, 1=Yes)",       cur.lipoEn ? "1" : "0", 2);
    WiFiManagerParameter p_debug  ("debugmode","Debug mode (0=No, 1=Yes, sends extra telemetry)", b_debug, 2);
    WiFiManagerParameter p_cpu    ("cpufreq",  "CPU frequency in MHz (80 or 240)",             b_cpu,     4);
    WiFiManagerParameter p_payfmt ("payfmt",   "LoRaWAN payload format (0=CayenneLPP, 1=compact binary, ~50% smaller)", b_payfmt, 2);
    WiFiManagerParameter p_section("<div style='margin:0.5rem 0 0.75rem 0;'><h3>LoRa-Weather-Node Parameter</h3><p style='margin:0 0 0.35rem 0;'>Firmware version: " FIRMWARE_VERSION "</p><p style='margin:0;'>Device settings are shown here on a separate page.</p></div>");

    // Parallel WiFi + MQTT transport, fully optional alongside LoRaWAN. WiFi
    // STA credentials/static-IP are configured via WiFiManager's own "wifi"
    // menu, not here.
    WiFiManagerParameter p_net_section("<div style='margin:1rem 0 0.75rem 0;'><h3>WiFi + MQTT Parameter</h3><p style='margin:0;'>WiFi network credentials and static IP (optional, default DHCP) are configured separately via the \"Configure WiFi\" menu.</p></div>");
    WiFiManagerParameter p_wifien    ("wifien",     "WiFi enabled (0=No, 1=Yes) - master switch for MQTT/Web API", b_wifien, 2);
    WiFiManagerParameter p_devname   ("devname",    "Device name (WiFi hostname, MQTT client id, default topic prefix)", cur.deviceName, 33);
    WiFiManagerParameter p_webapien  ("webapien",   "Web REST API + settings page enabled (0=No, 1=Yes)", b_webapien, 2);
    WiFiManagerParameter p_webadminpw("webadminpw", "Web settings page admin password (required to change settings remotely)", cur.webAdminPass, 33);
    WiFiManagerParameter p_mqtten    ("mqtten",     "MQTT publish/control enabled (0=No, 1=Yes)",   b_mqtten,     2);
    WiFiManagerParameter p_mqttctrlen("mqttctrlen", "MQTT command topic enabled (0=No, 1=Yes)",     b_mqttctrlen, 2);
    WiFiManagerParameter p_mqtthost  ("mqtthost",   "MQTT broker host",                             cur.mqttHost, 65);
    WiFiManagerParameter p_mqttport  ("mqttport",   "MQTT broker port (standard: 1883 = plain/no TLS, 8883 = TLS)", b_mqttport, 7);
    WiFiManagerParameter p_mqttuser  ("mqttuser",   "MQTT username (optional)",                     cur.mqttUser, 33);
    WiFiManagerParameter p_mqttpass  ("mqttpass",   "MQTT password (optional)",                     cur.mqttPass, 33);
    WiFiManagerParameter p_mqtttopic ("mqtttopic",  "MQTT topic prefix (optional, default = device name)", cur.mqttTopic, 65);
    WiFiManagerParameter p_mqtttls   ("mqtttls",    "MQTT security (0=plain/no TLS, 1=TLS/trust any cert, 2=TLS/verify cert below)", b_mqtttls, 2);
    WiFiManagerParameter p_cacert_html(b_cacert_html);

    WiFiManager wm;
    const char* menu[] = { "wifi", "info", "custom", "sep", "exit" };
    wm.setMenu(menu, 5);
    wm.setCustomMenuHTML("<div style='margin:0.75rem 0 1rem 0;'><div style='font-size:0.85rem;opacity:0.75;margin:0 0 0.25rem 0;'>Firmware version: " FIRMWARE_VERSION "</div><form action='/param' method='get'><button>LoRa-Weather-Node Parameter</button></form><br/><form action='/decoder' method='get'><button>Payload decoder (JS)</button></form><br/><form action='/help' method='get'><button>Help / Documentation</button></form><br/><form action='/calibrate' method='get'><button>Calibrate Wind Direction</button></form><br/><form action='/update' method='get'><button>Firmwareupdate</button></form></div>");

    // Static IP (optional, default DHCP): shown as WiFiManager's own built-in
    // IP/Gateway/Subnet/DNS fields on the "Configure WiFi" page (not a custom
    // WiFiManagerParameter), pre-filled from the existing config if set.
    // Captured on save via setPreSaveConfigCallback() below; leaving the IP
    // field blank falls back to DHCP.
    if (cur.staticIpEn && cur.staticIp[0] != '\0') {
        IPAddress ip, gw, sn, dns;
        ip.fromString(cur.staticIp);
        gw.fromString(cur.staticGateway);
        sn.fromString(cur.staticSubnet);
        if (cur.staticDns[0] != '\0') dns.fromString(cur.staticDns);
        else                          dns = gw;
        wm.setSTAStaticIPConfig(ip, gw, sn, dns);
    }
    wm.setShowStaticFields(true);
    wm.setShowDnsFields(true);

    // Register custom routes:
    // - /decoder: version-matched payload decoder (embedded from chirpstack/decoder.js)
    // - /help, /help.md: rendered README help page (works fully offline, no CDN)
    // - /calibrate*: guided wind-direction calibration for resistor-ladder/
    //   reed-switch vanes (see include/calibrate_page.h)
    wm.setWebServerCallback([&wm, &calibratedThisSession]() {
        wm.server->on("/decoder", [&wm]() {
            wm.server->sendHeader("Content-Disposition",
                                  "attachment; filename=\"weather-node-decoder.js\"");
            wm.server->send_P(200, "application/javascript; charset=utf-8", DECODER_JS);
        });
        wm.server->on("/help", [&wm]() {
            wm.server->send_P(200, "text/html; charset=utf-8", HELP_PAGE_HTML);
        });
        wm.server->on("/help.md", [&wm]() {
            wm.server->send_P(200, "text/plain; charset=utf-8", README_MD);
        });
        wm.server->on("/calibrate", [&wm]() {
            wm.server->send_P(200, "text/html; charset=utf-8", CALIBRATE_PAGE_HTML);
        });
        wm.server->on("/calibrate/sample", HTTP_GET, [&wm]() {
            int adc = adcAverage(PIN_ADC_WINDDIRECTION);
            char buf[32];
            snprintf(buf, sizeof(buf), "{\"adc\":%d}", adc);
            wm.server->send(200, "application/json", buf);
        });
        wm.server->on("/calibrate/save", HTTP_POST, [&wm, &calibratedThisSession]() {
            String body = wm.server->arg("plain");
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                wm.server->send(400, "text/plain", "Invalid JSON");
                return;
            }
            Config c = loadConfig();
            int n = 0;
            for (JsonObject p : doc["positions"].as<JsonArray>()) {
                if (n >= 16) break;
                c.windDirCalAdc[n] = (uint16_t)constrain(p["adc"].as<int>(), 0, 4095);
                c.windDirCalDeg[n] = (uint16_t)(((p["deg"].as<int>() % 360) + 360) % 360);
                n++;
            }
            if (n == 0) {
                wm.server->send(400, "text/plain", "No positions received");
                return;
            }
            c.windDirCalCount   = n;
            c.windDirSensorType = 1;
            saveConfig(c);
            // Keep the in-memory gCfg in sync too, in case the device keeps
            // running (e.g. calibration re-run from the button-triggered
            // portal) without an immediate restart afterward.
            gCfg.windDirCalCount   = c.windDirCalCount;
            gCfg.windDirSensorType = c.windDirSensorType;
            memcpy(gCfg.windDirCalAdc, c.windDirCalAdc, sizeof(gCfg.windDirCalAdc));
            memcpy(gCfg.windDirCalDeg, c.windDirCalDeg, sizeof(gCfg.windDirCalDeg));
            calibratedThisSession = true;
            wm.server->send(200, "text/plain",
                            "Calibration saved (" + String(n) + " positions). You can close this page.");
        });
    });

    // Builds a full Config from the current state of every WiFiManagerParameter
    // plus the separately-captured CA-cert/static-IP fields. Shared by the
    // immediate-persist save below and the function's own return value, so
    // there's exactly one place that knows how to translate the portal's
    // form fields into a Config.
    auto buildConfig = [&]() -> Config {
        // curSnapshot, not cur: setSaveParamsCallback below assigns into
        // gCfg, which cur (a reference, since openPortalSaveAndRestart()
        // calls this as runConfigPortal(gCfg)) aliases — using cur directly
        // here would mean a second param-only save this session starts from
        // the first save's result instead of the original pre-portal state.
        Config c = curSnapshot;
        if (strlen(p_dev.getValue())  == 16) c.devEui  = hexToUint64(p_dev.getValue());
        if (strlen(p_join.getValue()) == 16) c.joinEui = hexToUint64(p_join.getValue());
        if (strlen(p_key.getValue())  == 32) hexToBytes(p_key.getValue(), c.appKey, 16);
        normalizeRegionInput(p_region.getValue(), c.region, sizeof(c.region));
        int pcb = atoi(p_pcb.getValue());
        c.pcbVersion       = (pcb == PCB_REV_1_2) ? PCB_REV_1_2 : PCB_REV_1_3;
        // >= 0, not > 0: 0 m (sea level) is a valid altitude and must not be
        // silently rejected in favor of the previously stored value.
        if (atoi(p_alt.getValue()) >= 0) c.altitudeM = atoi(p_alt.getValue());
        if (atoi(p_uplink.getValue()) > 0) c.uplinkIntervalSec = atoi(p_uplink.getValue());
        int ss = atoi(p_sample.getValue());
        int uplinkIntervalSecVal = c.uplinkIntervalSec; // std::min can't deduce a common type across int/volatile int
        c.sampleSec        = (ss > 0) ? min(ss, uplinkIntervalSecVal) : c.sampleSec;
        c.windDirOffsetDeg = constrain(atoi(p_windoff.getValue()), -180, 180);
        c.windUnitMs       = (atoi(p_windunit.getValue()) == 0) ? 0 : 1;
        c.unitSystem       = (atoi(p_unitsys.getValue()) == 1) ? 1 : 0;
        float rainMmPerTipVal = atof(p_rainmmtip.getValue());
        if (rainMmPerTipVal > 0.0f) c.rainMmPerTip = rainMmPerTipVal;
        float windMsPerPpsVal = atof(p_windmspp.getValue());
        if (windMsPerPpsVal > 0.0f) c.windMsPerPps = windMsPerPpsVal;
        // Calibration table: always the latest one - gCfg is updated by
        // /calibrate/save during this session, otherwise equals curSnapshot.
        c.windDirCalCount = gCfg.windDirCalCount;
        memcpy(c.windDirCalAdc, gCfg.windDirCalAdc, sizeof(c.windDirCalAdc));
        memcpy(c.windDirCalDeg, gCfg.windDirCalDeg, sizeof(c.windDirCalDeg));
        // A calibration saved this session switches to the table, unless the
        // user explicitly changed the sensor-type field afterwards.
        int formDirType = (atoi(p_winddirtyp.getValue()) == 1) ? 1 : 0;
        c.windDirSensorType = (calibratedThisSession && formDirType == curSnapshot.windDirSensorType)
                              ? 1 : formDirType;
        c.windEn           = atoi(p_winden.getValue());
        c.rainEn           = atoi(p_rainen.getValue());
        c.lipoEn           = atoi(p_lipo.getValue());
        c.debugMode        = (atoi(p_debug.getValue()) == 1) ? 1 : 0;
        c.cpuFreqMhz       = (atoi(p_cpu.getValue()) == 240) ? 240 : 80;
        c.payloadFormat    = (atoi(p_payfmt.getValue()) == 1) ? 1 : 0;

        // Parallel WiFi + MQTT transport
        c.loraEn     = atoi(p_loraen.getValue()) ? 1 : 0;
        c.wifiEn     = atoi(p_wifien.getValue()) ? 1 : 0;
        strlcpy(c.deviceName, p_devname.getValue(), sizeof(c.deviceName));
        // Static IP fields only come from the separate "Configure WiFi" page
        // (see setPreSaveConfigCallback below) — leave c.staticIpEn/staticIp
        // untouched (keep whatever cur/previous NVS state had) unless that
        // page was actually submitted this session, or a param-only save
        // would otherwise silently reset an existing static IP to DHCP.
        if (capturedStaticIpSet) {
            if (capturedStaticIp[0] != '\0') {
                c.staticIpEn = 1;
                strlcpy(c.staticIp,      capturedStaticIp, sizeof(c.staticIp));
                strlcpy(c.staticGateway, capturedStaticGw,  sizeof(c.staticGateway));
                strlcpy(c.staticSubnet,  capturedStaticSn[0] ? capturedStaticSn : "255.255.255.0", sizeof(c.staticSubnet));
                strlcpy(c.staticDns,     capturedStaticDns, sizeof(c.staticDns));
            } else {
                c.staticIpEn     = 0;
                c.staticIp[0]      = '\0';
                c.staticGateway[0] = '\0';
                c.staticDns[0]     = '\0';
            }
        }
        c.webApiEn   = atoi(p_webapien.getValue()) ? 1 : 0;
        strlcpy(c.webAdminPass, p_webadminpw.getValue(), sizeof(c.webAdminPass));
        c.mqttEn     = atoi(p_mqtten.getValue()) ? 1 : 0;
        c.mqttCtrlEn = atoi(p_mqttctrlen.getValue()) ? 1 : 0;
        strlcpy(c.mqttHost, p_mqtthost.getValue(), sizeof(c.mqttHost));
        int mqttPortVal = atoi(p_mqttport.getValue());
        if (mqttPortVal > 0 && mqttPortVal <= 65535) c.mqttPort = mqttPortVal;
        strlcpy(c.mqttUser, p_mqttuser.getValue(), sizeof(c.mqttUser));
        strlcpy(c.mqttPass, p_mqttpass.getValue(), sizeof(c.mqttPass));
        strlcpy(c.mqttTopic, p_mqtttopic.getValue(), sizeof(c.mqttTopic));
        int mqttTlsVal = atoi(p_mqtttls.getValue());
        c.mqttTlsMode = (mqttTlsVal >= MQTT_TLS_NONE && mqttTlsVal <= MQTT_TLS_VERIFY) ? mqttTlsVal : MQTT_TLS_NONE;
        // Only overwrite the stored CA cert if the /param form was actually
        // submitted this session (setSaveParamsCallback fired) — otherwise a
        // portal session that never touches the params page (e.g. WiFi-only
        // reconfiguration) would silently wipe a previously stored certificate.
        if (capturedCaCertSet) strlcpy(c.mqttCaCert, capturedCaCert, sizeof(c.mqttCaCert));

        return c;
    };

    // The CA-cert field is a raw <textarea>, not a normal id-based
    // WiFiManagerParameter, so it isn't populated by doParamSave() the usual
    // way. setSaveParamsCallback() fires right after params are saved, while
    // wm.server still has the submitted request's args available.
    wm.setSaveParamsCallback([&wm, &capturedCaCertSet, &buildConfig]() {
        String v = wm.server->arg("mqttcacert");
        strlcpy(capturedCaCert, v.c_str(), sizeof(capturedCaCert));
        capturedCaCertSet = true;

        // Persist immediately. The "LoRa-Weather-Node Parameter" page
        // (/param) is a separate form from "Configure WiFi", and
        // startConfigPortal() only returns once THAT one is submitted — a
        // user who only needs to change parameters (the common case) would
        // otherwise see WiFiManager's own "Saved" confirmation, consider
        // themselves done, and never actually have anything written to NVS
        // at all until/unless they also submit the WiFi page.
        gCfg = buildConfig();
        gCfg.provisioned = 1;
        saveConfig(gCfg);

        // End the portal session right after — matches the "click Save,
        // device restarts" behavior a "Configure WiFi" submission already
        // has, instead of leaving the AP/portal running indefinitely with
        // no further indication anything happened. stopConfigPortal() just
        // sets an internal abort flag; the blocking startConfigPortal() loop
        // checks it and returns on its next iteration (very soon), so this
        // callback itself still runs to completion first.
        wm.stopConfigPortal();
    });

    // Fires from handleWifiSave() right after it parses the "ip"/"gw"/"sn"/
    // "dns" args into its own private fields — read them ourselves here since
    // WiFiManager doesn't expose a public getter for them. Unlike the CA-cert
    // capture above, this fires on every portal exit (submitting the "Configure
    // WiFi" page is the only way startConfigPortal() returns), so a blank IP
    // field reliably means "the user wants DHCP", not "page wasn't visited".
    wm.setPreSaveConfigCallback([&wm, &capturedStaticIpSet]() {
        strlcpy(capturedStaticIp,  wm.server->arg("ip").c_str(),  sizeof(capturedStaticIp));
        strlcpy(capturedStaticGw,  wm.server->arg("gw").c_str(),  sizeof(capturedStaticGw));
        strlcpy(capturedStaticSn,  wm.server->arg("sn").c_str(),  sizeof(capturedStaticSn));
        strlcpy(capturedStaticDns, wm.server->arg("dns").c_str(), sizeof(capturedStaticDns));
        capturedStaticIpSet = true;
    });

    wm.addParameter(&p_section);
    wm.addParameter(&p_lora_section);
    wm.addParameter(&p_loraen);
    wm.addParameter(&p_dev);    wm.addParameter(&p_join);   wm.addParameter(&p_key); wm.addParameter(&p_region);
    wm.addParameter(&p_payfmt);
    wm.addParameter(&p_units_section);
    wm.addParameter(&p_unitsys); wm.addParameter(&p_windunit);
    wm.addParameter(&p_cal_section);
    wm.addParameter(&p_rainmmtip); wm.addParameter(&p_windmspp); wm.addParameter(&p_winddirtyp);
    wm.addParameter(&p_windoff);
    wm.addParameter(&p_general_section);
    wm.addParameter(&p_pcb);    wm.addParameter(&p_alt);    wm.addParameter(&p_uplink);
    wm.addParameter(&p_sample);
    wm.addParameter(&p_winden);
    wm.addParameter(&p_rainen); wm.addParameter(&p_lipo);   wm.addParameter(&p_debug); wm.addParameter(&p_cpu);

    wm.addParameter(&p_net_section);
    wm.addParameter(&p_wifien);   wm.addParameter(&p_devname);
    wm.addParameter(&p_webapien); wm.addParameter(&p_webadminpw);
    wm.addParameter(&p_mqtten);   wm.addParameter(&p_mqttctrlen);
    wm.addParameter(&p_mqtthost); wm.addParameter(&p_mqttport); wm.addParameter(&p_mqttuser);
    wm.addParameter(&p_mqttpass); wm.addParameter(&p_mqtttopic);wm.addParameter(&p_mqtttls);
    wm.addParameter(&p_cacert_html);

    // Safety net for an abandoned portal session: WiFiManager's default
    // _webClientCheck=true means this counts down from the *last* HTTP
    // request the portal received, not from when the AP started — so an
    // actively-used session (browsing between pages, filling in the form)
    // never trips it; it only fires after 3 minutes of total silence (e.g.
    // the user wandered off mid-setup). Once it fires, startConfigPortal()
    // returns via the same path as a normal exit, so whatever was already
    // saved via setSaveParamsCallback survives; only later, unsaved edits
    // in an open-but-not-submitted form are lost.
    wm.setConfigPortalTimeout(180);

    // Update display once the AP is up and the IP address is known
    wm.setAPCallback([](WiFiManager*) {
        String ip = WiFi.softAPIP().toString();
        display.clear();
        display.drawString(0,  0, "Config Portal");
        display.drawString(0, 11, "SSID: WeatherNode");
        display.drawString(0, 22, "http://" + ip);
        display.drawString(0, 33, "OTA:  weather-node");
        display.drawString(0, 44, "1. Connect to SSID");
        display.drawString(0, 55, "2. Open URL in browser");
        display.display();
    });

    // ArduinoOTA running in parallel to the portal in its own task (firmware update without USB)
    ArduinoOTA.setHostname("weather-node");
    ArduinoOTA.onStart([]()  { dbgSerial.println("OTA: Start"); });
    ArduinoOTA.onEnd([]()    { dbgSerial.println("OTA: Done"); });
    ArduinoOTA.onError([](ota_error_t e) { dbgSerial.printf("OTA Error %u\n", e); });
    ArduinoOTA.begin();
    TaskHandle_t otaHandle = nullptr;
    xTaskCreate([](void*) {
        for (;;) { ArduinoOTA.handle(); vTaskDelay(10 / portTICK_PERIOD_MS); }
    }, "OTA", 4096, nullptr, 1, &otaHandle);

    wm.startConfigPortal("WeatherNode");

    if (otaHandle) { vTaskDelete(otaHandle); ArduinoOTA.end(); }

    return buildConfig();
}
