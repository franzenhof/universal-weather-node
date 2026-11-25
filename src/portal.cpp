#include "portal.h"
#include "portal_pages.h"
#include "system_restart.h"
#include "wind_window.h"
#include "globals.h"
#include "sensors.h"
#include "board_io.h"
#include "sources.h"
#include "generated_version.h"
#include "decoder_js.h"
#include "readme_md.h"
#include <new>
#include <memory>
#include "net_check.h"
#include "gz_send.h"
#include "help_page.h"
#include "calibrate_page.h"
#include "heltec_board.h"
#include "serial_log.h"
#include "loxone.h"
#include "api_info.h"
#include "i18n.h"

#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "ota_upload.h"
#include "oled_screens.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// WiFiManager inserts parameter values unescaped into value='...': a ' in
// a stored value (password, device name, ...) cut it off on the next save.
// Values are therefore shown HTML-escaped; the browser decodes the entities
// and submits the original text.
static String htmlAttr(const char* s)
{
    String out;
    for (; *s; s++) {
        switch (*s) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += *s;       break;
        }
    }
    return out;
}

// Firmware upload page for /update (see below).
// A template for sendI18nTemplate(): @N@ = OTA_TEXTS[N].
static const TextId OTA_TEXTS[] = { T_ota_title, T_ota_hint, T_ota_update, T_ota_back };
static const char OTA_PAGE_HTML[] PROGMEM =
    "<!DOCTYPE html><html lang='@L@'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>@0@</title></head><body style='font-family:sans-serif;max-width:480px;margin:1rem auto;padding:0 1rem;'>"
    "<h3>@0@</h3><form method='POST' action='/u' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin,application/octet-stream'><br/><small>@1@</small><br/><br/>"
    "<button type='submit'>@2@</button></form><p><a href='/'>@3@</a></p></body></html>";

// ============================================================================
// WI-FI CONFIGURATION PORTAL
// ============================================================================
// Our own portal pages (/update, /calibrate): a PROGMEM template with
// @N@ = texts[N] (HTML), @jN@ = texts[N] as a JavaScript string, @L@ = the
// language code; streamed in chunks.
static void sendI18nTemplate(WebServer& srv, const char* tpl, const TextId* texts, size_t n)
{
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.send(200, "text/html; charset=utf-8", "");
    String out;
    auto flush = [&]() { if (out.length()) { srv.sendContent(out); out = String(); } };
    for (const char* p = tpl; *p; p++) {
        if (*p == '@') {
            const char* q = p + 1;
            bool js = *q == 'j';
            if (js) q++;
            if (q[0] == 'L' && q[1] == '@') { out += langCode(i18nCurrent()); p = q + 1; continue; }
            if (isdigit((unsigned char)*q)) {
                size_t k = (size_t)atoi(q);
                while (isdigit((unsigned char)*q)) q++;
                if (*q == '@' && k < n) {
                    const char* t = T(texts[k]);
                    if (!js) out += t;
                    else {
                        out += '\'';
                        for (; *t; t++) { if (*t == '\'' || *t == '\\') out += '\\'; out += *t; }
                        out += '\'';
                    }
                    p = q;
                    continue;
                }
            }
        }
        out += *p;
        if (out.length() >= 512) flush();
    }
    flush();
    srv.sendContent("");
}

// ---- Shared with the web interface (portal_pages.h) ----
void portalSendDecoder(WebServer& srv)
{
    srv.sendHeader("Content-Disposition", "attachment; filename=\"weather-node-decoder.js\"");
    srv.send_P(200, "application/javascript; charset=utf-8", DECODER_JS);
}

void portalSendCalibratePage(WebServer& srv)
{
    sendI18nTemplate(srv, CALIBRATE_PAGE_HTML, CALIBRATE_TEXTS, sizeof(CALIBRATE_TEXTS) / sizeof(CALIBRATE_TEXTS[0]));
}

void portalSendCalibrateSample(WebServer& srv)
{
    // Heltec V4: GPIO2 is the RF front end's enable - never read it as the vane
    int adc = boardPcbAllowed() ? adcAverage(windDirPin()) : 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"adc\":%d}", adc);
    srv.send(200, "application/json", buf);
}

bool portalHandleCalibrateSave(WebServer& srv)
{
    JsonDocument doc;
    if (deserializeJson(doc, srv.arg("plain"))) {
        srv.send(400, "text/plain", "Invalid JSON");
        return false;
    }
    // on the heap: Config is ~7 KB, this also runs on the 8 KB web task (web_api.cpp)
    std::unique_ptr<Config> owner(new (std::nothrow) Config());
    if (!owner) {
        srv.send(503, "text/plain", "low memory, try again");
        return false;
    }
    Config& c = *owner;
    loadConfigInto(c);
    int n = 0;
    for (JsonObject p : doc["positions"].as<JsonArray>()) {
        if (n >= 16) break;
        c.windDirCalAdc[n] = (uint16_t)constrain(p["adc"].as<int>(), 0, 4095);
        c.windDirCalDeg[n] = (uint16_t)(((p["deg"].as<int>() % 360) + 360) % 360);
        n++;
    }
    if (n == 0) {
        srv.send(400, "text/plain", "No positions received");
        return false;
    }
    c.windDirCalCount   = n;
    c.windDirSensorType = 1;
    saveConfig(c);
    // Keep the in-memory gCfg in sync too: the web interface keeps running,
    // and so may the portal (calibration re-run from the button-triggered
    // portal) without an immediate restart afterward.
    gCfg.windDirCalCount   = c.windDirCalCount;
    gCfg.windDirSensorType = c.windDirSensorType;
    memcpy(gCfg.windDirCalAdc, c.windDirCalAdc, sizeof(gCfg.windDirCalAdc));
    memcpy(gCfg.windDirCalDeg, c.windDirCalDeg, sizeof(gCfg.windDirCalDeg));
    srv.send(200, "text/plain; charset=utf-8", TF(T_cal_saved, String(n)));
    return true;
}

