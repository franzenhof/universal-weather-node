#include "portal.h"
#include "system_restart.h"
#include "wind_window.h"
#include "globals.h"
#include "sensors.h"
#include "board_io.h"
#include "generated_version.h"
#include "decoder_js.h"
#include "readme_md.h"
#include "help_page.h"
#include "calibrate_page.h"
#include "heltec_board.h"
#include "serial_log.h"
#include "loxone.h"
#include "api_info.h"

#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "ota_check.h"
#include "esp_flash.h"

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
static const char OTA_PAGE_HTML[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Firmware update</title></head><body style='font-family:sans-serif;max-width:480px;margin:1rem auto;padding:0 1rem;'>"
    "<h3>Firmware update</h3><form method='POST' action='/u' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin,application/octet-stream'><br/><small>Either <b>universal-weather-node-&lt;version&gt;.bin</b> or the <i>-factory</i> file (only its firmware part is written). Files that are no firmware are refused.</small><br/><br/>"
    "<button type='submit'>Update</button></form><p><a href='/'>Back</a></p></body></html>";

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
    // value sources as one field: "air,wind,rain,uv,light" (0 auto, 1 board, 2-5 wireless 1-4, 6 off)
    char b_srcs[16];
    snprintf(b_srcs, sizeof(b_srcs), "%u,%u,%u,%u,%u", cur.srcSel[0], cur.srcSel[1], cur.srcSel[2], cur.srcSel[3], cur.srcSel[4]);
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

    WiFiManagerParameter p_lora_section("<div style='margin:1rem 0 0.5rem 0;'><h3>LoRaWAN (optional)</h3><p style='margin:0;'>Only for sending over a LoRaWAN network (TTN, ChirpStack, ...): switch it on and enter the keys. Not needed for a Wi-Fi station.</p></div>");
    WiFiManagerParameter p_loraen("loraen", "LoRaWAN transport enabled (0=No, 1=Yes)", b_loraen, 2);
    WiFiManagerParameter p_lowpower("lowpower", "Low-power mode: light sleep between samples (LoRa only, WiFi off; 0=No, 1=Yes)", b_lowpower, 2);
    WiFiManagerParameter p_vanesave("vanesave", "Low-power mode: switch the wind-vane supply off between readings (PCB v2.0; 0=No, 1=Yes)", b_vanesave, 2);
    WiFiManagerParameter p_dev    ("dev",      "DevEUI (16 hex chars)",                        b_dev,     17);
    WiFiManagerParameter p_join   ("join",     "JoinEUI (16 hex chars)",                       b_join,    17);
    // Secrets are never prefilled (the portal AP is open): empty field, an
    // empty submission keeps the stored value.
    WiFiManagerParameter p_key    ("key",      "AppKey (32 hex chars; leave empty to keep the stored key)", "", 33,
                                   "placeholder='(unchanged)' autocomplete='off'");
    WiFiManagerParameter p_region ("region",   "LoRaWAN region (EU868, US915, AU915, AS923, IN865, KR920, CN470, RU864)", b_region, 8);
    WiFiManagerParameter p_pcb    ("pcbver",   "PCB revision (12=Rev1.2, 13=Rev1.3, 20=v2.0; v2.0 is detected automatically; ignored on a Heltec V4: wireless build only)", b_pcb, 3);
    char b_board[2], l_board[160];
    itoa(cur.boardSel, b_board, 10);
    snprintf(l_board, sizeof(l_board), "Heltec board (0=Auto by flash size, 3=V3, 4=V4; detected: %s, running as %s%s)",
             boardName(gBoard.detected), boardName(gBoard.kind), boardIsR8() ? ", V4-R8 NOT supported" : "");
    WiFiManagerParameter p_board  ("board",    l_board, b_board, 2);
    WiFiManagerParameter p_alt    ("altitude", "Altitude above sea level in m (0-4000)",       b_alt,     6);
    WiFiManagerParameter p_uplink ("uplinksec","LoRaWAN / measurement interval in s (5-3600)",  b_uplink, 7);
    WiFiManagerParameter p_sample ("samplesec","Wind sample interval in s: 1 or 3",            b_sample, 4);
    WiFiManagerParameter p_lorawagg("lorawagg","LoRaWAN wind values: 0 = interval since the last uplink, 1 = 10-min (WMO)", b_lorawagg, 2);
    WiFiManagerParameter p_mqttsec ("mqttsec", "MQTT interval in s: 0 = like the LoRaWAN interval, else 2-3600", b_mqttsec, 7);
    WiFiManagerParameter p_mqttwagg("mqttwagg","MQTT wind values: 0 = interval since the last publish, 1 = 10-min (WMO)", b_mqttwagg, 2);
    WiFiManagerParameter p_rfen    ("rfen",    "Wireless sensors (868 MHz): 1 = receive with the LoRa chip (between uplinks), 0 = off", b_rfen, 2);
    WiFiManagerParameter p_rfproto ("rfproto", "Wireless sensor type: 16 = Auto (search: stores the type of the first sensor received), 1 = Bresser, 6 = TFA/LaCrosse IT+ (both rates), 2 = IT+ 17.241 kbit/s only, 4 = IT+ 9.579 kbit/s only, 8 = Fine Offset / Ecowitt / Froggit, 10 = IT+ 17.241 kbit/s + Fine Offset (same data rate), 14 = IT+ both rates + Fine Offset, 7 = Bresser + IT+ / 9 = Bresser + Fine Offset alternating (misses packets, not recommended)", b_rfproto, 2);
    // Free-text fields: shown HTML-escaped (htmlAttr), so the field length is
    // at least the escaped length; Config's own buffer limits the saved text.
    auto shownLen = [](int maxLen, const String& shown) { return max(maxLen, (int)shown.length()); };
    const String e_rfids = htmlAttr(cur.rfIds);
    WiFiManagerParameter p_rfids   ("rfids",   "Wireless sensor IDs (hex, comma-separated, max 4; empty = any)", e_rfids.c_str(), shownLen(sizeof(cur.rfIds) - 1, e_rfids));
    WiFiManagerParameter p_srcs    ("srcs",    "Value sources temp/hum, wind, rain, UV, light (0 auto, 1 board, 2-5 wireless 1-4, 6 off)", b_srcs, 15);
    WiFiManagerParameter p_windoff("windoff",  "Wind direction offset in degrees (-180 to +180)", b_windoff, 5);

    // Units (in "Basic setup"): one master switch (Metric/Imperial) plus the
    // km/h-vs-m/s refinement, which only applies when Units = Metric.
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

    WiFiManagerParameter p_winden ("winden",   "Wind sensor on the PCB enabled (0=No, 1=Yes; 0 without a PCB)", cur.windEn ? "1" : "0", 2);
    WiFiManagerParameter p_rainen ("rainen",   "Rain sensor on the PCB enabled (0=No, 1=Yes; 0 without a PCB)", cur.rainEn ? "1" : "0", 2);
    WiFiManagerParameter p_rainpersist("rainpersist", "Rain statistics (today/yesterday, last rain, week/month/year): save to flash at most hourly and before restarts so they survive a power cut (1=Yes, 0=RTC RAM only)", cur.rainPersist ? "1" : "0", 2);
    WiFiManagerParameter p_lipo   ("lipoen",   "LiPo measurement enabled (0=No, 1=Yes)",       cur.lipoEn ? "1" : "0", 2);
    WiFiManagerParameter p_debug  ("debugmode","Debug mode (0=No, 1=Yes, sends extra telemetry)", b_debug, 2);
    WiFiManagerParameter p_cpu    ("cpufreq",  "CPU frequency in MHz (80 or 240)",             b_cpu,     4);
    WiFiManagerParameter p_payfmt ("payfmt",   "LoRaWAN payload format: 1 = compact binary (FPort 3, recommended), 0 = CayenneLPP (FPort 1, about twice the size). "
                                               "At slow data rates (SF10-SF12: max. 51 bytes) the node leaves out the least important values "
                                               "(debug, AUX, outputs, Victron, LiPo, DS18B20, then BME280, rain, wind) until the uplink fits - "
                                               "binary usually sends everything, CayenneLPP often has to drop some. Both need the matching decoder (Payload decoder button).",
                                   b_payfmt, 2);
    // PCB v2.0 I/O block, all off by default; ignored on Rev 1.2/1.3.
    WiFiManagerParameter p_io_section(gIo.hwIdDetected
        ? "<div style='margin:0.5rem 0 0.5rem 0;'><strong>PCB v2.0 Inputs / Outputs</strong> (board detected as v2.0)</div>"
        : "<div style='margin:0.5rem 0 0.5rem 0;'><strong>PCB v2.0 Inputs / Outputs</strong> (only used on PCB v2.0)</div>");
    WiFiManagerParameter p_auxainen  ("auxainen",   "AUX analog input GPIO4 (0=off, 1=on)",                    b_auxainen,   2);
    WiFiManagerParameter p_auxainscale("auxainscale","AUX analog scale (1 = direct, 1.66667 with R25 15k for 0-5 V)", b_auxainscale, 11);
    WiFiManagerParameter p_auxpulseen("auxpulseen", "AUX pulse counter GPIO48 (0=off, 1=on)",                  b_auxpulseen, 2);
    WiFiManagerParameter p_wind2     ("wind2",      "2nd wind sensor on AUX (0=off, 1=speed at PULSE, 2=speed + direction at AIN)", b_wind2, 2);
    WiFiManagerParameter p_wind2off  ("wind2off",   "2nd wind vane offset in degrees (-180 to 180)",          b_wind2off,   5);
    WiFiManagerParameter p_heatmode  ("heatmode",   "Heater J12 (0=off, 1=on, 2=auto below threshold)",        b_heatmode,   2);
    WiFiManagerParameter p_heatduty  ("heatduty",   "Heater power in % (1-100, 10 s cycle / 0.1 Hz; auto: at full-power temperature)", b_heatduty, 4);
    WiFiManagerParameter p_heatbelow ("heatbelow",  "Heater auto: on below °C (off 1 K above)",                b_heatbelow,  7);
    WiFiManagerParameter p_heatminduty("heatminduty","Heater auto: power in % at the switch-on temperature",   b_heatminduty, 4);
    WiFiManagerParameter p_heatfull  ("heatfull",   "Heater auto: full power at and below °C",                 b_heatfull,   7);
    WiFiManagerParameter p_fanmode   ("fanmode",    "Fan J13 (0=off, 1=on, 2=auto above threshold)",           b_fanmode,    2);
    WiFiManagerParameter p_fanduty   ("fanduty",    "Fan speed in % (1-100, PWM)",                             b_fanduty,    4);
    WiFiManagerParameter p_fanabove  ("fanabove",   "Fan auto: on from °C (off 1 K below)",                    b_fanabove,   7);
    WiFiManagerParameter p_fanminpv  ("fanminpv",   "Fan auto: minimum PV W (0=ignore; missing VE.Direct data=off)", b_fanminpv,   5);
    WiFiManagerParameter p_tachoen   ("tachoen",    "Fan tacho GPIO40 (0=off, 1=on)",                          b_tachoen,    2);
    WiFiManagerParameter p_tachoppr  ("tachoppr",   "Tacho pulses per revolution (PC fans: 2)",                b_tachoppr,   2);
    WiFiManagerParameter p_vedirecten("vedirecten", "Victron VE.Direct (0=off, 1=on)",                         b_vedirecten, 2);
    WiFiManagerParameter p_outminbatt("outminbatt", "Heater/fan minimum battery V (0=no limit; missing VE.Direct data=off)",  b_outminbatt, 5);
    WiFiManagerParameter p_uven("uven", "UV sensor on GPIO3 (0=off, 1=on; PCB v2.0 only)", b_uven, 2);
    WiFiManagerParameter p_uvscale("uvscale", "UV divider scale (terminal V per ADC V: 1.1 for JP10 1-2 / 0-3.3 V, 1.66534 for JP10 2-3 / 0-5 V)", b_uvscale, 11);
    WiFiManagerParameter p_uvindexen("uvindexen", "Publish calculated UV index (0=off, 1=on)", b_uvindexen, 2);
    WiFiManagerParameter p_uvcal("uvcal", "UV calibration explicitly verified (0=no, 1=yes)", b_uvcal, 2);
    WiFiManagerParameter p_uvslope("uvslope", "UV index calibration slope (index per volt, 0.001-30)", b_uvslope, 11);
    WiFiManagerParameter p_uvoffset("uvoffset", "UV index calibration offset (-10 to +10)", b_uvoffset, 11);

    WiFiManagerParameter p_section("<div style='margin:0.5rem 0 0.75rem 0;'><h3>Universal-Weather-Node Parameter</h3><p style='margin:0 0 0.35rem 0;'>Firmware version: " FIRMWARE_VERSION "</p><p style='margin:0;'>A Wi-Fi station with a wireless outdoor sensor only needs the Wi-Fi network (\"Configure WiFi\"); the fields under <b>Basic setup</b> are optional. Everything else can also be changed later on the node's web settings page.</p></div>");
    WiFiManagerParameter p_basic_section("<div style='margin:0.5rem 0 0.5rem 0;'><strong>Basic setup</strong></div>");
    WiFiManagerParameter p_board_section("<div style='margin:1rem 0 0.5rem 0;'><strong>Board sensors and measurement</strong> (PCB: Davis wind/rain, BME280 is detected automatically)</div>");

    // Parallel WiFi + MQTT transport, fully optional alongside LoRaWAN. WiFi
    // STA credentials/static-IP are configured via WiFiManager's own "wifi"
    // menu, not here.
    WiFiManagerParameter p_net_section("<div style='margin:1rem 0 0.75rem 0;'><h3>WiFi + MQTT Parameter</h3><p style='margin:0;'>WiFi network credentials and static IP (optional, default DHCP) are configured separately via the \"Configure WiFi\" menu.</p><p style='margin:0.35rem 0 0 0;'>Weather network uploads (AWEKAS, Weather Underground, ...) and time zone are set on the node's web settings page (http://&lt;node-ip&gt;/api/settings) once WiFi is up.</p></div>");
    WiFiManagerParameter p_wifien    ("wifien",     "WiFi enabled (0=No, 1=Yes) - master switch for MQTT/Web API", b_wifien, 2);
    const String e_devname = htmlAttr(cur.deviceName), e_mqtthost = htmlAttr(cur.mqttHost),
                 e_mqttuser = htmlAttr(cur.mqttUser), e_mqtttopic = htmlAttr(cur.mqttTopic);
    WiFiManagerParameter p_devname   ("devname",    "Device name (WiFi hostname, MQTT client id, default topic prefix)", e_devname.c_str(), shownLen(33, e_devname));
    WiFiManagerParameter p_webapien  ("webapien",   "Web REST API + settings page enabled (0=No, 1=Yes)", b_webapien, 2);
    WiFiManagerParameter p_webadminpw("webadminpw", "Admin password for the web settings page and firmware updates (OTA, /update); leave empty to keep", "", 33,
                                      cur.webAdminPass[0] ? "type='password' autocomplete='new-password' placeholder='(unchanged)'"
                                                          : "type='password' autocomplete='new-password' placeholder='(not set)'");
    WiFiManagerParameter p_webadminpwclr("webadminpwclr", "Remove the stored admin password (settings page locked, OTA unprotected)", "1", 2,
                                         "type='checkbox'", WFM_LABEL_AFTER);
    WiFiManagerParameter p_mqtten    ("mqtten",     "MQTT publish/control enabled (0=No, 1=Yes)",   b_mqtten,     2);
    WiFiManagerParameter p_mqttctrlen("mqttctrlen", "MQTT command topic enabled (0=No, 1=Yes)",     b_mqttctrlen, 2);
    WiFiManagerParameter p_mqtthost  ("mqtthost",   "MQTT broker host",                             e_mqtthost.c_str(), shownLen(65, e_mqtthost));
    WiFiManagerParameter p_mqttport  ("mqttport",   "MQTT broker port (standard: 1883 = plain/no TLS, 8883 = TLS)", b_mqttport, 7);
    WiFiManagerParameter p_mqttuser  ("mqttuser",   "MQTT username (optional)",                     e_mqttuser.c_str(), shownLen(33, e_mqttuser));
    WiFiManagerParameter p_mqttpass  ("mqttpass",   "MQTT password (optional; leave empty to keep)", "", 33,
                                      cur.mqttPass[0] ? "type='password' autocomplete='new-password' placeholder='(unchanged)'"
                                                      : "type='password' autocomplete='new-password' placeholder='(not set)'");
    WiFiManagerParameter p_mqttpassclr("mqttpassclr", "Remove the stored MQTT password", "1", 2, "type='checkbox'", WFM_LABEL_AFTER);
    WiFiManagerParameter p_mqtttopic ("mqtttopic",  "MQTT topic prefix (optional, default = device name)", e_mqtttopic.c_str(), shownLen(65, e_mqtttopic));
    WiFiManagerParameter p_mqtttls   ("mqtttls",    "MQTT security (0=plain/no TLS, 1=TLS/trust any cert, 2=TLS/verify cert below)", b_mqtttls, 2);
    WiFiManagerParameter p_cacert_html(b_cacert_html);
    // Loxone UDP output (loxone.h)
    char b_loxen[2], b_loxport[8], b_loxsec[8];
    itoa(cur.loxEn,   b_loxen,   10);
    itoa(cur.loxPort, b_loxport, 10);
    itoa(cur.loxSec,  b_loxsec,  10);
    const String e_loxhost = htmlAttr(cur.loxHost), e_loxprefix = htmlAttr(cur.loxPrefix);
    WiFiManagerParameter p_loxen    ("loxen",     "Loxone UDP output (0=No, 1=Yes): one datagram name=value per value", b_loxen, 2);
    WiFiManagerParameter p_loxhost  ("loxhost",   "Loxone Miniserver IP or host name", e_loxhost.c_str(), shownLen(65, e_loxhost));
    WiFiManagerParameter p_loxport  ("loxport",   "Loxone UDP port (1-65535, default 7000)", b_loxport, 7);
    WiFiManagerParameter p_loxprefix("loxprefix", "Loxone name prefix (optional, max 16 of A-Z a-z 0-9 _ . -)", e_loxprefix.c_str(), shownLen(17, e_loxprefix));
    WiFiManagerParameter p_loxsec   ("loxsec",    "Loxone interval in s: 0 = every measurement cycle, else 2-3600", b_loxsec, 7);

    WiFiManager wm;
    const char* menu[] = { "wifi", "info", "custom", "sep", "exit" };
    wm.setMenu(menu, 5);
    wm.setCustomMenuHTML("<div style='margin:0.75rem 0 1rem 0;'><div style='font-size:0.85rem;opacity:0.75;margin:0 0 0.25rem 0;'>Firmware version: " FIRMWARE_VERSION "</div><form action='/param' method='get'><button>Universal-Weather-Node Parameter</button></form><br/><form action='/decoder' method='get'><button>Payload decoder (JS)</button></form><br/><form action='/help' method='get'><button>Help / Documentation</button></form><br/><form action='/calibrate' method='get'><button>Calibrate Wind Direction</button></form><br/><form action='/update' method='get'><button>Firmwareupdate</button></form></div>");

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
        // Firmware upload, always our own handlers - registered before
        // WiFiManager's /update and /u (this callback runs first, the first
        // matching handler wins): Digest auth when an admin password is set.
        // OtaUploadFilter (ota_check.h) takes the application image or the
        // -factory image (of which only the application part is written, if
        // its partition table matches this node's); anything else is refused
        // before a byte is written, so a wrong file can't leave a node that
        // doesn't boot.
        {
            static bool uploadAuthOk = false, uploadOk = false;
            static const char* uploadError = nullptr;
            static OtaUploadFilter filter;
            static uint8_t devicePt[OTA_FACTORY_PT_LEN];
            static bool devicePtOk = false;
            wm.server->on("/update", HTTP_ANY, [&wm]() {
                if (otaPass[0] && !wm.server->authenticate("admin", otaPass)) {
                    wm.server->requestAuthentication(DIGEST_AUTH, "WeatherNode firmware update");
                    return;
                }
                wm.server->send_P(200, "text/html; charset=utf-8", OTA_PAGE_HTML);
            });
            wm.server->on("/u", HTTP_POST, [&wm]() {
                if (!uploadAuthOk) {
                    wm.server->requestAuthentication(DIGEST_AUTH, "WeatherNode firmware update");
                    return;
                }
                String msg = uploadOk ? String("Update successful, restarting...")
                           : uploadError ? String("Update refused: ") + uploadError
                                         : String("Update failed: ") + Update.errorString();
                wm.server->send(uploadOk ? 200 : 400, "text/plain; charset=utf-8", msg);
                if (uploadOk) { delay(1000); systemRestart("firmware update (browser)"); }
            }, [&wm]() {
                HTTPUpload& up = wm.server->upload();
                auto refuse = [](const char* why) {
                    uploadError = why;
                    dbgSerial.printf("Portal OTA: refused - %s\n", why);
                    if (Update.isRunning()) Update.abort();
                };
                if (up.status == UPLOAD_FILE_START) {
                    uploadAuthOk = !otaPass[0] || wm.server->authenticate("admin", otaPass);
                    uploadOk     = false;
                    uploadError  = nullptr;
                    dbgSerial.printf("Portal OTA: upload %s\n", uploadAuthOk ? "started" : "refused (not authenticated)");
                    if (!uploadAuthOk) return;
                    if (!devicePtOk)
                        devicePtOk = esp_flash_read(nullptr, devicePt, OTA_FACTORY_PT_OFFSET, sizeof(devicePt)) == ESP_OK;
                    filter.begin(devicePtOk ? devicePt : nullptr);
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) dbgSerial.printf("Portal OTA: %s\n", Update.errorString());
                } else if (!uploadAuthOk || uploadError) {
                    return;   // the rest of the body is read and discarded
                } else if (up.status == UPLOAD_FILE_WRITE) {
                    if (!Update.isRunning()) return;
                    bool ok = filter.feed(up.buf, up.currentSize, [](const uint8_t* d, size_t n, void*) {
                        return Update.write(const_cast<uint8_t*>(d), n) == n;
                    }, nullptr);
                    if (!ok) refuse(filter.error());
                } else if (up.status == UPLOAD_FILE_END) {
                    if (!Update.isRunning()) return;
                    if (!filter.finish()) { refuse(filter.error()); return; }
                    uploadOk = Update.end(true);
                    if (uploadOk) dbgSerial.printf("Portal OTA: %s image written\n", filter.isFactory() ? "application part of the factory" : "application");
                    else          dbgSerial.printf("Portal OTA: %s\n", Update.errorString());
                } else if (up.status == UPLOAD_FILE_ABORTED) {
                    Update.abort();
                }
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
            // Heltec V4: GPIO2 is the RF front end's enable - never read it as the vane
            int adc = boardPcbAllowed() ? adcAverage(PIN_ADC_WINDDIRECTION) : 0;
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
        if (paramsSubmitted) strlcpy(c.rfIds, p_rfids.getValue(), sizeof(c.rfIds));
        {
            const char* p = p_srcs.getValue();
            for (int i = 0; i < 5 && *p; i++) {
                int v = atoi(p);
                if (v >= 0 && v <= 6) c.srcSel[i] = (uint8_t)v;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
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
        if (capturedCaCertSet) strlcpy(c.mqttCaCert, capturedCaCert, sizeof(c.mqttCaCert));

        c.loxEn = atoi(p_loxen.getValue()) ? 1 : 0;
        { int v = atoi(p_loxport.getValue()); if (loxPortValid(v)) c.loxPort = v; }
        { int v = atoi(p_loxsec.getValue());  if (loxIntervalValid(v)) c.loxSec = v; }
        if (paramsSubmitted) {
            strlcpy(c.loxHost, p_loxhost.getValue(), sizeof(c.loxHost));
            if (loxPrefixValid(p_loxprefix.getValue())) strlcpy(c.loxPrefix, p_loxprefix.getValue(), sizeof(c.loxPrefix));
        }

        return c;
    };

    // The CA-cert field is a raw <textarea>, not a normal id-based
    // WiFiManagerParameter, so it isn't populated by doParamSave() the usual
    // way. setSaveParamsCallback() fires right after params are saved, while
    // wm.server still has the submitted request's args available.
    wm.setSaveParamsCallback([&wm, &capturedCaCertSet, &paramsSubmitted, &buildConfig]() {
        String v = wm.server->arg("mqttcacert");
        strlcpy(capturedCaCert, v.c_str(), sizeof(capturedCaCert));
        capturedCaCertSet = true;
        paramsSubmitted   = true;

        // Persist immediately. The "Universal-Weather-Node Parameter" page
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

    // First-boot friendly order: the few fields a plain Wi-Fi station with a
    // wireless sensor may want first, LoRaWAN (optional) at the end.
    wm.addParameter(&p_section);
    wm.addParameter(&p_basic_section);
    wm.addParameter(&p_devname);  wm.addParameter(&p_alt);
    wm.addParameter(&p_rfen);     wm.addParameter(&p_rfproto); wm.addParameter(&p_rfids);
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
        display.drawString(0, 11, "SSID: WeatherNode");
        display.drawString(0, 22, "http://" + ip);
        display.drawString(0, 33, "OTA:  weather-node");
        display.drawString(0, 44, "1. Connect to SSID");
        display.drawString(0, 55, "2. Open URL in browser");
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

    wm.startConfigPortal("WeatherNode");

    if (otaHandle) { vTaskDelete(otaHandle); ArduinoOTA.end(); }

    return buildConfig();
}