void runConfigPortal(Config& cfg)
{
    // Config is ~7 KB: no copy of it on this (loop task) stack - the portal
    // path (setup() -> openPortalSaveAndRestart() -> here -> WiFiManager ->
    // the save callback) once held four and overflowed the 24 KB stack on
    // the first start of a new node. The result goes straight into cfg (gCfg),
    // the snapshot lives on the heap for the portal session only.
    const Config& cur = cfg;
    // Portal language: the setting; "Auto" = English here (the parameter
    // labels are fixed before any browser request).
    i18nSet(cur.uiLang > 0 && cur.uiLang <= LANG_COUNT ? (uint8_t)(cur.uiLang - 1) : (uint8_t)LANG_EN);
    // Frozen copy of the config as it was when the portal was opened. cur
    // itself is a reference to gCfg (openPortalSaveAndRestart() calls this
    // as runConfigPortal(gCfg)), which the immediate-persist save in
    // setSaveParamsCallback below reassigns — buildConfig() uses this
    // snapshot as its baseline instead, so a second param-only save in the
    // same session doesn't build on top of the first save's result.
    std::unique_ptr<Config> snapshotOwner(new (std::nothrow) Config(cur));
    if (!snapshotOwner) {
        dbgSerial.println("Portal: not enough memory to start - restarting.");
        delay(1000);
        ESP.restart();
    }
    const Config& curSnapshot = *snapshotOwner;

    // Set by /calibrate/save below. The calibration is persisted (and copied
    // into gCfg) right there, but buildConfig() starts from curSnapshot and
    // the form's sensor-type field still shows the pre-calibration value -
    // without this, closing the portal would write the old table back.
    bool calibratedThisSession = false;

    // Admin password as stored when the portal opened: protects ArduinoOTA
    // and the browser firmware upload (/update) below. static: used from the
    // web server/OTA callbacks, runConfigPortal() is never reentered.
    static char otaPass[sizeof(cur.webAdminPass)];
    strlcpy(otaPass, cur.webAdminPass, sizeof(otaPass));

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

    char b_dev[17], b_join[17], b_region[8], b_pcb[4], b_alt[8], b_uplink[8], b_sample[4], b_mqttsec[8], b_lorawagg[2], b_mqttwagg[2], b_rfen[2], b_rfproto[3], b_windoff[6], b_windunit[2], b_debug[2], b_cpu[4], b_payfmt[2], b_unitsys[2];
    char b_rainmmtip[12], b_windmspp[12], b_winddirtyp[2];
    char b_loraen[2], b_lowpower[2], b_vanesave[2];
    itoa(cur.loraEn,          b_loraen,  10);
    itoa(cur.lowPowerEn,      b_lowpower, 10);
    itoa(cur.vanePowerSave,   b_vanesave, 10);
    uint64ToHex(cur.devEui, b_dev, sizeof(b_dev));
    uint64ToHex(cur.joinEui, b_join, sizeof(b_join));
    strlcpy(b_region, cur.region, sizeof(b_region));
    itoa(cur.pcbVersion,       b_pcb,     10);
    itoa(cur.altitudeM,        b_alt,     10);
    itoa(cur.uplinkIntervalSec, b_uplink,  10);
    itoa(cur.sampleSec,        b_sample,  10);
    itoa(cur.mqttIntervalSec,  b_mqttsec, 10);
    itoa(cur.loraWindAgg,      b_lorawagg, 10);
    itoa(cur.mqttWindAgg,      b_mqttwagg, 10);
    itoa(cur.rfEn,             b_rfen,    10);
    itoa(cur.rfProto,          b_rfproto, 10);
    // value sources as one field: "air,wind,rain,uv,light,pressure" (0 auto, 1 board, 2-5 wireless 1-4, 6 off;
    // a value left out keeps its setting, so the old 5-value form still works)
    char b_srcs[24];   // six values up to 34
    snprintf(b_srcs, sizeof(b_srcs), "%u,%u,%u,%u,%u,%u", cur.srcSel[0], cur.srcSel[1], cur.srcSel[2], cur.srcSel[3], cur.srcSel[4], cur.srcSel[5]);
    itoa(cur.windDirOffsetDeg, b_windoff, 10);
    itoa(cur.windUnitMs,       b_windunit, 10);
    itoa(cur.debugMode,        b_debug,   10);
    itoa(cur.cpuFreqMhz,       b_cpu,     10);
    itoa(cur.payloadFormat,    b_payfmt,  10);
    itoa(cur.unitSystem,       b_unitsys, 10);
    dtostrf(cur.rainMmPerTip,  1, 4, b_rainmmtip);
    dtostrf(cur.windMsPerPps,  1, 4, b_windmspp);
    itoa(cur.windDirSensorType, b_winddirtyp, 10);

    // PCB v2.0 I/O block fields
    char b_wind2[2], b_wind2off[6];
    itoa(cur.wind2Mode,         b_wind2,    10);
    itoa(cur.wind2DirOffsetDeg, b_wind2off, 10);
    char b_auxainen[2], b_auxainscale[12], b_auxpulseen[2], b_heatmode[2], b_heatduty[4], b_heatbelow[8], b_heatminduty[4], b_heatfull[8];
    char b_fanmode[2], b_fanduty[4], b_fanabove[8], b_fanminpv[6], b_tachoen[2], b_tachoppr[2], b_vedirecten[2], b_outminbatt[6];
    itoa(cur.auxAinEn,      b_auxainen,   10);
    dtostrf(cur.auxAinScale, 1, 3, b_auxainscale);
    itoa(cur.auxPulseEn,    b_auxpulseen, 10);
    itoa(cur.heaterMode,    b_heatmode,   10);
    itoa(cur.heaterDutyPct, b_heatduty,   10);
    dtostrf(cur.heaterOnBelowDc / 10.0f, 1, 1, b_heatbelow);
    itoa(cur.heaterMinDutyPct, b_heatminduty, 10);
    dtostrf(cur.heaterFullBelowDc / 10.0f, 1, 1, b_heatfull);
    itoa(cur.fanMode,       b_fanmode,    10);
    itoa(cur.fanDutyPct,    b_fanduty,    10);
    dtostrf(cur.fanOnAboveDc / 10.0f, 1, 1, b_fanabove);
    itoa(cur.fanMinPvW,     b_fanminpv,   10);
    itoa(cur.tachoEn,       b_tachoen,    10);
    itoa(cur.tachoPpr,      b_tachoppr,   10);
    itoa(cur.veDirectEn,    b_vedirecten, 10);
    dtostrf(cur.outMinBattDv / 10.0f, 1, 1, b_outminbatt);
    char b_uven[2], b_uvscale[12], b_uvindexen[2], b_uvcal[2], b_uvslope[12], b_uvoffset[12];
    itoa(cur.uvEn, b_uven, 10);
    dtostrf(cur.uvAinScale, 1, 5, b_uvscale);
    itoa(cur.uvIndexEn, b_uvindexen, 10);
    itoa(cur.uvCalibrated, b_uvcal, 10);
    dtostrf(cur.uvIndexSlope, 1, 3, b_uvslope);
    dtostrf(cur.uvIndexOffset, 1, 3, b_uvoffset);

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
    // Both buffers (~4.4 KB) come from the heap only while the portal runs -
    // not static RAM, not this function's stack frame (runConfigPortal() is
    // never called reentrantly; the lambdas below use them through the
    // static pointers).
    constexpr size_t CACERT_HTML_LEN = sizeof(cur.mqttCaCert) + 300, CACERT_LEN = sizeof(cur.mqttCaCert);
    static char* b_cacert_html = nullptr;
    static char* capturedCaCert = nullptr;
    if (!b_cacert_html) b_cacert_html = (char*)malloc(CACERT_HTML_LEN);
    if (!capturedCaCert) capturedCaCert = (char*)calloc(1, CACERT_LEN);
    static char noCaField[1] = "";
    if (!b_cacert_html || !capturedCaCert) { free(b_cacert_html); free(capturedCaCert); b_cacert_html = capturedCaCert = nullptr; }
    if (b_cacert_html) snprintf(b_cacert_html, CACERT_HTML_LEN,
             "<label>%s</label><br/>"
             "<textarea name='mqttcacert' id='mqttcacert' rows='8' cols='48' "
             "placeholder='-----BEGIN CERTIFICATE-----'>%s</textarea><br/>", T(T_pt_cacert),
             cur.mqttCaCert);
    bool capturedCaCertSet = false;
    // Set once the parameter form was submitted (setSaveParamsCallback). Until
    // then the text/secret parameters hold their display values (escaped,
    // empty secrets, checkbox values) - buildConfig() keeps the stored ones.
    bool paramsSubmitted = false;

    // Static IP fields ("ip"/"gw"/"sn"/"dns") are WiFiManager's own built-in
    // form fields on the "Configure WiFi" page, not WiFiManagerParameters, so
    // they're captured the same way as the CA cert: read directly from
    // wm.server in a callback below.
    static char capturedStaticIp[16];
    static char capturedStaticGw[16];
    static char capturedStaticSn[16];
    static char capturedStaticDns[16];
    bool capturedStaticIpSet = false;

    WiFiManagerParameter p_lora_section(T(T_pt_lorawan_optional_only_for_sending));
    WiFiManagerParameter p_loraen("loraen", T(T_pt_lorawan_transport_enabled_0_no), b_loraen, 2);
    WiFiManagerParameter p_lowpower("lowpower", T(T_pt_low_power_mode_light_sleep), b_lowpower, 2);
    WiFiManagerParameter p_vanesave("vanesave", T(T_pt_low_power_mode_switch_the), b_vanesave, 2);
    WiFiManagerParameter p_dev    ("dev",      T(T_pt_deveui_16_hex_chars),                        b_dev,     17);
    WiFiManagerParameter p_join   ("join",     T(T_pt_joineui_16_hex_chars),                       b_join,    17);
    // Secrets are never prefilled (the portal AP is open): empty field, an
    // empty submission keeps the stored value.
    WiFiManagerParameter p_key    ("key",      T(T_pt_appkey_32_hex_chars_leave), "", 33,
                                   "placeholder='(unchanged)' autocomplete='off'");
    WiFiManagerParameter p_region ("region",   T(T_pt_lorawan_region_eu868_us915_au915), b_region, 8);
    WiFiManagerParameter p_pcb    ("pcbver",   T(T_pt_pcb_revision_12_rev1_2), b_pcb, 3);
    char b_board[2], l_board[160];
    itoa(cur.boardSel, b_board, 10);
    strlcpy(l_board, (TF(T_pt_board, boardName(gBoard.detected), boardName(gBoard.kind)) +
                      (boardIsR8() ? String(T(T_pt_v4_r8_not_supported)) : String())).c_str(), sizeof(l_board));
    WiFiManagerParameter p_board  ("board",    l_board, b_board, 2);
    WiFiManagerParameter p_alt    ("altitude", T(T_pt_altitude_above_sea_level_in),       b_alt,     6);
    WiFiManagerParameter p_uplink ("uplinksec",T(T_pt_lorawan_measurement_interval_in_s),  b_uplink, 7);
    WiFiManagerParameter p_sample ("samplesec",T(T_pt_wind_sample_interval_in_s),            b_sample, 4);
    WiFiManagerParameter p_lorawagg("lorawagg",T(T_pt_lorawan_wind_values_0_interval), b_lorawagg, 2);
    WiFiManagerParameter p_mqttsec ("mqttsec", T(T_pt_mqtt_interval_in_s_0), b_mqttsec, 7);
    WiFiManagerParameter p_mqttwagg("mqttwagg",T(T_pt_mqtt_wind_values_0_interval), b_mqttwagg, 2);
    WiFiManagerParameter p_rfen    ("rfen",    T(T_pt_wireless_sensors_868_mhz_1), b_rfen, 2);
    WiFiManagerParameter p_rfproto ("rfproto", T(T_pt_wireless_sensor_type_16_auto), b_rfproto, 2);
    // Free-text fields: shown HTML-escaped (htmlAttr), so the field length is
    // at least the escaped length; Config's own buffer limits the saved text.
    auto shownLen = [](int maxLen, const String& shown) { return max(maxLen, (int)shown.length()); };
    const String e_rfids = htmlAttr(cur.rfIds);
    WiFiManagerParameter p_rfids   ("rfids",   T(T_pt_wireless_sensors_mine_hex_ids), e_rfids.c_str(), shownLen(sizeof(cur.rfIds) - 1, e_rfids));
    const String e_rfignore = htmlAttr(cur.rfIgnore);
    WiFiManagerParameter p_rfignore("rfignore", T(T_pt_hidden_wireless_sensors_hex_ids), e_rfignore.c_str(), shownLen(sizeof(cur.rfIgnore) - 1, e_rfignore));
    char b_rfonly[4];
    itoa(cur.rfOnlyMine, b_rfonly, 10);
    WiFiManagerParameter p_rfonly  ("rfonly",  T(T_pt_only_my_sensors_1_accept), b_rfonly, 1);
    WiFiManagerParameter p_srcs    ("srcs",    T(T_pt_value_sources_temp_hum_wind), b_srcs, 23);
    WiFiManagerParameter p_windoff("windoff",  T(T_pt_wind_direction_offset_in_degrees), b_windoff, 5);

    // Units (in "Basic setup"): one master switch (Metric/Imperial) plus the
    // km/h-vs-m/s refinement, which only applies when Units = Metric.
    WiFiManagerParameter p_unitsys ("unitsys",  T(T_pt_units_0_metric_c_mm), b_unitsys, 2);
    WiFiManagerParameter p_windunit("windunit", T(T_pt_wind_speed_unit_if_metric), b_windunit, 2);

    // Generic (non-Davis) sensor calibration. Rain/wind-speed are simple
    // scale factors; wind direction additionally supports a calibrated
    // lookup table for reed-switch/resistor-ladder vanes (which don't
    // output a continuous voltage) — see the "Calibrate Wind Direction"
    // button on the portal start page.
    WiFiManagerParameter p_cal_section(T(T_pt_sensor_calibration_defaults_match_the));
    WiFiManagerParameter p_rainmmtip("rainmmtip", T(T_pt_rain_mm_per_tip_pulse),                     b_rainmmtip, 11);
    WiFiManagerParameter p_windmspp ("windmspp",  T(T_pt_wind_speed_m_s_per),       b_windmspp,  11);
    WiFiManagerParameter p_winddirtyp("winddirtyp", T(T_pt_wind_direction_sensor_0_potentiometer), b_winddirtyp, 2);

    WiFiManagerParameter p_winden ("winden",   T(T_pt_wind_sensor_on_the_pcb), cur.windEn ? "1" : "0", 2);
    WiFiManagerParameter p_rainen ("rainen",   T(T_pt_rain_sensor_on_the_pcb), cur.rainEn ? "1" : "0", 2);
    WiFiManagerParameter p_rainpersist("rainpersist", T(T_pt_rain_statistics_today_yesterday_last), cur.rainPersist ? "1" : "0", 2);
    WiFiManagerParameter p_lipo   ("lipoen",   T(T_pt_lipo_measurement_enabled_0_no),       cur.lipoEn ? "1" : "0", 2);
    WiFiManagerParameter p_debug  ("debugmode",T(T_pt_debug_mode_0_no_1), b_debug, 2);
    WiFiManagerParameter p_cpu    ("cpufreq",  T(T_pt_cpu_frequency_in_mhz_80),             b_cpu,     4);
    WiFiManagerParameter p_payfmt ("payfmt",   T(T_pt_lorawan_payload_format_1_compact),
                                   b_payfmt, 2);
    // PCB v2.0 I/O block, all off by default; ignored on Rev 1.2/1.3.
    WiFiManagerParameter p_io_section(gIo.hwIdDetected
        ? T(T_pt_pcb_v2_0_inputs_outputs)
        : T(T_pt_pcb_v2_0_inputs_outputs2));
    WiFiManagerParameter p_auxainen  ("auxainen",   T(T_pt_aux_analog_input_gpio4_0),                    b_auxainen,   2);
    WiFiManagerParameter p_auxainscale("auxainscale",T(T_pt_aux_analog_scale_1_direct), b_auxainscale, 11);
    WiFiManagerParameter p_auxpulseen("auxpulseen", T(T_pt_aux_pulse_counter_gpio48_0),                  b_auxpulseen, 2);
    WiFiManagerParameter p_wind2     ("wind2",      T(T_pt_2nd_wind_sensor_on_aux), b_wind2, 2);
    WiFiManagerParameter p_wind2off  ("wind2off",   T(T_pt_2nd_wind_vane_offset_in),          b_wind2off,   5);
    WiFiManagerParameter p_heatmode  ("heatmode",   T(T_pt_heater_j12_0_off_1),        b_heatmode,   2);
    WiFiManagerParameter p_heatduty  ("heatduty",   T(T_pt_heatduty), b_heatduty, 4);
    WiFiManagerParameter p_heatbelow ("heatbelow",  T(T_pt_heater_auto_on_below_c),                b_heatbelow,  7);
    WiFiManagerParameter p_heatminduty("heatminduty",T(T_pt_heatminduty),   b_heatminduty, 4);
    WiFiManagerParameter p_heatfull  ("heatfull",   T(T_pt_heater_auto_full_power_at),                 b_heatfull,   7);
    WiFiManagerParameter p_fanmode   ("fanmode",    T(T_pt_fan_j13_0_off_1),           b_fanmode,    2);
    WiFiManagerParameter p_fanduty   ("fanduty",    T(T_pt_fanduty),                             b_fanduty,    4);
    WiFiManagerParameter p_fanabove  ("fanabove",   T(T_pt_fan_auto_on_from_c),                    b_fanabove,   7);
    WiFiManagerParameter p_fanminpv  ("fanminpv",   T(T_pt_fan_auto_minimum_pv_w), b_fanminpv,   5);
    WiFiManagerParameter p_tachoen   ("tachoen",    T(T_pt_fan_tacho_gpio40_0_off),                          b_tachoen,    2);
    WiFiManagerParameter p_tachoppr  ("tachoppr",   T(T_pt_tacho_pulses_per_revolution_pc),                b_tachoppr,   2);
    WiFiManagerParameter p_vedirecten("vedirecten", T(T_pt_victron_ve_direct_0_off),                         b_vedirecten, 2);
    WiFiManagerParameter p_outminbatt("outminbatt", T(T_pt_heater_fan_minimum_battery_v),  b_outminbatt, 5);
    WiFiManagerParameter p_uven("uven", T(T_pt_uv_sensor_on_gpio3_0), b_uven, 2);
    WiFiManagerParameter p_uvscale("uvscale", T(T_pt_uv_divider_scale_terminal_v), b_uvscale, 11);
    WiFiManagerParameter p_uvindexen("uvindexen", T(T_pt_publish_calculated_uv_index_0), b_uvindexen, 2);
    WiFiManagerParameter p_uvcal("uvcal", T(T_pt_uv_calibration_explicitly_verified_0), b_uvcal, 2);
    WiFiManagerParameter p_uvslope("uvslope", T(T_pt_uv_index_calibration_slope_index), b_uvslope, 11);
    WiFiManagerParameter p_uvoffset("uvoffset", T(T_pt_uv_index_calibration_offset_10), b_uvoffset, 11);

    const String s_section = String("<div style='margin:0.5rem 0 0.75rem 0;'><h3>") + T(T_pt_section_title) +
                             "</h3><p style='margin:0 0 0.35rem 0;'>" + TF(T_pt_fw_version, FIRMWARE_VERSION) +
                             "</p><p style='margin:0;'>" + T(T_pt_section_text) + "</p></div>";
    WiFiManagerParameter p_section(s_section.c_str());
    char b_uilang[3];
    itoa(cur.uiLang, b_uilang, 10);
    WiFiManagerParameter p_uilang ("uilang",   T(T_pt_language), b_uilang, 2);
    WiFiManagerParameter p_basic_section(T(T_pt_basic_setup));
    WiFiManagerParameter p_board_section(T(T_pt_board_sensors_and_measurement_pcb));

    // Parallel WiFi + MQTT transport, fully optional alongside LoRaWAN. WiFi
    // STA credentials/static-IP are configured via WiFiManager's own "wifi"
    // menu, not here.
    WiFiManagerParameter p_net_section(T(T_pt_wifi_mqtt_parameter_wifi_network));
    WiFiManagerParameter p_wifien    ("wifien",     T(T_pt_wifi_enabled_0_no_1), b_wifien, 2);
    const String e_devname = htmlAttr(cur.deviceName), e_mqtthost = htmlAttr(cur.mqttHost),
                 e_mqttuser = htmlAttr(cur.mqttUser), e_mqtttopic = htmlAttr(cur.mqttTopic);
    WiFiManagerParameter p_devname   ("devname",    T(T_pt_device_name_wifi_hostname_mqtt), e_devname.c_str(), shownLen(33, e_devname));
    WiFiManagerParameter p_webapien  ("webapien",   T(T_pt_web_rest_api_settings_page), b_webapien, 2);
    WiFiManagerParameter p_webadminpw("webadminpw", T(T_pt_admin_password_for_the_web), "", 33,
                                      cur.webAdminPass[0] ? "type='password' autocomplete='new-password' placeholder='(unchanged)'"
                                                          : "type='password' autocomplete='new-password' placeholder='(not set)'");
    WiFiManagerParameter p_webadminpwclr("webadminpwclr", T(T_pt_remove_the_stored_admin_password), "1", 2,
                                         "type='checkbox'", WFM_LABEL_AFTER);
    WiFiManagerParameter p_mqtten    ("mqtten",     T(T_pt_mqtt_publish_control_enabled_0),   b_mqtten,     2);
    WiFiManagerParameter p_mqttctrlen("mqttctrlen", T(T_pt_mqtt_command_topic_enabled_0),     b_mqttctrlen, 2);
    WiFiManagerParameter p_mqtthost  ("mqtthost",   T(T_pt_mqtt_broker_host),                             e_mqtthost.c_str(), shownLen(65, e_mqtthost));
    WiFiManagerParameter p_mqttport  ("mqttport",   T(T_pt_mqtt_broker_port_standard_1883), b_mqttport, 7);
    WiFiManagerParameter p_mqttuser  ("mqttuser",   T(T_pt_mqtt_username_optional),                     e_mqttuser.c_str(), shownLen(33, e_mqttuser));
    WiFiManagerParameter p_mqttpass  ("mqttpass",   T(T_pt_mqtt_password_optional_leave_empty), "", 33,
                                      cur.mqttPass[0] ? "type='password' autocomplete='new-password' placeholder='(unchanged)'"
                                                      : "type='password' autocomplete='new-password' placeholder='(not set)'");
    WiFiManagerParameter p_mqttpassclr("mqttpassclr", T(T_pt_remove_the_stored_mqtt_password), "1", 2, "type='checkbox'", WFM_LABEL_AFTER);
    WiFiManagerParameter p_mqtttopic ("mqtttopic",  T(T_pt_mqtt_topic_prefix_optional_default), e_mqtttopic.c_str(), shownLen(65, e_mqtttopic));
    WiFiManagerParameter p_mqtttls   ("mqtttls",    T(T_pt_mqtt_security_0_plain_no), b_mqtttls, 2);
    WiFiManagerParameter p_cacert_html(b_cacert_html ? b_cacert_html : noCaField);
    // Loxone UDP output (loxone.h)
    char b_loxen[2], b_loxport[8], b_loxsec[8];
    itoa(cur.loxEn,   b_loxen,   10);
    itoa(cur.loxPort, b_loxport, 10);
    itoa(cur.loxSec,  b_loxsec,  10);
    const String e_loxhost = htmlAttr(cur.loxHost), e_loxprefix = htmlAttr(cur.loxPrefix);
    WiFiManagerParameter p_loxen    ("loxen",     T(T_pt_loxone_udp_output_0_no), b_loxen, 2);
    WiFiManagerParameter p_loxhost  ("loxhost",   T(T_pt_loxone_miniserver_ip_or_host), e_loxhost.c_str(), shownLen(65, e_loxhost));
    WiFiManagerParameter p_loxport  ("loxport",   T(T_pt_loxone_udp_port_1_65535), b_loxport, 7);
    WiFiManagerParameter p_loxprefix("loxprefix", T(T_pt_loxone_name_prefix_optional_max), e_loxprefix.c_str(), shownLen(17, e_loxprefix));
    WiFiManagerParameter p_loxsec   ("loxsec",    T(T_pt_loxone_interval_in_s_0), b_loxsec, 7);

    WiFiManager wm;
    const char* menu[] = { "wifi", "info", "custom", "sep", "exit" };
    wm.setMenu(menu, 5);
    // kept alive until the portal ends (WiFiManager stores the pointer)
    static String menuHtml;
    menuHtml = String("<div style='margin:0.75rem 0 1rem 0;'><div style='font-size:0.85rem;opacity:0.75;margin:0 0 0.25rem 0;'>") +
               TF(T_pt_fw_version, FIRMWARE_VERSION) + "</div><form action='/param' method='get'><button>" + T(T_pt_section_title) +
               "</button></form><br/><form action='/decoder' method='get'><button>" + T(T_pt_menu_decoder) +
               "</button></form><br/><form action='/help' method='get'><button>" + T(T_c_help) +
               "</button></form><br/><form action='/calibrate' method='get'><button>" + T(T_pt_menu_calibrate) +
               "</button></form><br/><form action='/update' method='get'><button>" + T(T_pt_menu_update) + "</button></form></div>";
    wm.setCustomMenuHTML(menuHtml.c_str());

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
        // /help.md is gzip (gz_send.h): it needs the client's Accept-Encoding
        static const char* hdrs[] = { "Accept-Encoding" };
        wm.server->collectHeaders(hdrs, 1);
        // Firmware upload, always our own handlers - registered before
        // WiFiManager's /update and /u (this callback runs first, the first
        // matching handler wins): Digest auth when an admin password is set.
        // OtaUploadFilter (ota_check.h) takes the application image or the
        // -factory image (of which only the application part is written, if
        // its partition table matches this node's); anything else is refused
        // before a byte is written, so a wrong file can't leave a node that
        // doesn't boot.
        // The streaming part is shared with the web interface (ota_upload.h).
        {
            wm.server->on("/update", HTTP_ANY, [&wm]() {
                if (otaPass[0] && !wm.server->authenticate("admin", otaPass)) {
                    wm.server->requestAuthentication(DIGEST_AUTH, "WeatherNode firmware update");
                    return;
                }
                sendI18nTemplate(*wm.server, OTA_PAGE_HTML, OTA_TEXTS, sizeof(OTA_TEXTS) / sizeof(OTA_TEXTS[0]));
            });
            wm.server->on("/u", HTTP_POST, [&wm]() {
                const OtaOutcome o = otaUploadOutcome();
                if (o == OTA_OUT_NOT_AUTHORIZED || (o == OTA_OUT_NONE && otaPass[0])) {
                    otaUploadReset();
                    wm.server->requestAuthentication(DIGEST_AUTH, "WeatherNode firmware update");
                    return;
                }
                String msg = o == OTA_OUT_OK      ? String(T(T_ota_ok))
                           : o == OTA_OUT_REFUSED ? TF(T_ota_refused, otaUploadError())
                                                  : TF(T_ota_failed, o == OTA_OUT_NONE ? "no file" : otaUploadError());
                otaUploadReset();
                wm.server->send(o == OTA_OUT_OK ? 200 : 400, "text/plain; charset=utf-8", msg);
                if (o == OTA_OUT_OK) { delay(1000); systemRestart("firmware update (browser)"); }
            }, [&wm]() {
                HTTPUpload& up = wm.server->upload();
                if (up.status == UPLOAD_FILE_START)
                    otaUploadStart(otaEspOps(), !otaPass[0] || wm.server->authenticate("admin", otaPass), "Portal OTA");
                else if (up.status == UPLOAD_FILE_WRITE) otaUploadChunk(up.buf, up.currentSize);
                else if (up.status == UPLOAD_FILE_END)   otaUploadFinish();
                else if (up.status == UPLOAD_FILE_ABORTED) otaUploadAbort();
            });
        }
        // WiFiManager's own /restart (not in the menu, reachable by URL)
        // would call ESP.restart() directly: same purpose, but through
        // systemRestart() so the rain statistics are saved first.
        wm.server->on("/restart", [&wm]() {
            wm.server->send(200, "text/plain; charset=utf-8", "Restarting...");
            delay(1000);
            systemRestart("portal /restart");
        });
        wm.server->on("/decoder", [&wm]() { portalSendDecoder(*wm.server); });
        wm.server->on("/help", [&wm]() {
            wm.server->send_P(200, "text/html; charset=utf-8", HELP_PAGE_HTML);
        });
        wm.server->on("/help.md", [&wm]() {
            sendGzipAsset(*wm.server, README_MD_GZ, README_MD_GZ_LEN, README_MD_LEN, "text/plain; charset=utf-8");
        });
        wm.server->on("/calibrate", [&wm]() { portalSendCalibratePage(*wm.server); });
        wm.server->on("/calibrate/sample", HTTP_GET, [&wm]() { portalSendCalibrateSample(*wm.server); });
        wm.server->on("/calibrate/save", HTTP_POST, [&wm, &calibratedThisSession]() {
            if (portalHandleCalibrateSave(*wm.server)) calibratedThisSession = true;
        });
    });

    // Builds a full Config from the current state of every WiFiManagerParameter
    // plus the separately-captured CA-cert/static-IP fields. Shared by the
    // immediate-persist save below and the function's own return value, so
    // there's exactly one place that knows how to translate the portal's
    // form fields into a Config.
    auto buildConfig = [&](Config& c) {
        // curSnapshot, not cur: setSaveParamsCallback below assigns into
        // gCfg, which cur (a reference, since openPortalSaveAndRestart()
        // calls this as runConfigPortal(gCfg)) aliases — using cur directly
        // here would mean a second param-only save this session starts from
        // the first save's result instead of the original pre-portal state.
        c = curSnapshot;
        if (strlen(p_dev.getValue())  == 16) c.devEui  = hexToUint64(p_dev.getValue());
        if (strlen(p_join.getValue()) == 16) c.joinEui = hexToUint64(p_join.getValue());
        if (strlen(p_key.getValue())  == 32) hexToBytes(p_key.getValue(), c.appKey, 16);
        normalizeRegionInput(p_region.getValue(), c.region, sizeof(c.region));
        int pcb = atoi(p_pcb.getValue());
        c.pcbVersion       = (pcb == PCB_REV_1_2 || pcb == PCB_REV_2_0) ? pcb : PCB_REV_1_3;
        if (gIo.hwIdDetected) c.pcbVersion = PCB_REV_2_0;   // the ID divider wins
        int board = atoi(p_board.getValue());
        if (boardSelValid(board)) c.boardSel = board;

        // PCB v2.0 I/O block - out-of-range input keeps the previous value.
        auto intIn = [](WiFiManagerParameter& p, int lo, int hi, int keep) {
            const char* v = p.getValue();
            if (!v[0]) return keep;
            int n = atoi(v);
            return (n < lo || n > hi) ? keep : n;
        };
        auto dIn = [](WiFiManagerParameter& p, float lo, float hi, int keep) {   // °C / V -> x10
            const char* v = p.getValue();
            if (!v[0]) return keep;
            float f = atof(v);
            return (f < lo || f > hi) ? keep : (int)lroundf(f * 10.0f);
        };
        c.auxAinEn        = atoi(p_auxainen.getValue())   == 1 ? 1 : 0;
        float scale = atof(p_auxainscale.getValue());
        if (scale > 0.0f && scale <= 20.0f) c.auxAinScale = scale;
        c.auxPulseEn      = atoi(p_auxpulseen.getValue()) == 1 ? 1 : 0;
        c.wind2Mode       = constrain(atoi(p_wind2.getValue()), 0, 2);
        c.wind2DirOffsetDeg = constrain(atoi(p_wind2off.getValue()), -180, 180);
        if (c.wind2Mode >= 1) c.auxPulseEn = 0;   // the 2nd wind sensor uses the AUX inputs
        if (c.wind2Mode == 2) c.auxAinEn = 0;
        c.heaterMode      = intIn(p_heatmode, 0, 2, 0);
        c.heaterDutyPct   = intIn(p_heatduty, 1, 100, c.heaterDutyPct);
        c.heaterOnBelowDc = dIn(p_heatbelow, -40.0f, 40.0f, c.heaterOnBelowDc);
        c.heaterMinDutyPct = intIn(p_heatminduty, 1, 100, c.heaterMinDutyPct);
        c.heaterFullBelowDc = dIn(p_heatfull, -40.0f, 40.0f, c.heaterFullBelowDc);
        c.fanMode         = intIn(p_fanmode, 0, 2, 0);
        c.fanDutyPct      = intIn(p_fanduty, 1, 100, c.fanDutyPct);
        c.fanOnAboveDc    = dIn(p_fanabove, -40.0f, 60.0f, c.fanOnAboveDc);
        c.fanMinPvW       = intIn(p_fanminpv, 0, 5000, c.fanMinPvW);
        c.tachoEn         = atoi(p_tachoen.getValue())    == 1 ? 1 : 0;
        c.tachoPpr        = intIn(p_tachoppr, 1, 8, c.tachoPpr);
        c.veDirectEn      = atoi(p_vedirecten.getValue()) == 1 ? 1 : 0;
        c.outMinBattDv    = dIn(p_outminbatt, 0.0f, 60.0f, c.outMinBattDv);
        auto uvFloat = [](WiFiManagerParameter& p, float keep, float lo, float hi, const char* label) -> float {
            const char* text = p.getValue();
            char* end = nullptr;
            float value = strtof(text, &end);
            if (!text[0] || *end != '\0' || !isfinite(value) || value < lo || value > hi) {
                dbgSerial.printf("Config portal: invalid %s; keeping previous setting.\n", label);
                return keep;
            }
            return value;
        };
        auto uvBool = [](WiFiManagerParameter& p, int keep, const char* label) -> int {
            const char* value = p.getValue();
            if (!strcmp(value, "0") || !strcmp(value, "1")) return value[0] == '1';
            dbgSerial.printf("Config portal: invalid %s; keeping previous setting.\n", label);
            return keep;
        };
        c.uvEn = uvBool(p_uven, c.uvEn, "UV sensor enable");
        if (c.uvEn && c.pcbVersion < PCB_REV_2_0) {
            c.uvEn = 0;
            dbgSerial.println("Config portal: UV sensor rejected on Rev 1.x; PCB v2.0 required.");
        }
        c.uvAinScale = uvFloat(p_uvscale, c.uvAinScale, 1.0f, 5.0f, "UV divider scale");
        c.uvIndexEn = uvBool(p_uvindexen, c.uvIndexEn, "UV index enable");
        c.uvCalibrated = uvBool(p_uvcal, c.uvCalibrated, "UV calibration flag");
        c.uvIndexSlope = uvFloat(p_uvslope, c.uvIndexSlope, 0.001f, 30.0f, "UV index slope");
        c.uvIndexOffset = uvFloat(p_uvoffset, c.uvIndexOffset, -10.0f, 10.0f, "UV index offset");
        // >= 0, not > 0: 0 m (sea level) is a valid altitude and must not be
        // silently rejected in favor of the previously stored value.
        if (atoi(p_alt.getValue()) >= 0) c.altitudeM = atoi(p_alt.getValue());
        { int u = atoi(p_uplink.getValue()); if (u >= 5 && u <= 3600) c.uplinkIntervalSec = u; }
        int ss = atoi(p_sample.getValue());
        c.sampleSec        = (ss > 0) ? windSampleSecNormalized(ss) : c.sampleSec;
        c.loraWindAgg      = atoi(p_lorawagg.getValue()) == 1 ? 1 : 0;
        c.mqttWindAgg      = atoi(p_mqttwagg.getValue()) == 1 ? 1 : 0;
        int ms = atoi(p_mqttsec.getValue());
        c.mqttIntervalSec  = (ms == 0 || (ms >= 2 && ms <= 3600)) ? ms : c.mqttIntervalSec;
        { int v = atoi(p_rfen.getValue()); c.rfEn = v == RF_RADIO_SX1262 ? RF_RADIO_SX1262 : RF_RADIO_OFF; }
        { int v = atoi(p_rfproto.getValue()); if (rfProtoValid(v)) c.rfProto = v; }
        if (paramsSubmitted) {
            strlcpy(c.rfIds, p_rfids.getValue(), sizeof(c.rfIds));
            strlcpy(c.rfIgnore, p_rfignore.getValue(), sizeof(c.rfIgnore));
            int v = atoi(p_rfonly.getValue());
            if (v == 0 || v == 1) c.rfOnlyMine = v;
        }
        {
            const char* p = p_srcs.getValue();
            for (int i = 0; i < (int)sizeof(c.srcSel) && *p; i++) {
                int v = atoi(p);
                if (srcSelValid(v) && c.srcSel[i] != (uint8_t)v) {
                    c.srcSel[i] = (uint8_t)v;
                    c.srcId[i]  = 0;   // a new number chosen here: not the old binding any more
                }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        c.windDirOffsetDeg = constrain(atoi(p_windoff.getValue()), -180, 180);
        c.windUnitMs       = (atoi(p_windunit.getValue()) == 0) ? 0 : 1;
        c.unitSystem       = (atoi(p_unitsys.getValue()) == 1) ? 1 : 0;
        { int v = atoi(p_uilang.getValue()); if (v >= 0 && v <= LANG_COUNT) c.uiLang = v; }
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
        c.rainPersist      = atoi(p_rainpersist.getValue()) == 0 ? 0 : 1;
        c.lipoEn           = atoi(p_lipo.getValue());
        c.debugMode        = (atoi(p_debug.getValue()) == 1) ? 1 : 0;
        c.cpuFreqMhz       = (atoi(p_cpu.getValue()) == 240) ? 240 : 80;
        c.payloadFormat    = (atoi(p_payfmt.getValue()) == 1) ? 1 : 0;

        // Parallel WiFi + MQTT transport
        c.loraEn     = atoi(p_loraen.getValue()) ? 1 : 0;
        c.lowPowerEn = atoi(p_lowpower.getValue()) ? 1 : 0;
        c.vanePowerSave = atoi(p_vanesave.getValue()) ? 1 : 0;
        c.wifiEn     = atoi(p_wifien.getValue()) ? 1 : 0;
        if (paramsSubmitted) strlcpy(c.deviceName, p_devname.getValue(), sizeof(c.deviceName));
        // Static IP fields only come from the separate "Configure WiFi" page
        // (see setPreSaveConfigCallback below) — leave c.staticIpEn/staticIp
        // untouched (keep whatever cur/previous NVS state had) unless that
        // page was actually submitted this session, or a param-only save
        // would otherwise silently reset an existing static IP to DHCP.
        if (capturedStaticIpSet) {
            // a complete address only (an IP without a valid gateway was stored before and made
            // every later web save fail with "static IP: invalid address"); else DHCP
            if (ipValid(capturedStaticIp) && ipValid(capturedStaticGw) &&
                (!capturedStaticSn[0] || ipValid(capturedStaticSn)) && (!capturedStaticDns[0] || ipValid(capturedStaticDns))) {
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
        if (paramsSubmitted) {
            if (!strcmp(p_webadminpwclr.getValue(), "1")) c.webAdminPass[0] = '\0';
            else if (p_webadminpw.getValue()[0])          strlcpy(c.webAdminPass, p_webadminpw.getValue(), sizeof(c.webAdminPass));
        }
        c.mqttEn     = atoi(p_mqtten.getValue()) ? 1 : 0;
        c.mqttCtrlEn = atoi(p_mqttctrlen.getValue()) ? 1 : 0;
        if (paramsSubmitted) strlcpy(c.mqttHost, p_mqtthost.getValue(), sizeof(c.mqttHost));
        int mqttPortVal = atoi(p_mqttport.getValue());
        if (mqttPortVal > 0 && mqttPortVal <= 65535) c.mqttPort = mqttPortVal;
        if (paramsSubmitted) {
            strlcpy(c.mqttUser, p_mqttuser.getValue(), sizeof(c.mqttUser));
            if (!strcmp(p_mqttpassclr.getValue(), "1")) c.mqttPass[0] = '\0';
            else if (p_mqttpass.getValue()[0])          strlcpy(c.mqttPass, p_mqttpass.getValue(), sizeof(c.mqttPass));
            strlcpy(c.mqttTopic, p_mqtttopic.getValue(), sizeof(c.mqttTopic));
        }
        int mqttTlsVal = atoi(p_mqtttls.getValue());
        c.mqttTlsMode = (mqttTlsVal >= MQTT_TLS_NONE && mqttTlsVal <= MQTT_TLS_VERIFY) ? mqttTlsVal : MQTT_TLS_NONE;
        // Only overwrite the stored CA cert if the /param form was actually
        // submitted this session (setSaveParamsCallback fired) — otherwise a
        // portal session that never touches the params page (e.g. WiFi-only
        // reconfiguration) would silently wipe a previously stored certificate.
        if (capturedCaCertSet && capturedCaCert) strlcpy(c.mqttCaCert, capturedCaCert, sizeof(c.mqttCaCert));

        c.loxEn = atoi(p_loxen.getValue()) ? 1 : 0;
        { int v = atoi(p_loxport.getValue()); if (loxPortValid(v)) c.loxPort = v; }
        { int v = atoi(p_loxsec.getValue());  if (loxIntervalValid(v)) c.loxSec = v; }
        if (paramsSubmitted) {
            strlcpy(c.loxHost, p_loxhost.getValue(), sizeof(c.loxHost));
            if (loxPrefixValid(p_loxprefix.getValue())) strlcpy(c.loxPrefix, p_loxprefix.getValue(), sizeof(c.loxPrefix));
        }

    };

    // The CA-cert field is a raw <textarea>, not a normal id-based
    // WiFiManagerParameter, so it isn't populated by doParamSave() the usual
    // way. setSaveParamsCallback() fires right after params are saved, while
    // wm.server still has the submitted request's args available.
    wm.setSaveParamsCallback([&wm, &capturedCaCertSet, &paramsSubmitted, &buildConfig]() {
        String v = wm.server->arg("mqttcacert");
        if (capturedCaCert) {
            strlcpy(capturedCaCert, v.c_str(), CACERT_LEN);
            capturedCaCertSet = true;
        }
        paramsSubmitted   = true;

        // Persist immediately. The "Universal-Weather-Node Parameter" page
        // (/param) is a separate form from "Configure WiFi", and
        // startConfigPortal() only returns once THAT one is submitted — a
        // user who only needs to change parameters (the common case) would
        // otherwise see WiFiManager's own "Saved" confirmation, consider
        // themselves done, and never actually have anything written to NVS
        // at all until/unless they also submit the WiFi page.
        buildConfig(gCfg);
        dbgSerial.printf("Portal: settings saved (loop task stack: %u bytes never used).\n",
                         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
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

    // First-boot friendly order: the few fields a plain Wi-Fi station with a
    // wireless sensor may want first, LoRaWAN (optional) at the end.
    wm.addParameter(&p_section);
    wm.addParameter(&p_basic_section);
    wm.addParameter(&p_uilang);
    wm.addParameter(&p_devname);  wm.addParameter(&p_alt);
    wm.addParameter(&p_rfen);     wm.addParameter(&p_rfproto); wm.addParameter(&p_rfids); wm.addParameter(&p_rfignore); wm.addParameter(&p_rfonly);
    wm.addParameter(&p_unitsys);  wm.addParameter(&p_windunit);

    wm.addParameter(&p_board_section);
    wm.addParameter(&p_winden);   wm.addParameter(&p_rainen);  wm.addParameter(&p_rainpersist);
    wm.addParameter(&p_srcs);
    wm.addParameter(&p_pcb);      wm.addParameter(&p_board);      wm.addParameter(&p_uplink);
    wm.addParameter(&p_sample);   wm.addParameter(&p_lipo);    wm.addParameter(&p_debug); wm.addParameter(&p_cpu);

    wm.addParameter(&p_cal_section);
    wm.addParameter(&p_rainmmtip); wm.addParameter(&p_windmspp); wm.addParameter(&p_winddirtyp);
    wm.addParameter(&p_windoff);

    wm.addParameter(&p_io_section);
    wm.addParameter(&p_auxainen);  wm.addParameter(&p_auxainscale); wm.addParameter(&p_auxpulseen);
    wm.addParameter(&p_wind2);     wm.addParameter(&p_wind2off);
    wm.addParameter(&p_heatmode);  wm.addParameter(&p_heatduty);    wm.addParameter(&p_heatbelow);
    wm.addParameter(&p_heatminduty); wm.addParameter(&p_heatfull);
    wm.addParameter(&p_fanmode);   wm.addParameter(&p_fanduty);     wm.addParameter(&p_fanabove);
    wm.addParameter(&p_fanminpv);  wm.addParameter(&p_tachoen);     wm.addParameter(&p_tachoppr);
    wm.addParameter(&p_vedirecten); wm.addParameter(&p_outminbatt);
    wm.addParameter(&p_uven); wm.addParameter(&p_uvscale);
    wm.addParameter(&p_uvindexen); wm.addParameter(&p_uvcal);
    wm.addParameter(&p_uvslope); wm.addParameter(&p_uvoffset);

    wm.addParameter(&p_net_section);
    wm.addParameter(&p_wifien);
    wm.addParameter(&p_webapien); wm.addParameter(&p_webadminpw); wm.addParameter(&p_webadminpwclr);
    wm.addParameter(&p_mqtten);   wm.addParameter(&p_mqttctrlen);
    wm.addParameter(&p_mqtthost); wm.addParameter(&p_mqttport); wm.addParameter(&p_mqttuser);
    wm.addParameter(&p_mqttsec);  wm.addParameter(&p_mqttwagg);
    wm.addParameter(&p_mqttpass); wm.addParameter(&p_mqttpassclr);
    wm.addParameter(&p_mqtttopic);wm.addParameter(&p_mqtttls);
    wm.addParameter(&p_cacert_html);
    wm.addParameter(&p_loxen);    wm.addParameter(&p_loxhost); wm.addParameter(&p_loxport);
    wm.addParameter(&p_loxprefix); wm.addParameter(&p_loxsec);

    wm.addParameter(&p_lora_section);
    wm.addParameter(&p_loraen);
    wm.addParameter(&p_dev);    wm.addParameter(&p_join);   wm.addParameter(&p_key); wm.addParameter(&p_region);
    wm.addParameter(&p_payfmt); wm.addParameter(&p_lorawagg);
    wm.addParameter(&p_lowpower);
    wm.addParameter(&p_vanesave);

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
        display.drawString(0, OLED_ROW[1], "SSID: WeatherNode");
        display.drawString(0, OLED_ROW[2], "http://" + ip);
        display.drawString(0, OLED_ROW[3], "OTA:  weather-node");
        display.drawString(0, OLED_ROW[4], "1. Connect to SSID");
        display.drawString(0, OLED_ROW[5], "2. Open URL in browser");
        display.display();
    });

    // ArduinoOTA running in parallel to the portal in its own task (firmware
    // update without USB). The portal AP is open, so with an admin password
    // set, OTA needs it (espota/PlatformIO: --auth / upload_flags). Without
    // one, OTA stays open like before.
    ArduinoOTA.setHostname("weather-node");
    if (otaPass[0]) ArduinoOTA.setPassword(otaPass);
    ArduinoOTA.onStart([]()  { dbgSerial.println("OTA: Start"); });
    // No reboot inside ArduinoOTA: systemRestart() saves the rain statistics first.
    ArduinoOTA.setRebootOnSuccess(false);
    ArduinoOTA.onEnd([]()    { dbgSerial.println("OTA: Done"); systemRestart("firmware update (ArduinoOTA)"); });
    ArduinoOTA.onError([](ota_error_t e) { dbgSerial.printf("OTA Error %u\n", e); });
    ArduinoOTA.begin();
    TaskHandle_t otaHandle = nullptr;
    xTaskCreate([](void*) {
        for (;;) { ArduinoOTA.handle(); vTaskDelay(10 / portTICK_PERIOD_MS); }
    }, "OTA", 6144, nullptr, 1, &otaHandle);   // + NVS write of rainStatsFlush()

    // Same DHCP hostname as in normal operation (wifi_mqtt.cpp prepareSta())
    // when the portal connects to the entered network: the core applies it
    // when WiFiManager switches the STA mode on.
    char host[HOSTNAME_MAX_LEN + 1];
    deviceHostname(host, sizeof(host));
    WiFi.setHostname(host);

    dbgSerial.printf("Portal: starting (loop task stack: %u bytes never used so far).\n",
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    wm.startConfigPortal("WeatherNode");

    if (otaHandle) { vTaskDelete(otaHandle); ArduinoOTA.end(); }

    buildConfig(cfg);
    free(b_cacert_html);  b_cacert_html = nullptr;   // the portal's heap buffers
    free(capturedCaCert); capturedCaCert = nullptr;
}
