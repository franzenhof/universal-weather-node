#include "web_api.h"
#include "system_restart.h"
#include "globals.h"
#include "params.h"
#include "board_io.h"
#include "heltec_board.h"
#include "sources.h"
#include "lora_payload.h"
#include "json_payload.h"
#include "history.h"
#include "wifi_mqtt.h"
#define README_MD_DEFINE   // the one definition of README_MD / HELP_PAGE_HTML (readme_md.h, help_page.h)
#include "readme_md.h"
#include "help_page.h"
#include "log_page.h"
#include "chart_page_gz.h"   // the dashboard, gzip (tools/gz_embed.py; source: chart_page.h)
#include "gz_send.h"
#include "web_manifest.h"
#include "web_assets.h"   // icons + service worker (tools/version.py, git-ignored)
#include "tls_gate.h"
#include "rf_autosearch.h"
#include "generated_version.h"
#include "serial_log.h"
#include "units.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "lora_transport.h"
#include "wind_window.h"
#include "rf_sensors.h"
#include "davis_rx.h"
#include "rf_trace.h"
#include "ota_upload.h"
#include "i18n.h"
#include "setup_wizard.h"
#include "records.h"
#include "out_status.h"
#include "calibration.h"
#include "astro.h"
#include "rf_stats.h"
#include "rf_names.h"
#include "sensors.h"
#include "net_check.h"
#include <esp_heap_caps.h>
#include "settings_backup.h"
#include "alarms.h"
#include "custom_upload.h"
#include "oled_screens.h"
#include <memory>
#include <new>
#include "davis_diag.h"
#include "derived_now.h"
#include "api_info.h"
#include "portal_pages.h"   // /decoder, /calibrate*: the portal's pages and handlers

#include <Arduino.h>
#include <new>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "loxone.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

WebServer server(80);
bool      serverStarted = false;

// Guards `server` (begin/stop/handleClient) and `serverStarted` themselves —
// separate from history.cpp's own mutex, which guards different data (the
// ring buffer). server.handleClient() now runs continuously from its own
// FreeRTOS task (webApiTask, pinned to core 0, see webApiSetup() below)
// instead of once per main-loop() iteration, so it no longer sits behind
// sendLoRa()'s blocking airtime — but webApiStop() (main.cpp, called from
// the main loop task right before opening the WiFiManager config portal)
// can now run concurrently with that task's own begin/stop/handleClient
// cycle, and WiFiServer's underlying socket isn't documented as safe to
// stop() from one task while another is mid-accept()/handleClient() on it.
SemaphoreHandle_t serverMutex = xSemaphoreCreateMutex();
TaskHandle_t      webApiTaskHandle = nullptr;
// Set by webApiStop() under serverMutex. webApiTask checks it before every
// begin(): between webApiStop() releasing the mutex and suspending the task,
// a task already waiting on the mutex would otherwise restart the server.
bool              apiStopped = false;

// Streams a JsonDocument to the client as chunked transfer-encoding, one
// bounded buffer at a time. Used by handleApiHistory() below instead of
// measureJson()+serializeJson() (which walked the whole document twice —
// once just to precompute Content-Length — doubling ArduinoJson's traversal
// cost on every poll). Reuses WebServer's own chunk framing: passing
// CONTENT_LENGTH_UNKNOWN to setContentLength() before send() makes
// WebServer::_prepareHeader() switch to "Transfer-Encoding: chunked" and
// sendContent() add the size-prefixed framing per call; _finalizeResponse()
// appends the terminating zero-length chunk automatically once the handler
// returns. Same Print-subclass pattern as SerialLog (serial_log.h).
class ChunkedPrint : public Print
{
public:
    explicit ChunkedPrint(WebServer& srv) : server_(srv) {}

    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t* data, size_t len) override
    {
        size_t remaining = len;
        while (remaining) {
            size_t space = sizeof(buf_) - fill_;
            size_t n     = remaining < space ? remaining : space;
            memcpy(buf_ + fill_, data, n);
            fill_ += n;
            data  += n;
            remaining -= n;
            if (fill_ == sizeof(buf_)) flushChunk();
        }
        return len;
    }

    void flush() override { flushChunk(); }

private:
    void flushChunk()
    {
        if (fill_) {
            server_.sendContent(buf_, fill_);
            fill_ = 0;
        }
    }

    WebServer& server_;
    char       buf_[512];
    size_t     fill_ = 0;
};

// A streamed HTML page (settings, status, ...): chunked transfer encoding
// through ChunkedPrint's 512-byte buffer, so no page needs one large
// contiguous String (the settings page was ~36 KB). The handlers append with
// += like to a String; finish() sends the rest.
// Settings backup (handleSettingsBackup): the settings page is rendered into
// nothing while every form field's name and value is recorded here.
JsonObject gCapture;            // null = normal page output
bool       gCaptureSecrets = false;

class HtmlOut
{
public:
    // Sends the status line and headers right away (not while capturing).
    HtmlOut(WebServer& srv, int code) : out_(srv), discard_(!gCapture.isNull())
    {
        if (discard_) return;
        srv.sendHeader("Cache-Control", "no-store");
        srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
        srv.send(code, "text/html; charset=utf-8", "");
    }
    HtmlOut& operator+=(const char* s)   { if (s && !discard_) out_.write((const uint8_t*)s, strlen(s)); return *this; }
    HtmlOut& operator+=(const String& s) { if (!discard_) out_.write((const uint8_t*)s.c_str(), s.length()); return *this; }
    HtmlOut& operator+=(const __FlashStringHelper* s) { return *this += reinterpret_cast<const char*>(s); }
    HtmlOut& operator+=(char c)          { if (!discard_) out_.write((uint8_t)c); return *this; }
    HtmlOut& operator+=(int v)           { return num("%d", v); }
    HtmlOut& operator+=(unsigned v)      { return num("%u", v); }
    HtmlOut& operator+=(long v)          { return num("%ld", v); }
    HtmlOut& operator+=(unsigned long v) { return num("%lu", v); }
    void finish() { if (!discard_) out_.flush(); }

private:
    template <class V> HtmlOut& num(const char* fmt, V v)
    {
        char b[16];
        int n = snprintf(b, sizeof(b), fmt, v);
        if (!discard_) out_.write((const uint8_t*)b, n > 0 ? (size_t)n : 0);
        return *this;
    }
    ChunkedPrint out_;
    bool         discard_;
};

// Write-only fields are recorded by fieldSecret() (only when asked for); text
// fields that may carry a key (webhook / custom-upload URLs) likewise.
// Fields of the Alarms tab belong to their own form (form='aform'): the main
// settings form stays small enough for the web server's argument parser.
const char* gFieldForm = nullptr;
void formAttr(HtmlOut& html) { if (gFieldForm) { html += "' form='"; html += gFieldForm; } }

void capture(const char* name, const String& value)
{
    if (gCapture.isNull() || (backupIsSecret(name) && !gCaptureSecrets)) return;
    gCapture[name] = value;
}

// Read-only JSON API for third-party UIs (docs/api.md): /api/data,
// /api/history and /api/info may be read from any origin (CORS "*"; no
// credentials, nothing secret in them). /api/settings and /api/log* get no
// CORS headers: they stay same-origin + Digest (see requireAuth()).
void sendPublicApiHeaders()
{
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
}

// CORS preflight (OPTIONS) for the read-only endpoints. A plain GET needs
// none, but a client adding a header (e.g. Cache-Control) triggers one.
// Allow-Private-Network: Chrome's Private Network Access preflight for a
// page from a public origin reading a node in the LAN.
void handlePublicApiOptions()
{
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "*");
    server.sendHeader("Access-Control-Allow-Private-Network", "true");
    server.sendHeader("Access-Control-Max-Age", "86400");
    server.send(204);
}

// Streams a document built by the API builders (chunked, see ChunkedPrint).
void sendJsonDoc(JsonDocument& doc, const char* what)
{
    sendPublicApiHeaders();
    if (doc.overflowed()) {
        dbgSerial.printf("API %s: JSON allocation failed.\n", what);
        server.send(500, "text/plain", "JSON allocation failed");
        return;
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    ChunkedPrint out(server);
    serializeJson(doc, out);
    out.flush();
}

void handleApiData()
{
    // static: retains its underlying buffer across requests instead of
    // growing it from scratch and freeing it every call — safe because
    // WebServer::handleClient() only ever processes one request at a time on
    // this one task (see ChunkedPrint's comment above); buildJsonPayload()
    // clears the content (not the buffer) at its own top (see buildJsonPayload()).
    static JsonDocument doc;
    buildJsonPayload(doc);
    sendJsonDoc(doc, "telemetry");
}

// Records (records.h): extremes of today ... all time. No secrets, no auth.
void handleApiRecords()
{
    static JsonDocument doc;   // static: see handleApiData()
    static Records r;          // ~430 bytes, not on the web task's stack
    recordsSnapshot(r);
    struct tm lt;
    const bool clock = localTimeNow(lt);
    if (clock) recordsRoll(r, lt);   // no cycle since midnight yet: "today" empty, yesterday shifted
    recordsJson(doc, r, gCfg, clock);
    sendJsonDoc(doc, "records");
}

// Heavy pages while an HTTPS request runs with little free memory: "busy, try
// again" instead of risking both (tls_gate.h). Never blocks.
bool webBusyRefused()
{
    if (!webShouldRefuse(tlsRunning(), heap_caps_get_free_size(MALLOC_CAP_8BIT))) return false;
    server.sendHeader("Retry-After", "5");
    server.send(503, "text/plain; charset=utf-8", T(T_c_busy_retry));
    return true;
}

// Device description for third-party UIs (api_info.h). No secrets, no auth.
void handleApiInfo()
{
    static JsonDocument doc;   // static: see handleApiData()
    buildInfoJson(doc);
    sendJsonDoc(doc, "info");
}

// ---- Home screen / installed app (docs/web-dashboard.md "Install as an app") ----
void sendStaticBytes(const uint8_t* data, size_t len, const char* type)
{
    server.sendHeader("Cache-Control", "public, max-age=604800");
    server.send_P(200, type, (const char*)data, len);
}
void handleIcon192()       { sendStaticBytes(ICON_192, ICON_192_LEN, "image/png"); }
void handleIcon512()       { sendStaticBytes(ICON_512, ICON_512_LEN, "image/png"); }
void handleIconMaskable()  { sendStaticBytes(ICON_MASKABLE_512, ICON_MASKABLE_512_LEN, "image/png"); }
void handleAppleTouch()    { sendStaticBytes(APPLE_TOUCH_ICON, APPLE_TOUCH_ICON_LEN, "image/png"); }
void handleFavicon()       { sendStaticBytes(FAVICON_32, FAVICON_32_LEN, "image/png"); }   // /favicon.ico too: a PNG (every browser takes it)

// Service worker (web/sw.js): only registered by the dashboard in a secure context
void handleServiceWorker()
{
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Service-Worker-Allowed", "/");
    server.send_P(200, "application/javascript; charset=utf-8", (const char*)SW_JS, SW_JS_LEN);
}

// Web app manifest (web_manifest.h)
void handleManifest()
{
    JsonDocument doc;
    buildManifest(doc, gCfg);
    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "application/manifest+json", out);
}

// Rendered README help page, works fully offline (no CDN dependency) — see
// include/help_page.h. Same route pair as the WiFiManager portal's /help.
void handleHelp()
{
    server.send_P(200, "text/html; charset=utf-8", HELP_PAGE_HTML);
}

void handleHelpMd()
{
    sendGzipAsset(server, README_MD_GZ, README_MD_GZ_LEN, README_MD_LEN, "text/plain; charset=utf-8");
}

// ---------------------------------------------------------------------------
// Shared look of the server-rendered pages (/status, /api/settings): same
// CSS custom properties/palette as the dashboard (chart_page.h), so all
// pages read as one UI. The one-line script only applies the theme stored
// by the dashboard's toggle — both pages work unchanged without JS.
// ---------------------------------------------------------------------------
const char PAGE_CSS[] PROGMEM = R"CSS(<style>
:root{--bg:#f4f6f8;--panel:#fff;--border:#dde3e9;--text:#1a2028;--dim:#62707c;--accent:#0f9d94;--link:#0b7f78;--ok:#2e9d57;--warn:#c98a12;--bad:#d0443a;}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#12161b;--panel:#1a2028;--border:#2a323c;--text:#e7ecf1;--dim:#8996a3;--accent:#3fd0c9;--link:#3fd0c9;--ok:#4cc27a;--warn:#e0a63e;--bad:#ef6a5f;}}
:root[data-theme="dark"]{--bg:#12161b;--panel:#1a2028;--border:#2a323c;--text:#e7ecf1;--dim:#8996a3;--accent:#3fd0c9;--link:#3fd0c9;--ok:#4cc27a;--warn:#e0a63e;--bad:#ef6a5f;}
*{box-sizing:border-box;}
body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);max-width:1000px;margin:0 auto;padding:0 1rem 2rem;}
nav{display:flex;flex-wrap:wrap;gap:1.1rem;padding:0.9rem 0;border-bottom:1px solid var(--border);margin-bottom:1rem;}
a:where(:link,:visited){color:var(--link);}
:root{color-scheme:light dark;}:root[data-theme="light"]{color-scheme:light;}:root[data-theme="dark"]{color-scheme:dark;}
nav a{color:var(--dim);text-decoration:none;font-size:0.9rem;}
nav a:hover,nav a:focus,nav a.on{color:var(--accent);}nav a.on{font-weight:600;}
h3{margin:0 0 0.25rem;}
.sub{color:var(--dim);font-size:0.85rem;margin:0 0 1rem;}
section{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:0.75rem 0.9rem;margin-bottom:0.9rem;}
h4{margin:0 0 0.5rem;font-size:0.85rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
table{width:100%;border-collapse:collapse;}
td{padding:0.4rem 0.25rem;border-top:1px solid var(--border);vertical-align:top;}
tr:first-child td{border-top:0;}
td:first-child{color:var(--dim);width:38%;}
.num{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;}
.dot{display:inline-block;width:0.6rem;height:0.6rem;border-radius:50%;margin-right:0.45rem;}
.ok{background:var(--ok);}.warn{background:var(--warn);}.bad{background:var(--bad);}.off{background:var(--border);}
.dim,small{color:var(--dim);font-size:0.8rem;}
.tag{font-size:0.7rem;border-radius:4px;padding:0.05rem 0.35rem;margin-left:0.4rem;text-transform:none;letter-spacing:0;border:1px solid currentColor;}
.tag.live{color:var(--ok);}.tag.restart{color:var(--warn);}
.banner{border-radius:8px;padding:0.6rem 0.8rem;margin-bottom:0.9rem;border:1px solid;}
.banner.okb{border-color:var(--ok);}.banner.badb{border-color:var(--bad);}
[hidden]{display:none!important;}
label{display:grid;grid-template-columns:minmax(150px,38%) 1fr;gap:0.6rem;align-items:center;padding:0.35rem 0;border-top:1px solid var(--border);}
section>label:first-of-type,section>h4+label{border-top:0;}
label small{display:block;}
input,select,textarea{font:inherit;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:0.35rem 0.5rem;width:100%;}
input[type=checkbox]{width:1.1rem;height:1.1rem;justify-self:start;accent-color:var(--accent);}
textarea{font-family:ui-monospace,Consolas,monospace;font-size:0.75rem;}
.clear{display:inline-flex;gap:0.3rem;align-items:center;margin-top:0.25rem;font-size:0.8rem;color:var(--dim);border:0;padding:0;}
.ro{font-family:ui-monospace,Consolas,monospace;}
button{font:inherit;background:var(--accent);color:#fff;border:0;border-radius:6px;padding:0.55rem 1.4rem;cursor:pointer;}
.tabs{display:flex;flex-wrap:wrap;gap:0.4rem;margin:0 0 0.9rem;}
.tabs a{color:var(--dim);text-decoration:none;border:1px solid var(--border);border-radius:6px;padding:0.35rem 0.8rem;background:var(--panel);}
.tabs a.on{color:var(--accent);border-color:var(--accent);}
.js .tab{display:none;}.js .tab.on{display:block;}
.advtgl{position:absolute;opacity:0;width:1px;height:1px;}
.advlbl{border:0;grid-template-columns:1fr;cursor:pointer;}.advlbl span::before{content:'\2610  ';}
.advtgl:checked+.advlbl span::before{content:'\2611  ';color:var(--accent);}
.advtgl:not(:checked)~form .adv{display:none;}
.rft td,.rft th{font-size:0.85rem;padding:0.3rem 0.25rem;text-align:left;border-top:1px solid var(--border);}
.rft td:first-child{width:auto;color:var(--text);}.rft th{color:var(--dim);font-weight:normal;}
.rft select{width:auto;padding:0.15rem 0.3rem;}.sb{padding:0.15rem 0.5rem;font-size:0.8rem;margin:0 0.1rem;}
form.inl{display:inline;}
.lng{width:auto;margin-left:auto;padding:0.15rem 0.3rem;font-size:0.8rem;}
@media (max-width:560px){label{grid-template-columns:1fr;gap:0.25rem;}td:first-child{width:45%;}.rft .hm{display:none;}}
</style>)CSS";

// Appends `s` with the five HTML-special characters escaped — for config
// strings (device name, MQTT host, server replies) placed into page text or
// attribute values.
template <class Out>
void appendEscaped(Out& html, const char* s)
{
    for (; *s; s++) {
        switch (*s) {
            case '&':  html += "&amp;";  break;
            case '<':  html += "&lt;";   break;
            case '>':  html += "&gt;";   break;
            case '"':  html += "&quot;"; break;
            case '\'': html += "&#39;";  break;
            default:   html += *s;       break;
        }
    }
}

// The language of this request (i18n.h): the browser's own choice (cookie
// wnLang, set by the language menu / the dashboard), else the Language
// setting, else Accept-Language. Every page / error handler calls it first.
void selectLanguage()
{
    i18nSet(i18nLangFor(gCfg.uiLang, server.header("Cookie").c_str(), server.header("Accept-Language").c_str()));
}

// A JavaScript string literal ('...') of a (translated) text.
void jsString(HtmlOut& html, const char* s)
{
    html += '\'';
    for (; *s; s++) {
        if (*s == '\'' || *s == '\\') html += '\\';
        if (*s == '<') { html += "\\x3c"; continue; }   // no "</script>" inside
        html += *s;
    }
    html += '\'';
}

void pageStart(HtmlOut& html, const char* title, const char* activeHref, int refreshSec = 0)
{
    const char* name = gCfg.deviceName[0] ? gCfg.deviceName : "WeatherNode";
    html += "<!DOCTYPE html><html lang='"; html += langCode(i18nCurrent());
    html += "'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    if (refreshSec > 0) { html += "<meta http-equiv='refresh' content='"; html += refreshSec; html += "'>"; }
    html += "<title>"; appendEscaped(html, name); html += ' '; html += title; html += "</title>";
    // home screen / installed app (handleManifest(), the icons below)
    html += "<link rel='manifest' href='/manifest.webmanifest'><link rel='icon' type='image/png' sizes='32x32' href='/favicon-32.png'>"
            "<link rel='apple-touch-icon' href='/apple-touch-icon.png'>"
            "<meta name='theme-color' content='#f4f6f8' media='(prefers-color-scheme: light)'>"
            "<meta name='theme-color' content='#12161b' media='(prefers-color-scheme: dark)'>"
            "<meta name='mobile-web-app-capable' content='yes'><meta name='apple-mobile-web-app-capable' content='yes'>"
            "<meta name='apple-mobile-web-app-title' content='";
    appendEscaped(html, name); html += "'>";
    html += FPSTR(PAGE_CSS);
    html += "<script>try{var t=localStorage.getItem('wnTheme');if(t)document.documentElement.setAttribute('data-theme',t);}catch(e){}"
            // data-dep (DepScope): show a field only while its condition holds
            "document.addEventListener('DOMContentLoaded',function(){var d=document,els=d.querySelectorAll('[data-dep]');if(!els.length)return;"
            "function val(n){var f=d.querySelector('[name=\"'+n+'\"]');if(!f)return null;return f.type=='checkbox'?(f.checked?'1':'0'):f.value;}"
            "function ok(c){var m=c.match(/^(\\w+)(!?=)(.*)$/);if(!m)return true;var v=val(m[1]);if(v===null)return true;"
            "var h=m[3].split('|').indexOf(v)>=0;return m[2]=='='?h:!h;}"
            "function upd(){els.forEach(function(e){e.hidden=!e.getAttribute('data-dep').split(',').some(function(a){return a.split('&').every(ok);});});}"
            "d.addEventListener('change',upd);d.addEventListener('input',upd);upd();});</script>";
    html += "</head><body><nav>";
    const struct { const char* href; TextId text; } links[] = {
        { "/", T_c_dashboard }, { "/status", T_c_status }, { "/api/log", T_c_log },
        { "/api/settings", T_c_settings }, { "/help", T_c_help },
    };
    for (auto& l : links) {
        html += "<a href='"; html += l.href; html += "'";
        if (!strcmp(l.href, activeHref)) html += " class='on'";
        html += ">"; html += T(l.text); html += "</a>";
    }
    // per-browser language (cookie + the dashboard's localStorage key); "" = the device setting
    html += "<select id='lng' class='lng' aria-label='Language' onchange=\"var v=this.value;try{if(v)localStorage.setItem('wnLang',v);"
            "else localStorage.removeItem('wnLang');}catch(e){}document.cookie='wnLang='+v+';path=/;max-age='+(v?31536000:0)+';SameSite=Lax';"
            "location.reload();\"><option value=''>";
    html += T(T_c_lang_device);
    html += "</option>";
    static const char* const names[LANG_COUNT] = { "English", "Deutsch", "Fran&ccedil;ais", "Espa&ntilde;ol", "Portugu&ecirc;s" };
    String cookie = server.header("Cookie");
    for (uint8_t l = 0; l < LANG_COUNT; l++) {
        html += "<option value='"; html += langCode(l); html += "'";
        String c = String("wnLang=") + langCode(l);
        if (cookie.indexOf(c) >= 0) html += " selected";
        html += ">"; html += names[l]; html += "</option>";
    }
    html += "</select></nav><h3>"; appendEscaped(html, name); html += ' '; html += title; html += "</h3>";
}

// "42 s ago" / "7 min ago" / "3.5 h ago" for a millis() timestamp (texts:
// c_ago_s / c_ago_min / c_ago_h, word order per language).
String agoSeconds(unsigned long s)
{
    if (s < 60)   return TF(T_c_ago_s, String(s));
    if (s < 3600) return TF(T_c_ago_min, String(s / 60));
    return TF(T_c_ago_h, String(s / 3600.0f, 1));
}
String agoText(unsigned long atMs) { return agoSeconds((millis() - atMs) / 1000); }

// Wireless sensor types (gCfg.rfProto / parameter 50) for the settings
// select and the status page.
struct RfTypeEntry { int value; TextId text; };
const RfTypeEntry RF_TYPES[] = {
    { RF_PROTO_AUTO, T_rft_auto },
    { RF_PROTO_ALL, T_rft_all },
    { RF_PROTO_BRESSER, T_rft_bresser },
    { RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9, T_rft_itplus_both },
    { RF_PROTO_ITPLUS17, T_rft_itplus17 },
    { RF_PROTO_ITPLUS9, T_rft_itplus9 },
    { RF_PROTO_FINEOFFSET, T_rft_fineoffset },
    { RF_PROTO_ITPLUS17 | RF_PROTO_FINEOFFSET, T_rft_itplus17_fo },
    { RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9 | RF_PROTO_FINEOFFSET, T_rft_itplus_both_fo },
    { RF_PROTO_BRESSER | RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9, T_rft_bresser_itplus },
    { RF_PROTO_BRESSER | RF_PROTO_FINEOFFSET, T_rft_bresser_fo },
    { RF_PROTO_DAVIS, T_rft_davis },
};
constexpr size_t RF_TYPE_COUNT = sizeof(RF_TYPES) / sizeof(RF_TYPES[0]);

String rfSensorTypeText(int v)
{
    for (const RfTypeEntry& t : RF_TYPES) if (t.value == v) return T(t.text);
    return TF(T_st_bit_mask, String(v));
}

void addRow(HtmlOut& html, const String& label, const String& value)
{
    html += "<tr><td>" + label + "</td><td class='num'>" + value + "</td></tr>";
}

// One connection/service row: status dot (ok/warn/bad/off), name, details.
void addStateRow(HtmlOut& html, const char* cls, const char* label, const String& detail)
{
    html += "<tr><td><span class='dot "; html += cls; html += "'></span>";
    html += label; html += "</td><td>"; html += detail; html += "</td></tr>";
}

// Status page, Connections: Loxone UDP output.
void addLoxoneRow(HtmlOut& html)
{
    if (!gCfg.loxEn) { addStateRow(html, "off", "Loxone UDP", String("<span class='dim'>") + T(T_c_disabled) + "</span>"); return; }
    const LoxoneStatus& ls = loxoneStatus();
    String d = String(T(T_st_lox_to)) + " ";
    loxoneLock();
    appendEscaped(d, gCfg.loxHost[0] ? gCfg.loxHost : T(T_st_no_host));
    loxoneUnlock();
    d += ":" + String(gCfg.loxPort);
    if (ls.ipValid) d += " <span class='dim'>(" + ls.ip.toString() + ")</span>";
    d += " &middot; " + (gCfg.loxSec ? TF(T_c_every_s, String(gCfg.loxSec)) : String(T(T_st_every_cycle)));
    const char* cls = "ok";
    if (!gCfg.wifiEn)       { cls = "bad"; d += String(" &middot; ") + T(T_st_needs_wifi_plain); }
    if (ls.everSent) {
        d += " &middot; " + TF(T_st_lox_last_set, agoText(ls.lastSendMs), String(ls.lastValues));
        if (ls.lastFailed) { cls = "warn"; d += ", " + TF(T_st_n_failed, String(ls.lastFailed)); }
    } else {
        cls = "warn";
        d += String(" &middot; ") + T(T_st_nothing_sent_yet);
    }
    if (ls.errors) {
        d += " &middot; " + TF(T_st_n_errors, String(ls.errors));
        if (ls.lastError) d += ", " + TF(T_st_last_error, ls.lastError, agoText(ls.lastErrorMs));
    } else if (ls.lastError) {
        d += String(" &middot; ") + ls.lastError;   // DNS problem before any set
    }
    static OutStatus os[OUT_COUNT];   // static: not on the web task's stack
    outSnapshot(os);
    if (os[OUT_LOXONE].fails) d += " &middot; " + TF(T_st_out_fails_row, String(os[OUT_LOXONE].fails));
    addStateRow(html, cls, "Loxone UDP", d);
}

// "<br><small>last OK … · last error …: code “text” · n failures in a row</small>" (out_status.h)
String outStatusLine(const OutStatus& s)
{
    if (!s.hasOk && !s.hasErr) return String();
    String d = "<br><small>";
    if (s.hasOk) d += TF(T_st_out_last_ok, agoText(s.okMs));
    if (s.hasErr) {
        if (s.hasOk) d += " &middot; ";
        d += TF(T_st_out_last_err, agoText(s.errMs)) + ": ";
        if (s.code) d += String(s.code) + " ";
        d += "&ldquo;"; appendEscaped(d, s.err); d += "&rdquo;";
    }
    if (s.fails) d += " &middot; " + TF(T_st_out_fails_row, String(s.fails));
    return d + "</small>";
}

void addWeatherNetworkRows(HtmlOut& html)
{
    static OutStatus os[OUT_COUNT];   // static: not on the web task's stack
    outSnapshot(os);
    int wxIdx = 0;
    WxTargetStatus st[WX_TARGET_COUNT];
    wxUploadStatus(st);
    for (const WxTargetStatus& s : st) {
        String d;
        const char* cls;
        if (!s.enabled) {
            cls = "off";
            d   = String("<span class='dim'>") + T(T_st_not_configured) + "</span>";
        } else if (s.lastAttemptMs == 0) {
            cls = "warn";
            d   = TF(T_st_wx_enabled_no_upload, String(s.intervalSec));
            if (s.lastReply[0]) { d += " <span class='dim'>("; appendEscaped(d, s.lastReply); d += ")</span>"; }
        } else if (s.lastOkMs == s.lastAttemptMs) {
            cls = "ok";
            d   = TF(T_st_wx_ok, agoText(s.lastOkMs));
        } else {
            cls = "bad";
            d   = TF(T_st_wx_failed, agoText(s.lastAttemptMs)) + ": ";
            if (s.lastHttpCode > 0) d += "HTTP " + String(s.lastHttpCode) + ", ";
            d += "&ldquo;"; appendEscaped(d, s.lastReply); d += "&rdquo;";
            d += " &middot; " + TF(T_st_wx_last_success, s.lastOkMs ? agoText(s.lastOkMs) : String(T(T_c_never)));
        }
        if (s.enabled && s.lastAttemptMs) {
            d += "<br><small>" + TF(T_st_wx_counts, String(s.okCount), String(s.failCount), String(s.intervalSec));
            const OutStatus& o = os[OUT_WX_FIRST + wxIdx];
            if (o.fails) d += " &middot; " + TF(T_st_out_fails_row, String(o.fails));
            if (o.hasErr && o.hasOk && s.lastOkMs == s.lastAttemptMs)   // OK now, but an error before
                d += " &middot; " + TF(T_st_out_last_err, agoText(o.errMs));
            d += "</small>";
        }
        wxIdx++;
        addStateRow(html, cls, s.name, d);
    }
}

// Status page box "Davis": receiver state and the last value per message type.
// Status page: per-sensor buttons Mine / not Mine and Hide (POST
// /api/rf/sensor, Digest like the settings, back to the status page).
// back: "setup" = return to the wizard (step 2) instead of the status page
String rfQuickActions(uint32_t id, const char* back = nullptr)
{
    char ids[12];
    snprintf(ids, sizeof(ids), "%08lX", (unsigned long)id);
    String f = " <form class='inl' method='POST' action='/api/rf/sensor'><input type='hidden' name='redirect' value='1'>";
    if (back) { f += "<input type='hidden' name='back' value='"; f += back; f += "'>"; }
    f += "<input type='hidden' name='id' value='";
    f += ids;
    f += "'>";
    if (rfStateOf(id) == RF_STATE_MINE)
        f += String("<button class='sb' name='action' value='received' title='") + T(T_st_qa_unmine) + "'>&#9734;</button>";
    else
        f += String("<button class='sb' name='action' value='mine' title='") + T(T_st_qa_mine) + "'>&#9733;</button>";
    f += String("<button class='sb' name='action' value='hide' title='") + T(T_st_qa_hide) + "'>&#10005;</button></form>";
    return f;
}

// radio trace (defined further down)
void addRfTraceStatusRow(HtmlOut& html);
void addRfTraceControls(HtmlOut& html);
void addRfSensorTable(HtmlOut& html);
void sendRfTraceFile(bool attachment);

void addDavisBox(HtmlOut& html)
{
    const DavisDiag& d = gDavisDiag;
    html += "<section><h4>"; html += T(T_st_davis_box); html += "</h4><table>";
    if (!d.running) {
        addStateRow(html, "bad", T(T_st_receiver), d.radioError ? TF(T_st_radio_error, String(d.radioError)) : String(T(T_st_not_running)));
    } else {
        const char* cls = d.state == DAVIS_DIAG_TRACK ? "ok" : d.state == DAVIS_DIAG_RAW ? "warn" : "warn";
        String st = String(davisDiagStateName(d.state));
        if (d.state == DAVIS_DIAG_ACQUIRE) st += String(" &middot; ") + T(T_st_dv_acquire);
        if (d.state == DAVIS_DIAG_LOST)    st += String(" &middot; ") + T(T_st_dv_lost);
        if (d.state == DAVIS_DIAG_RAW)     st += String(" &middot; ") + T(T_st_dv_raw);
        if (d.radioError) { cls = "bad"; st += " &middot; " + TF(T_st_radio_error, String(d.radioError)); }
        addStateRow(html, cls, T(T_st_state), st);
    }
    addRow(html, T(T_st_transmitter_id), d.txId ? String(d.txId) + (gCfg.davisTxId ? "" : T(T_st_first_heard))
                                          : String(gCfg.davisTxId ? String(gCfg.davisTxId) + " <span class='dim'>(" + T(T_st_dv_set_not_heard) + ")</span>" : T(T_st_first_heard_none_yet)));
    float sync = davisSyncPct(d);
    addRow(html, T(T_st_sync), sync >= 0.0f ? String(sync, 1) + " % <span class='dim'>(" + TF(T_st_dv_slots, String(d.trackSlots - d.misses), String(d.trackSlots)) + ")</span>"
                                      : String(T(T_st_not_tracked_yet)));
    addRow(html, T(T_st_last_packet), d.havePacket ? agoText(d.lastPacketMs) + " &middot; RSSI " + String(d.lastRssi, 0) + " dBm" +
                                (d.haveTiming ? " &middot; " + TF(T_st_dv_timing, String(d.lastTimingErrUs / 1000.0f, 1)) : String())
                                : String(T(T_st_none_yet)));
    addRow(html, T(T_st_packets_crc_errors_other_misses), String(d.packets) + " / " + String(d.crcErrors) + " / " + String(d.otherPackets) +
                 " / " + String(d.misses) + (d.resyncs ? " &middot; " + TF(T_st_dv_resyncs, String(d.resyncs)) : String()));
    addRow(html, T(T_st_channel), String(d.channel) + " &middot; " + String(d.freqMHz, 4) + " MHz <span class='dim'>(" +
                 TF(gCfg.davisTable ? T_st_dv_table_classic : T_st_dv_table_measured, String(gCfg.davisRxBw / 10.0f, 1)) + ")</span>");
    addRow(html, T(T_st_period), d.periodMs > 0.0f ? String(d.periodMs, 2) + " ms" : String(T(T_st_unknown_until_the_id_is)));
    addRow(html, T(T_st_frequency_offset), TF(T_st_dv_offset, String(d.offsetHz / 1000.0f, 1), String(d.learnedOffsetHz / 1000.0f, 1)) +
                 (gCfg.davisScanKhz ? " <span class='dim'>(" + TF(T_st_dv_scan, String(gCfg.davisScanKhz)) + ")</span>" : String()) +
                 (gCfg.davisAfc ? T(T_st_afc_on) : String()));
    if (d.hasRainRate) addRow(html, T(T_st_rain_rate_iss), String(rainRateForOutput(d.rainRateMmH, gCfg), 2) + " " + rainRateUnitLabel(gCfg));
    if (d.hasGust)     addRow(html, T(T_st_10_min_gust_iss), String(windSpeedForOutput(d.gust10MinMs, gCfg), 1) + " " + windSpeedUnitLabel(gCfg));
    for (int t = 0; t < 16; t++) {
        const DavisTypeDiag& m = d.types[t];
        if (!m.count) continue;
        char hex[20];
        snprintf(hex, sizeof(hex), "%02X %02X %02X %02X %02X %02X", m.bytes[0], m.bytes[1], m.bytes[2], m.bytes[3], m.bytes[4], m.bytes[5]);
        String v; appendEscaped(v, m.text);
        addRow(html, TF(T_st_dv_message, String(t, HEX), davisDiagTypeName((uint8_t)t)),
               v + " &middot; " + agoText(m.lastMs) + " &middot; " + String(m.count) + "&times; <span class='dim'><code>" + hex + "</code></span>");
    }
    addRow(html, T(T_st_sniffer), String(T(gCfg.davisSniffer ? T_c_on : T_c_off)) +
                 (gCfg.davisRaw ? " &middot; <b>" + TF(T_st_dv_raw_ch, String(gCfg.davisRawCh)) + "</b>" : String()) +
                 " &middot; <a href='/api/log'>" + T(T_c_log) + "</a> &middot; <a href='/api/davis/log'>" + T(T_st_dv_capture) + "</a>");
    html += "</table></section>";
}

// Derived texts of /api/data (English API values) for the status page.
const char* trDerived(const char* en)
{
    static const struct { const char* en; TextId t; } map[] = {
        { "falling very rapidly", T_dv_falling_very_rapidly }, { "falling quickly", T_dv_falling_quickly },
        { "falling", T_dv_falling }, { "falling slowly", T_dv_falling_slowly }, { "steady", T_dv_steady },
        { "rising slowly", T_dv_rising_slowly }, { "rising", T_dv_rising }, { "rising quickly", T_dv_rising_quickly },
        { "rising very rapidly", T_dv_rising_very_rapidly },
        { "improving", T_dv_improving }, { "worsening", T_dv_worsening }, { "storm warning", T_dv_storm_warning },
        { "settled", T_dv_settled }, { "unsettled", T_dv_unsettled }, { "no change", T_dv_no_change },
    };
    for (const auto& m : map) if (en && !strcmp(en, m.en)) return T(m.t);
    return en ? en : "";
}

// Compass point "NNW" in the page language (c_compass: 16 comma-separated points from N).
String trCompass(const char* en)
{
    static const char* const pts[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    int k = -1;
    for (int i = 0; i < 16; i++) if (en && !strcmp(en, pts[i])) k = i;
    if (k < 0) return en ? en : "";
    const char* p = T(T_c_compass);
    for (int i = 0; i < k && p; i++) { p = strchr(p, ','); if (p) p++; }
    if (!p) return en;
    const char* e = strchr(p, ',');
    return e ? String(p).substring(0, e - p) : String(p);
}

// Auto search in progress: every candidate with its packets and signal (rf_autosearch.h)
void addAutoSearchRows(HtmlOut& html)
{
    const AutoSearch& as = rfAutoSearchState();
    const float thr = autoRssiThreshold(boardIsV4());
    for (const AutoCand& c : as.c) {
        if (!c.id) continue;
        char id[12];
        snprintf(id, sizeof(id), "%08lX", (unsigned long)c.id);
        const bool weak = c.meanRssi() < thr;
        addStateRow(html, weak ? "warn" : "ok", (String(T(T_st_auto_candidate)) + " " + id).c_str(),
                    TF(T_st_auto_cand_info, rfProtoName(c.proto), String(c.inWindow), String(AUTO_MIN_PACKETS),
                       String(c.meanRssi(), 0)) + (weak ? String(" &middot; ") + T(T_st_auto_weak) : String()));
    }
}

// Health overview: latest readings, device state, and whether every
// transport/upload is actually working. Plain server-rendered HTML with no
// JS dependency (a fast check for curl/monitoring or a JS-disabled browser);
// reloads itself every 30 s.
void handleStatusPage()
{
    selectLanguage();
    if (webBusyRefused()) return;   // tls_gate.h: an HTTPS request runs and the heap is low
    WebHeavyGuard heavy(millis());   // meanwhile no new HTTPS request starts
    JsonDocument doc;
    buildJsonPayload(doc);

    HtmlOut html(server, 200);
    pageStart(html, T(T_c_status), "/status", 30);
    html += "<p class='sub'>"; html += TF(T_st_sub, FIRMWARE_VERSION); html += "</p>";

    html += "<section><h4>"; html += T(T_st_measurements); html += "</h4><table>";
    bool srcSet = gCfg.rfEn != 0;
    for (int g = 0; g < SG_COUNT; g++) srcSet |= gCfg.srcSel[g] != SRC_AUTO;
    if (srcSet) {   // which sensor feeds each value (settings "Value sources")
        const TextId names[SG_COUNT] = { T_st_src_air, T_st_src_wind, T_st_src_rain, T_st_src_uv, T_st_src_light, T_st_src_pressure };
        String v;
        for (int g = 0; g < SG_COUNT; g++) {
            char d[24]; srcDescribe((SrcGroup)g, d, sizeof(d));
            if (g) v += " &middot; ";
            v += String(T(names[g])) + ": " + d;
        }
        if (gSrc.rfBatteryLow) v += String(" &middot; <b>") + T(T_st_wireless_battery_low) + "</b>";
        addRow(html, T(T_st_sources), "<span class='dim'>" + v + "</span>");
    }
    if (srcActive(SG_WIND)) {
        addRow(html, T(T_st_wind_direction),     String(doc["windDirDeg"].as<int>()) + "&deg;" +
                                           (doc["windDirText"].is<const char*>() ? " (" + trCompass(doc["windDirText"].as<const char*>()) + ")" : String()));
        addRow(html, T(T_st_wind_speed),         String(doc["windSpeed"].as<float>(), 1) + " " + doc["windUnit"].as<const char*>());
        addRow(html, T(T_st_wind_gust),          String(doc["windGust"].as<float>(),  1) + " " + doc["windUnit"].as<const char*>());
    }
    if (srcActive(SG_RAIN)) {
        bool dailyValid;
        float dailyMm = wxDailyRainMm(dailyValid);
        addRow(html, T(T_st_rain_rate),          String(doc["rainRate"].as<float>(), 2)       + " " + doc["rainRateUnit"].as<const char*>());
        addRow(html, T(T_st_rain_this_cycle),  String(doc["rain"].as<float>(), 2)           + " " + doc["rainUnit"].as<const char*>());
        addRow(html, T(T_st_rain_today),         String(rainForOutput(dailyMm, gCfg), 2) + " " + rainUnitLabel(gCfg) +
                                           (dailyValid ? "" : T(T_st_clock_not_synced_yet)));
        addRow(html, T(T_st_rain_last_hour_24_h), String(doc["rain1h"].as<float>(), 2) + " / " + String(doc["rain24h"].as<float>(), 2) +
                                           " " + doc["rainUnit"].as<const char*>());
        if (doc["rainYesterday"].is<float>())
            addRow(html, T(T_st_rain_yesterday), String(doc["rainYesterday"].as<float>(), 2) + " " + doc["rainUnit"].as<const char*>());
        if (doc["rainWeek"].is<float>())
            addRow(html, T(T_st_rain_this_week_month_year), String(doc["rainWeek"].as<float>(), 2) + " / " +
                   String(doc["rainMonth"].as<float>(), 2) + " / " + String(doc["rainYear"].as<float>(), 2) + " " +
                   doc["rainUnit"].as<const char*>() + (gCfg.rainPersist ? "" : T(T_st_ram_only_lost_on_power)));
        else
            addRow(html, T(T_st_rain_this_week_month_year), T(T_st_after_the_first_clock_sync));
        addRow(html, T(T_st_rain_since_start),   String(doc["rainSinceStart"].as<float>(), 2) + " " + doc["rainUnit"].as<const char*>());
        if (doc["lastRain"].is<const char*>()) {
            String lr = doc["lastRain"].as<const char*>();
            time_t t = 0;
            struct tm u = {};
            if (sscanf(lr.c_str(), "%d-%d-%dT%d:%d", &u.tm_year, &u.tm_mon, &u.tm_mday, &u.tm_hour, &u.tm_min) == 5) {
                u.tm_year -= 1900; u.tm_mon -= 1;
                t = mktime(&u);   // UTC string, TZ-independent difference below
                struct tm nowU; time_t now = time(nullptr); gmtime_r(&now, &nowU);
                long ageMin = (long)difftime(mktime(&nowU), t) / 60;
                addRow(html, T(T_st_last_rain), ageMin < 60   ? TF(T_st_ago_min, String(ageMin)) :
                                                ageMin < 2880 ? TF(T_st_ago_hours, String(ageMin / 60)) :
                                                                TF(T_st_ago_days, String(ageMin / 1440)));
            }
        }
    }
    if (gSrc.tempValid)
        addRow(html, T(T_st_temperature),        String(doc["temp"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>());
    if (gSrc.tempValid && gSrc.humValid)
        addRow(html, T(T_st_humidity),           String(doc["humidityPct"].as<float>(), 1) + " %");
    if (gSrc.pressValid) {
        addRow(html, T(T_st_pressure_absolute),String(doc["pressureAbs"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
        addRow(html, T(T_st_pressure_qnh),     String(doc["pressureQnh"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
        if (doc["pressureTrend3h"].is<float>()) {
            float d3 = doc["pressureTrend3h"].as<float>();
            addRow(html, T(T_st_pressure_tendency_3_h), String(d3 >= 0 ? "+" : "") + String(d3, gCfg.unitSystem == 1 ? 2 : 1) + " " +
                   doc["pressureUnit"].as<const char*>() + " &middot; " + trDerived(doc["pressureTendency"].as<const char*>()) +
                   " &middot; " + TF(T_st_trend, String("<b>") + trDerived(doc["weatherTrend"].as<const char*>()) + "</b>"));
        } else {
            addRow(html, T(T_st_pressure_tendency_3_h), T(T_st_available_3_h_after_start));
        }
    }
    if (doc["dewPoint"].is<float>())
        addRow(html, T(T_st_dew_point),          String(doc["dewPoint"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>());
    if (doc["vpd"].is<float>())
        addRow(html, T(T_st_vapour_pressure_deficit), String(doc["vpd"].as<float>(), 2) + " kPa");
    if (doc["feelsLike"].is<float>()) {
        DerivedNow dn;
        derivedNow(dn, windSpeedMs, windDirection);
        addRow(html, T(T_st_feels_like),         String(doc["feelsLike"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>() +
               (dn.windChill ? T(T_st_wind_chill) : dn.heatIndex ? T(T_st_heat_index) : ""));
    }
    if (doc["tempTrend1h"].is<float>()) {
        float t1 = doc["tempTrend1h"].as<float>();
        addRow(html, T(T_st_temperature_trend_1_h), String(t1 >= 0 ? "+" : "") + String(t1, 1) + " K" +
               (gCfg.unitSystem == 1 ? " <span class='dim'>(&deg;F)</span>" : ""));
    }
    if (doc["cloudBase"].is<long>())
        addRow(html, T(T_st_cloud_base),         String(doc["cloudBase"].as<long>()) + " " + doc["cloudBaseUnit"].as<const char*>() +
               " <span class='dim'>(" + T(T_st_above_station) + ")</span>");
    if (doc["sunshineTodayMin"].is<long>()) {
        long m = doc["sunshineTodayMin"].as<long>();
        String v = TF(T_st_h_min, String(m / 60), String(m % 60));
        if (doc["sunshineYesterdayMin"].is<long>()) {
            long y = doc["sunshineYesterdayMin"].as<long>();
            v += " <span class='dim'>&middot; " + TF(T_st_yesterday, TF(T_st_h_min, String(y / 60), String(y % 60))) + "</span>";
        }
        addRow(html, T(T_st_sunshine_today), v);
    }
    if (doc["warnings"].is<JsonObject>()) {
        JsonObject w = doc["warnings"];
        String v;
        if (w["frost"])  { v += T(T_st_warn_frost);  v += ' '; }
        if (w["fog"])    { v += T(T_st_warn_fog);    v += ' '; }
        if (w["sultry"]) { v += T(T_st_warn_sultry); v += ' '; }
        if (v.length()) addRow(html, T(T_st_warnings), "<b>" + v + "</b>");
    }
    if (bus1Count > 0) {
        JsonArray ds18b20 = doc["ds18b20"].as<JsonArray>();
        int i = 0;
        for (JsonVariant v : ds18b20) {
            if (!v.isNull())
                addRow(html, TF(T_st_ds18b20, String(i)), String(v.as<float>(), 1) + "&deg;" + doc["ds18b20Unit"].as<const char*>());
            i++;
        }
    }
    if (gCfg.lipoEn) {   // below 2.5 V: no battery connected, running from USB
        const float bv = doc["batteryVolt"].as<float>();
        addRow(html, T(T_st_battery), bv < BATTERY_PRESENT_V ? String(T(T_st_usb_power)) : String(bv, 3) + " V");
    }
    if (gSrc.uvRadio) addRow(html, T(T_st_uv_index), String(gSrc.uvIndexRadio, 1));
    if (gSrc.lightValid) addRow(html, T(T_st_illuminance), String((long)lroundf(gSrc.lightLux)) + " lx");
    if (ioV2() && gSrc.uvBoard) {
        if (doc["uvSensorVolts"].is<float>())
            addRow(html, T(T_st_uv_sensor_voltage), String(doc["uvSensorVolts"].as<float>(), 3) + " V");
        addRow(html, T(T_st_uv_index_calibration), gIo.uvIndexCalibrated ? T(T_st_calibrated) : T(T_st_not_calibrated));
        if (doc["uvIndex"].is<float>())
            addRow(html, T(T_st_uv_index), String(doc["uvIndex"].as<float>(), 2));
    }
    if (ioV2() && gCfg.wind2Mode) {
        const char* wu = windSpeedUnitLabel(gCfg);
        if (ioWind2DirActive()) addRow(html, T(T_st_wind_2_direction), String(gIo.wind2DirDeg) + "&deg;");
        addRow(html, T(T_st_wind_2_speed), String(windSpeedForOutput(gIo.wind2SpeedMs, gCfg), 1) + " " + wu);
        addRow(html, T(T_st_wind_2_gust),  String(windSpeedForOutput(gIo.wind2GustMs,  gCfg), 1) + " " + wu);
    }
    if (ioV2() && ioAuxAinActive()) addRow(html, T(T_st_aux_analog_input), String(gIo.auxAinV, 3) + " V");
    if (ioV2() && gCfg.auxPulseEn && !gCfg.wind2Mode) addRow(html, T(T_st_aux_pulses), TF(T_st_aux_pulses_val, String(gIo.auxPulsesCycle),
                                          String(gIo.auxPulsesTotal)));
    html += "</table></section>";

    if (ioV2() && (gCfg.heaterMode || gCfg.fanMode || gCfg.tachoEn)) {
        const TextId modeText[] = { T_c_off, T_c_on, T_c_auto };
        html += "<section><h4>"; html += T(T_st_outputs); html += "</h4><table>";
        if (gCfg.heaterMode)
            addStateRow(html, gIo.heaterOn ? "ok" : "off", T(T_st_heater_j12),
                        (gIo.heaterOn ? TF(T_st_on_pct, String(gIo.heaterDuty)) : String(T(T_c_off))) +
                        " &middot; " + TF(T_st_mode, T(modeText[constrain((int)gCfg.heaterMode, 0, 2)])));
        if (gCfg.fanMode || gCfg.tachoEn) {
            String d = gIo.fanOn ? TF(T_st_on_pct, String(gIo.fanDuty)) : String(T(T_c_off));
            d += " &middot; " + TF(T_st_mode, T(modeText[constrain((int)gCfg.fanMode, 0, 2)]));
            if (gCfg.tachoEn) d += " &middot; " + String((int)lroundf(gIo.fanRpm)) + " rpm";
            addStateRow(html, gIo.fanStalled ? "bad" : (gIo.fanOn ? "ok" : "off"), T(T_st_fan_j13),
                        gIo.fanStalled ? d + " &middot; <b>" + T(T_st_no_tacho) + "</b>" : d);
        }
        if (gIo.battLockout)
            addStateRow(html, "warn", T(T_st_battery_lockout), TF(T_st_outputs_held, String(gCfg.outMinBattDv / 10.0f, 1)));
        html += "</table></section>";
    }

    if (ioV2() && gCfg.veDirectEn) {
        static const struct { int cs; TextId text; } csText[] = {
            { 0, T_cs_off }, { 2, T_cs_fault }, { 3, T_cs_bulk }, { 4, T_cs_absorption }, { 5, T_cs_float }, { 7, T_cs_equalize },
            { 245, T_cs_starting }, { 247, T_cs_auto_eq }, { 252, T_cs_external } };
        const VeDirectValues& ve = gIo.ve;
        html += "<section><h4>Victron VE.Direct</h4><table>";
        if (!gIo.veValid) {
            addStateRow(html, "bad", T(T_st_connection), T(T_st_no_valid_data_check_the));
        } else {
            if (!isnan(ve.battV)) addRow(html, T(T_st_battery_voltage), String(ve.battV, 2) + " V");
            if (!isnan(ve.battA)) addRow(html, T(T_st_battery_current), String(ve.battA, 2) + " A");
            if (!isnan(ve.socPct)) addRow(html, T(T_st_state_of_charge), String(ve.socPct, 1) + " %");
            if (!isnan(ve.loadA))  addRow(html, T(T_st_load_output_current), String(ve.loadA, 2) + " A");
            if (!isnan(ve.pvV))   addRow(html, T(T_st_pv_voltage), String(ve.pvV, 2) + " V");
            if (!isnan(ve.pvW))   addRow(html, T(T_st_pv_power), String(ve.pvW, 0) + " W");
            if (!isnan(ve.yieldTodayKwh)) addRow(html, T(T_st_yield_today), String(ve.yieldTodayKwh, 2) + " kWh");
            if (ve.chargeState >= 0) {
                String t = String(ve.chargeState);
                for (const auto& c : csText) if (c.cs == ve.chargeState) { t = T(c.text); break; }
                addRow(html, T(T_st_charge_state), t);
            }
            if (ve.error >= 0)
                addStateRow(html, ve.error ? "bad" : "ok", T(T_st_error), ve.error ? TF(T_st_code, String(ve.error)) : String(T(T_c_none)));
        }
        html += "</table></section>";
    }

    html += "<section><h4>"; html += T(T_st_connections); html += "</h4><table>";
    const String disabled = String("<span class='dim'>") + T(T_c_disabled) + "</span>";
    static OutStatus os[OUT_COUNT];   // send status per output (out_status.h)
    outSnapshot(os);
    const char* fmtName = gCfg.payloadFormat ? T(T_st_binary) : "CayenneLPP";
    if (!gCfg.loraEn)            addStateRow(html, "off", "LoRaWAN", disabled);
    else if (davisLoraPaused())  addStateRow(html, "warn", "LoRaWAN", T(T_st_lorawan_paused_davis_mode_the));
    else if (!loraReady())      addStateRow(html, "warn", "LoRaWAN", T(T_st_not_joined_automatic_retries_measurem) + outStatusLine(os[OUT_LORAWAN]));
    else if (sendFailCount == 0 && lastPayloadDropped[0])
        addStateRow(html, "warn", "LoRaWAN", TF(T_st_uplinks_ok, fmtName) + " &middot; " + TF(T_st_left_out, lastPayloadDropped) + outStatusLine(os[OUT_LORAWAN]));
    else if (sendFailCount == 0) addStateRow(html, "ok",  "LoRaWAN", TF(T_st_uplinks_ok, fmtName) + outStatusLine(os[OUT_LORAWAN]));
    else                         addStateRow(html, "bad", "LoRaWAN", TF(T_st_send_failures, String(sendFailCount)) + outStatusLine(os[OUT_LORAWAN]));

    if (!gCfg.wifiEn) {
        addStateRow(html, "off", "WiFi", disabled);
    } else if (wifiIsConnected()) {
        String d;
        appendEscaped(d, WiFi.SSID().c_str());
        char host[HOSTNAME_MAX_LEN + 1];
        deviceHostname(host, sizeof(host));   // DHCP and mDNS name
        d += " &middot; " + WiFi.localIP().toString() + " &middot; " + String(host) + ".local &middot; " +
             String(WiFi.RSSI()) + " dBm";
        addStateRow(html, "ok", "WiFi", d);
    } else {
        addStateRow(html, "bad", "WiFi", T(T_st_disconnected));
    }

    if (!gCfg.mqttEn) {
        addStateRow(html, "off", "MQTT", disabled);
    } else {
        String d;
        appendEscaped(d, gCfg.mqttHost);
        d += ":" + String(gCfg.mqttPort);
        addStateRow(html, mqttIsConnected() ? "ok" : "bad", "MQTT",
                    String(T(mqttIsConnected() ? T_st_connected : T_st_disconnected)) + " &middot; " + d + outStatusLine(os[OUT_MQTT]));
        if (os[OUT_HA].hasOk || os[OUT_HA].hasErr)
            addStateRow(html, os[OUT_HA].fails ? "bad" : "ok", T(T_st_ha_discovery), outStatusLine(os[OUT_HA]).substring(4));
    }

    addLoxoneRow(html);

    struct tm lt;
    if (!gCfg.wifiEn) {
        addStateRow(html, "off", T(T_st_clock_ntp), T(T_st_needs_wifi));
    } else if (localTimeNow(lt)) {
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &lt);
        addStateRow(html, "ok", T(T_st_clock_ntp), TF(T_st_synced, buf));
    } else {
        addStateRow(html, "warn", T(T_st_clock_ntp), T(T_st_waiting_for_first_sync));
    }
    html += "</table></section>";

    html += "<section><h4>"; html += T(T_st_weather_networks); html += "</h4><table>";
    addWeatherNetworkRows(html);
    {   // custom uploads (custom_upload.h)
        for (int i = 0; i < CUSTOM_UPLOADS; i++) {
            customUploadLock();
            const bool on = customUploads()[i].url[0] != '\0';
            customUploadUnlock();
            if (!on) continue;
            const OutStatus& o = os[OUT_CUSTOM_FIRST + i];
            const String line = outStatusLine(o);
            addStateRow(html, o.fails ? "bad" : (o.hasOk ? "ok" : "warn"), TF(T_cu_url, String(i + 1)).c_str(),
                        line.length() ? line.substring(4) : String(T(T_st_nothing_sent_yet)));
        }
    }
    html += "</table></section>";

    {   // alarms (alarms.h): active rules and the channels' send status
        std::unique_ptr<AlarmRule[]> rules(new (std::nothrow) AlarmRule[ALARM_RULES]);
        std::unique_ptr<AlarmState[]> st(new (std::nothrow) AlarmState[ALARM_RULES]);
        std::unique_ptr<AlarmChannels> ch(new (std::nothrow) AlarmChannels());
        if (rules && st && ch) {
            alarmsSnapshot(rules.get(), st.get());
            alarmsLock();
            *ch = alarmsChannels();
            alarmsUnlock();
            int defined = 0;
            for (int i = 0; i < ALARM_RULES; i++) defined += rules[i].src != AS_OFF;
            if (defined) {
                html += "<section id='alarms'><h4>"; html += T(T_st_alarms); html += "</h4><table>";
                int active = 0;
                for (int i = 0; i < ALARM_RULES; i++) {
                    if (!st[i].active) continue;
                    active++;
                    String name;
                    appendEscaped(name, rules[i].name[0] ? rules[i].name : alarmSrcKey(rules[i].src));
                    addStateRow(html, rules[i].priority >= 4 ? "bad" : "warn", name.c_str(),
                                TF(T_st_al_active, agoSeconds(millis() / 1000 - st[i].since)));
                }
                if (!active) addStateRow(html, "ok", T(T_st_alarms), TF(T_st_al_none, String(defined)));
                const bool conf[4] = { ch->ntfyTopic[0] != 0, ch->poToken[0] && ch->poUser[0], ch->tgToken[0] && ch->tgChat[0], ch->whUrl[0] != 0 };
                static const char* const names[4] = { "ntfy", "Pushover", "Telegram", "Webhook" };
                for (int c = 0; c < 4; c++) {
                    if (!conf[c]) continue;
                    const OutStatus& o = os[OUT_NOTIFY_FIRST + c];
                    const String line = outStatusLine(o);
                    addStateRow(html, o.fails ? "bad" : (o.hasOk ? "ok" : "off"), names[c],
                                line.length() ? line.substring(4) : String(T(T_st_nothing_sent_yet)));
                }
                html += "</table></section>";
            }
        }
    }

    if (gCfg.loraEn) {
        html += "<section><h4>LoRaWAN</h4><table>";
        const LoraDiag& ld = gLoraDiag;
        if (!ld.haveUplink) {
            addRow(html, T(T_st_uplinks), T(T_st_none_since_start));
        } else {
            // DR0-5 = SF12-SF7 at 125 kHz in EU868 and the other 1-channel-plan
            // regions; US915/AU915 use a different table, show the DR only there.
            String dr = "DR" + String(ld.dataRate);
            bool euLike = strncmp(gCfg.region, "US", 2) != 0 && strncmp(gCfg.region, "AU", 2) != 0;
            if (euLike && ld.dataRate <= 5) dr += " (SF" + String(12 - ld.dataRate) + ")";
            dr += " &middot; " + String(ld.freqMHz, 1) + " MHz &middot; " + String(ld.txPowerDbm) + " dBm";
            if (gBoard.fem != FEM_NONE)
                dr += " <span class='dim'>(SX1262 " + String(femChipPowerDbm(ld.txPowerDbm, gBoard.fem)) + " dBm + " +
                      femName(gBoard.fem) + " PA)</span>";
            addRow(html, T(T_st_last_uplink), dr);
            unsigned long cfgMs = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
            if (ld.intervalMs > cfgMs)
                addRow(html, T(T_st_uplink_interval), TF(T_st_uplink_stretched, String(ld.intervalMs / 1000), String(gCfg.uplinkIntervalSec)));
            char addr[12];
            snprintf(addr, sizeof(addr), "%08lX", (unsigned long)ld.devAddr);
            addRow(html, T(T_st_device_address_fcnt_up), String(addr) + " &middot; " + String(ld.fCntUp));
        }
        if (ld.haveDownlink) {
            addRow(html, T(T_st_last_downlink), agoText(ld.lastDownlinkMs) + " &middot; RSSI " + String(ld.lastDlRssi, 0) + " dBm &middot; SNR " +
                                          String(ld.lastDlSnr, 1) + " dB");
        } else {
            addRow(html, T(T_st_last_downlink), T(T_st_none_since_start));
        }
        if (ld.rejoinThreshold)
            addRow(html, T(T_st_uplinks_without_downlink), String(ld.uplinksWithoutDownlink) + " / " + String(ld.rejoinThreshold) +
                   " <span class='dim'>(" + T(T_st_rejoin_limit) + ")</span>");
        addRow(html, T(T_st_rejoins_since_start), String(ld.rejoins));
        html += "</table></section>";
    }

    if (gCfg.rfEn) {
        html += "<section id='rfsensors'><h4>"; html += T(T_st_wireless_box); html += "</h4><table>";
        if (rfDavisMode(gCfg.rfEn, gCfg.rfProto))
            addStateRow(html, gDavisDiag.running ? "ok" : "bad", T(T_st_receiver), T(T_st_davis_exclusive_mode_see_the));
        else if (rfSensorsReceiving())
            addStateRow(html, "ok", T(T_st_receiver), TF(gCfg.loraEn ? T_st_listening_lora : T_st_listening, rfSensorsListening()) +
                        " &middot; " + TF(T_st_packets_since_start, String(rfSensorsPacketsTotal())) +
                        (rfSensorsScheduled() ? String(" &middot; ") + TF(T_st_rf_scheduled, String(rfSensorsScheduled())) : String()));
        else if (rfSensorsLastError() == 0)   // LoRaWAN uses the chip right now (uplink + receive windows)
            addStateRow(html, "ok", T(T_st_receiver), String(T(gCfg.loraEn ? T_st_paused_for_a_lorawan_uplink : T_st_starting)) + " &middot; " +
                        TF(T_st_packets_since_start, String(rfSensorsPacketsTotal())));
        else
            addStateRow(html, "bad", T(T_st_receiver), TF(T_st_not_receiving, String(rfSensorsLastError())));
        if (gCfg.rfProto == RF_PROTO_DAVIS)
            addRow(html, T(T_st_sensor_type), rfSensorTypeText(gCfg.rfProto));
        else if (gCfg.rfProto == RF_PROTO_AUTO) {
            addStateRow(html, "warn", T(T_st_sensor_type), T(gCfg.rfIds[0] ? T_st_auto_searching_mine : T_st_auto_searching));
            addAutoSearchRows(html);   // candidates so far (rf_autosearch.h)
        } else if (rfSensorsDetectedProto())
            addStateRow(html, rfAutoLockedWeak() ? "warn" : "ok", T(T_st_sensor_type),
                        TF(T_st_detected_type, rfSensorTypeText(rfSensorsDetectedProto())) +
                        " <span class='dim'>(" + T(T_st_auto_saved) + ")</span>" +
                        (rfAutoLockedWeak() ? String(" &middot; <b>") + T(T_st_auto_weak) + "</b>" : String()));
        else
            addRow(html, T(T_st_sensor_type), rfSensorTypeText(gCfg.rfProto));
        bool any = false;
        for (int i = 0; i < RF_MAX_SENSORS; i++) {
            const RfSensorReading& r = gRfSensors[i];
            if (!r.used) continue;
            any = true;
            char id[12];
            snprintf(id, sizeof(id), "%08lX", (unsigned long)r.id);
            unsigned long ago = (millis() - r.lastMs) / 1000;
            bool gone = (r.staleMask & RF_VAL_ANY) && ago < rfFreshMs(r) / 1000;   // millis() wrapped
            String v = TF(T_st_age_rssi, gone ? "&gt;" + agoSeconds(rfFreshMs(r) / 1000) : agoSeconds(ago), String(r.rssi, 0));
            if (!r.batteryOk) v += String(" &middot; <b>") + T(T_st_battery_low) + "</b>";
            if (r.batteryMv) v += " &middot; " + String(r.batteryMv / 1000.0f, 2) + " V";
            auto stale = [i](const char* key) { return srcRfValueFresh(i, key) ? String() : String(T(T_st_stale)); };
            if (r.hasTemp)     v += " &middot; " + String(temperatureForOutput(r.tempC, gCfg), 1) + "&deg;" + temperatureUnitLabel(gCfg) + stale("temp");
            if (r.hasHumidity) v += " &middot; " + String(r.humidity) + " %" + stale("humidity");
            if (r.hasWind)     v += " &middot; " + TF(T_st_wind_gust_v, String(windSpeedForOutput(r.windAvgMs, gCfg), 1),
                                    String(windSpeedForOutput(r.windGustMs, gCfg), 1)) + " " + windSpeedUnitLabel(gCfg) +
                                    " &middot; " + trCompass(windDirText((int)lroundf(r.windDirDeg))) + " " +
                                    String((int)lroundf(r.windDirDeg)) + "&deg;" + stale("wind");
            RfStat* rst = (r.hasRain || r.hasLightning) ? rfStatFor(r.id, false) : nullptr;
            uint32_t st_t = 0, st_ymd = 0;
            if (rst) { rfStatsClock(st_t, st_ymd); rfStatRoll(*rst, st_t, st_ymd); }
            if (r.hasRain) {
                const int dec = gCfg.unitSystem == 1 ? 2 : 1;
                const String ru = String(" ") + rainUnitLabel(gCfg);
                if (rst && rst->ymd) v += " &middot; " + TF(T_st_rf_rain_today, String(rainForOutput(rst->rainTodayMm, gCfg), dec) + ru);
                if (rst) v += " &middot; " + TF(T_st_rain_1h, String(rainForOutput(rfStatRain1h(*rst, st_t), gCfg), dec) + ru);
                v += " &middot; <span class='dim'>" + TF(T_st_rain_total, String(rainForOutput(r.rainTotalMm, gCfg), 1) + ru) + "</span>" + stale("rain");
            }
            if (r.hasUv)       v += " &middot; UV " + String(r.uvIndex, 1) + stale("uv");
            if (r.hasLight)    v += " &middot; " + String((long)lroundf(r.lightLux)) + " lx" + stale("light");
            if (r.hasMoisture) v += " &middot; " + TF(T_st_soil_moisture, String(r.moisturePct));
            if (r.hasSolar)    v += " &middot; " + TF(T_st_solar, String((long)lroundf(r.solarWm2)));
            if (r.hasSupercap) v += " &middot; " + TF(T_st_supercap, String(r.supercapV, 1));
            if (r.hasLightning) {   // strikes from the counter's increments (rf_stats.h); the counter itself only as diagnostic
                if (rst && rst->ymd) v += " &middot; " + TF(T_st_strikes_today, String(rst->strikesToday));
                if (rst) v += " &middot; " + TF(T_st_strikes_1h, String(rfStatStrikes1h(*rst, st_t)));
                if (rst && rst->lastStrike) {
                    time_t tt = (time_t)rst->lastStrike;
                    struct tm lt; localtime_r(&tt, &lt);
                    char hm[8]; strftime(hm, sizeof(hm), "%H:%M", &lt);
                    v += " &middot; " + TF(T_st_last_strike, hm);
                }
                if (r.lightningKm) v += " &middot; " + TF(T_st_storm_km, String(r.lightningKm));
                v += " &middot; <span class='dim'>" + TF(T_st_lightning_count, String(r.lightningCount)) + "</span>";
            }
            if (r.hasPressure) v += " &middot; " + String(pressureForOutput(r.pressureHpa, gCfg), gCfg.unitSystem == 1 ? 2 : 1) +
                                    " " + pressureUnitLabel(gCfg);
            if (r.hasPm25)     v += " &middot; PM2.5 " + String(r.pm25, 1) + " &micro;g/m&sup3;";
            if (r.hasPm10)     v += String(" &middot; PM10 ") + (r.pm10Estimated ? String(T(T_st_est)) + " " : String()) + String(r.pm10, 1) + " &micro;g/m&sup3;";
            if (r.hasPm1)      v += " &middot; PM1 " + String(r.pm1, 1) + " &micro;g/m&sup3;";
            if (r.hasPm4)      v += " &middot; PM4 " + String(r.pm4, 1) + " &micro;g/m&sup3;";
            if (r.hasCo2)      v += " &middot; CO&#8322; " + String(r.co2Ppm) + " ppm";
            if (r.extPower)    v += String(" &middot; ") + T(T_st_usb_powered);
            if (r.hasLeak)     v += String(" &middot; ") + (r.leak ? String("<b>") + T(T_st_leak) + "</b>" : String(T(T_st_dry))) + " (" +
                                    TF(r.leakHighSensitivity ? T_st_leak_raw_high : T_st_leak_raw_low, String(r.leakRaw)) + ")";
            if (r.hasLeafWetness)  v += " &middot; " + TF(T_st_leaf_wetness, String(r.leafWetnessPct));
            if (r.hasConductivity) v += " &middot; EC " + String((long)lroundf(r.conductivityUsCm)) + " &micro;S/cm";
            String kind = r.proto == RF_PROTO_BRESSER ? TF(T_st_type_n, String(r.type)) : String(rfProtoName(r.proto));
            if (r.model)   kind += String(" ") + r.model;
            if (r.channel) kind += " ch " + String(r.channel);
            int slot = rfSlotOfIndex(i);   // 0-based; 0..3 = value sources Wireless 1-4
            String label = slot >= 0 ? TF(T_st_wireless_n, String(slot + 1)) + " &middot; " : String();
            if (rfStateOf(r.id) == RF_STATE_MINE) label = "&#9733; " + label;
            const char* nm = rfNameOf(r.id);
            if (nm[0]) { appendEscaped(label, nm); label += " &middot; "; }
            uint32_t eco;
            String ecoText;
            if (r.proto == RF_PROTO_FINEOFFSET && rfEcowittId(r.id, eco)) { char e[8]; snprintf(e, sizeof(e), "%lX", (unsigned long)eco); ecoText = TF(T_st_ecowitt_id, e); }
            addRow(html, label + "ID " + id + ecoText + " (" + kind + ")", v + rfQuickActions(r.id));
        }
        {
            int hidden = rfHiddenInRange(millis());
            if (hidden)
                addRow(html, T(T_st_hidden_sensors), TF(T_st_hidden_in_range, String(hidden)));
        }
        if (!any) addRow(html, T(T_st_sensors), T(T_st_none_received_yet_bresser_sensors));
        if (gCfg.mqttEn && !rfDavisMode(gCfg.rfEn, gCfg.rfProto)) {
            size_t listed = rfParseIds(gCfg.rfIds).size();
            if (listed)
                addRow(html, T(T_st_home_assistant), TF(T_st_ha_devices, String((unsigned)listed)));
            else
                addRow(html, T(T_st_home_assistant), T(T_st_no_device_per_wireless_sensor));
        }
        addRfTraceStatusRow(html);
        html += "</table></section>";
        if (rfDavisMode(gCfg.rfEn, gCfg.rfProto)) addDavisBox(html);
    }

    html += "<section><h4>"; html += T(T_st_device); html += "</h4><table>";
    if (gCfg.debugMode) addRow(html, T(T_st_measurement_cycle), String(doc["cycle"].as<int>()));
    addRow(html, T(T_st_next_measurement_in), String(doc["nextCycleInSec"].as<uint32_t>()) + " s");
    {
        String b = String("Heltec WiFi LoRa 32 ") + boardName(gBoard.kind);
        if (boardIsV4()) b += gBoard.fem == FEM_KCT8103L ? ".3 (KCT8103L front end)" : ".2 (GC1109 front end)";
        b += " <span class='dim'>(" + (gBoard.autoSel ? TF(T_st_board_detected, String(gBoard.flashBytes >> 20))
                                                      : TF(T_st_board_manual, boardName(gBoard.detected), String(gBoard.flashBytes >> 20))) + ")</span>";
        if (boardIsR8()) b += String(" <b>&ndash; ") + T(T_st_r8_unsupported) + "</b>";
        addRow(html, T(T_st_board), b);
    }
    if (boardPcbAllowed())
        addRow(html, T(T_st_pcb_revision),   gCfg.pcbVersion == PCB_REV_2_0
                                       ? String(gIo.hwIdDetected ? T(T_st_v2_0_detected) : T(T_st_v2_0_set_manually)) +
                                         (boardIsV4() ? String(" &middot; ") + T(T_st_v4_sockets) : String())
                                       : String(gCfg.pcbVersion == PCB_REV_1_2 ? "Rev 1.2" : "Rev 1.3"));
    else
        addRow(html, "PCB",            T(T_st_heltec_v4_wireless_build_only));
    addRow(html, T(T_st_bme280_sensor),      bme280Present ? T(T_st_present) : T(T_st_not_detected));
    addRow(html, T(T_st_ds18b20_sensors),    String(bus1Count));
    addRow(html, T(T_st_cpu_temperature),    String(doc["cpuTemp"].as<float>(), 1) + "&deg;" + doc["cpuTempUnit"].as<const char*>() +
                                       " <span class='dim'>(" + T(T_st_chip_above) + ")</span>");
    addRow(html, T(T_st_uptime),             String((float)millis() / 3600000.0f, 2) + " h");
    if (gCfg.updateCheckEn) {
        char st[48]; wxUpdateStatus(st, sizeof(st));
        char up[24]; wxUpdateAvailable(up, sizeof(up));
        String v;
        if (up[0]) {
            String u; appendEscaped(u, up);
            v = "<b>" + TF(T_st_available, u) + "</b> &middot; <a href='/api/settings#system'>" + T(T_ota_title) + "</a>";
        }
        else       appendEscaped(v, st);
        addRow(html, T(T_st_firmware_update), v);
    }
    addRow(html, T(T_st_last_restart),       gResetReason);
    addRow(html, T(T_st_free_heap),          TF(T_st_heap, String((float)ESP.getFreeHeap() / 1024.0f, 1),
                                       String((float)ESP.getMinFreeHeap() / 1024.0f, 1)) + " &middot; " +
                                       TF(T_st_heap_largest, String((float)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024.0f, 1)));
    html += "</table></section>";

    html += "<p class='dim'><a href='/api/data'>"; html += T(T_st_raw_json); html += "</a></p></body></html>";
    html.finish();
}

// Gates both viewing and saving the settings page behind HTTP Digest Auth
// (browser's built-in login prompt) using the admin password configured via
// the WiFi portal. Digest rather than Basic: the settings page can change
// LoRaWAN keys and MQTT credentials, and Basic would put the password on
// the (unencrypted) network in plain text with every request. No password
// set at all -> access is refused outright rather than prompting for one
// that can never match.
bool requireAuth()
{
    if (gCfg.webAdminPass[0] == '\0') {
        server.send(403, "text/plain; charset=utf-8", T(T_st_forbidden_no_admin_password_configure));
        return false;
    }
    if (!server.authenticate("admin", gCfg.webAdminPass)) {
        server.requestAuthentication(DIGEST_AUTH, "WeatherNode settings");
        return false;
    }
    return true;
}

// CSRF guard for state-changing POSTs. The browser re-sends cached Digest
// credentials with any request to the node, so a foreign web page could
// otherwise submit the settings form for a logged-in user. Browsers mark
// the page a form was sent from in Origin (or at least Referer): its host
// must be the one the request is addressed to (Host). Requests carrying
// neither header (curl, scripts) are allowed - they carry no browser-cached
// credentials. "Origin: null" (sandboxed/privacy contexts) is refused.
// The setup wizard and what it posts: open while no admin password is set
// and the setup is not done (a new node in its own network - the wizard's
// password step closes it), else the normal Digest login (setup_wizard.h).
bool requireWizardAuth()
{
    if (wizardOpenAccess(gCfg)) return true;
    return requireAuth();
}

bool sameOriginRequest()
{
    String src = server.header("Origin");
    if (src.isEmpty()) src = server.header("Referer");
    if (src.isEmpty()) return true;
    int start = src.indexOf("://");
    if (start < 0) return false;
    start += 3;
    int end = src.indexOf('/', start);
    String host = end < 0 ? src.substring(start) : src.substring(start, end);
    if (host.isEmpty() || !host.equalsIgnoreCase(server.hostHeader())) {
        dbgSerial.printf("Web: cross-site POST refused (from '%s', host '%s')\n", host.c_str(), server.hostHeader().c_str());
        return false;
    }
    return true;
}

bool requireSameOrigin()
{
    if (sameOriginRequest()) return true;
    server.send(403, "text/plain; charset=utf-8", T(T_st_forbidden_cross_site_request_open));
    return false;
}


void handleLogPage()
{
    selectLanguage();
    if (!requireAuth()) return;
    HtmlOut html(server, 200);
    pageStart(html, T(T_log_title), "/api/log");
    html += FPSTR(LOG_PAGE_STYLE);
    addRfTraceControls(html);
    html += "<h4>"; html += T(T_log_title); html += "</h4>";
    html += "<p class='sub'>"; html += T(T_log_intro); html += "</p>";
    html += "<button class='logbtn' id='toggle'>"; html += T(T_log_pause); html += "</button>";
    html += "<button class='logbtn' id='clear'>"; html += T(T_log_clear); html += "</button>";
    html += "<pre id='log'>"; html += T(T_log_loading); html += "</pre>";
    html += "<script>var L={pause:"; jsString(html, T(T_log_pause)); html += ",resume:"; jsString(html, T(T_log_resume));
    html += ",failed:"; jsString(html, T(T_log_failed)); html += "};</script>";
    html += FPSTR(LOG_PAGE_SCRIPT);
    html += "</body></html>";
    html.finish();
}

// Settings -> System, buttons "reset records" / "delete chart history" /
// "reset rain / sunshine statistics" / "reset AUX pulse total": what =
// records (today .. this year), recordsall (also all-time), history, rain,
// sunshine or aux.
// Done in the loop (records / history store), back to the System tab.
void handleSettingsReset()
{
    selectLanguage();
    if (!requireAuth()) return;
    const String w = server.arg("what");
    if (w == "records" || w == "recordsall") recordsResetRequest(w == "recordsall");
    else if (w == "history") historyClearRequest();
    else if (w == "rain") rainStatsReset();
    else if (w == "sunshine") sunshineStatsReset();
    else if (w == "aux") cmdResetAuxPulses();
    else { server.send(400, "text/plain", "what: records, recordsall, history, rain, sunshine or aux"); return; }
    server.sendHeader("Location", "/api/settings#system");
    server.send(303, "text/plain", "");
}

// The settings' wireless sensor table alone (live refresh, see handleSettingsGet()).
void handleRfSensorTable()
{
    selectLanguage();
    if (!requireAuth()) return;
    HtmlOut html(server, 200);
    addRfSensorTable(html);
    html.finish();
}

void handleLogRaw()
{
    selectLanguage();
    if (!requireAuth()) return;
    // heap while sending, not 8 KB of static RAM for a rarely used page
    std::unique_ptr<char[]> buf(new (std::nothrow) char[4096]);   // = the log buffer (serial_log.cpp)
    if (!buf) { server.send(503, "text/plain", "low memory, try again"); return; }
    dbgSerial.snapshot(buf.get(), 4096);
    server.send(200, "text/plain; charset=utf-8", buf.get());
}

// Davis sniffer capture: now the radio trace (Davis mode starts
// one); same file as GET /api/rf/trace, shown inline.
void handleDavisLog()
{
    selectLanguage();
    if (!requireAuth()) return;
    sendRfTraceFile(false);
}

// ---- Radio trace (rf_trace.h): status page rows, REST handlers ----
char rfTraceMessage[200] = "";   // result of the last start/stop (shown once on the status page)

String rfTraceFilterText(const RfTraceStatus& st)
{
    if (st.filter == RF_TRACE_PROBLEMS) return T(T_tr_only_problems);
    if (st.filter == RF_TRACE_ONE_ID) {
        char b[12];
        snprintf(b, sizeof(b), "%08lX", (unsigned long)st.filterId);
        return TF(T_tr_only_id, b);
    }
    return T(T_tr_all);
}

// State line (recording / finished, filter, progress, time left).
String rfTraceStateText(const RfTraceStatus& st)
{
    String v = st.running ? String("<b>") + T(T_tr_recording) + "</b>" : String(T(T_tr_finished));
    if (!st.running && st.stopReason[0]) {
        // the reasons rf_trace.cpp / this file set (English in the trace file)
        const char* r = !strcmp(st.stopReason, "time is up") ? T(T_tr_reason_time) :
                        !strcmp(st.stopReason, "stopped by the user") ? T(T_tr_reason_user) :
                        !strcmp(st.stopReason, "Davis mode left") ? T(T_tr_reason_davis) : nullptr;
        v += " (";
        if (r) v += r; else appendEscaped(v, st.stopReason);
        v += ")";
    }
    v += " &middot; " + rfTraceFilterText(st) + " &middot; " +
         TF(T_tr_progress, String(st.seen), String(st.recorded), String(st.usedBytes * 100UL / RF_TRACE_SIZE));
    if (st.overwrittenBytes) v += String(" (") + T(T_tr_overwritten) + ")";
    if (st.running) v += " &middot; " + (st.durationMs ? TF(T_tr_min_left, String((st.leftMs + 59999) / 60000))
                                                       : String(T(T_tr_until_stopped)) + (st.autoStarted ? String(" (") + T(T_tr_davis_mode) + ")" : String()));
    return v;
}

// Status page: one compact row; the controls live on the (password
// protected) log page, so a visitor sees no buttons that end in a login.
void addRfTraceStatusRow(HtmlOut& html)
{
    const RfTraceStatus st = rfTraceStatusNow();
    String v;
    if (st.allocated) v += rfTraceStateText(st) + "<br>";
    v += T(T_tr_on_log_page);
    addRow(html, T(T_st_radio_trace_short), v);
}

// Log page: the full radio trace controls (start / stop / download / discard).
void addRfTraceControls(HtmlOut& html)
{
    const RfTraceStatus st = rfTraceStatusNow();
    String v;
    if (rfTraceMessage[0]) { v += "<b>"; appendEscaped(v, rfTraceMessage); v += "</b><br>"; rfTraceMessage[0] = '\0'; }
    if (!gCfg.rfEn && !st.allocated) v += String(T(T_st_wireless_sensors_are_off)) + "<br>";
    if (st.allocated) {
        v += rfTraceStateText(st);
        v += String("<br><a href='/api/rf/trace'>") + T(T_tr_download) + "</a> ";
        v += "<form method='POST' action='/api/rf/trace' style='display:inline'><input type='hidden' name='redirect' value='1'>";
        if (st.running) v += String("<button name='action' value='stop'>") + T(T_tr_stop) + "</button> ";
        v += String("<button name='action' value='clear'>") + T(T_tr_discard) + "</button></form>";
        v += String("<br><span class='dim'>") + T(T_tr_kept) + "</span><br>";
    }
    if (!st.running) {
        v += "<form method='POST' action='/api/rf/trace'><input type='hidden' name='redirect' value='1'>"
             "<input type='hidden' name='action' value='start'>"
             "<select name='minutes'><option value='5'>5 min</option><option value='15' selected>15 min</option>"
             "<option value='60'>60 min</option></select> "
             "<select name='filter'><option value='all'>";
        v += T(T_tr_all);
        v += "</option><option value='problems'>";
        v += T(T_tr_only_unknown);
        v += "</option><option value='id'>";
        v += T(T_tr_only_this_id);
        // only shown while "only this ID" is selected (data-dep, see pageStart())
        v += "</option></select> <input name='id' size='9' maxlength='8' data-dep='filter=id' placeholder='";
        v += T(T_tr_hex_id);
        v += "'> <button type='submit'>";
        v += T(T_tr_start);
        v += "</button></form>";
    }
    v += String("<span class='dim'>") + T(T_tr_note) + "</span>";
    html += "<section><h4>"; html += T(T_st_radio_trace); html += "</h4><p>"; html += v; html += "</p></section>";
}

void rfTraceWriteChunked(const char* s, size_t n, void* ctx)
{
    static_cast<Print*>(ctx)->write((const uint8_t*)s, n);
}

String rfTraceReceiverInfo()
{
    String r = rfDavisMode(gCfg.rfEn, gCfg.rfProto) ? String("Davis exclusive mode")
             : rfSensorsReceiving() ? String("receiving, now listening to ") + rfSensorsListening()
             : String("not receiving (last radio error ") + String(rfSensorsLastError()) + ")";
    r += ", " + String(rfSensorsPacketsTotal()) + " packets since start";
    if (rfSensorsDetectedProto()) r += ", auto search found type " + String(rfSensorsDetectedProto());
    if (rfSensorsScheduled()) r += ", timed reception for " + String(rfSensorsScheduled()) + " sensors";
    return r;
}

void sendRfTraceFile(bool attachment)
{
    char host[HOSTNAME_MAX_LEN + 1];
    deviceHostname(host, sizeof(host));
    if (attachment) {
        char name[96];
        rfTraceFileName(name, sizeof(name), host);
        server.sendHeader("Content-Disposition", String("attachment; filename=\"") + name + "\"");
    }
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain; charset=utf-8", "");
    ChunkedPrint out(server);
    String info = rfTraceReceiverInfo();
    rfTraceWriteFile(rfTraceWriteChunked, &out, host, info.c_str());
    out.flush();
}

void sendRfTraceStatusJson(bool ok, const char* error)
{
    const RfTraceStatus st = rfTraceStatusNow();
    JsonDocument doc;
    doc["ok"] = ok;
    if (error && error[0]) doc["error"] = error;
    doc["allocated"] = st.allocated;
    doc["running"] = st.running;
    static const char* const filters[] = { "all", "problems", "id" };
    doc["filter"] = filters[st.filter <= RF_TRACE_ONE_ID ? st.filter : 0];
    if (st.filter == RF_TRACE_ONE_ID) {
        char id[12];
        snprintf(id, sizeof(id), "%08lX", (unsigned long)st.filterId);
        doc["id"] = id;
    }
    doc["autoStarted"] = st.autoStarted;
    doc["durationSec"] = st.durationMs / 1000;
    doc["elapsedSec"] = st.elapsedMs / 1000;
    doc["leftSec"] = st.leftMs / 1000;
    doc["packets"] = st.seen;
    doc["lines"] = st.recorded;
    doc["bufferBytes"] = st.usedBytes;
    doc["bufferSize"] = (uint32_t)RF_TRACE_SIZE;
    doc["overwrittenBytes"] = st.overwrittenBytes;
    if (!st.running && st.stopReason[0]) doc["stopReason"] = st.stopReason;
    String body;
    serializeJson(doc, body);
    server.sendHeader("Cache-Control", "no-store");
    server.send(ok ? 200 : 409, "application/json", body);
}

void handleRfTraceGet()
{
    selectLanguage();
    if (!requireAuth()) return;
    if (server.hasArg("status")) { sendRfTraceStatusJson(true, nullptr); return; }
    sendRfTraceFile(true);
}

void handleRfTracePost()
{
    selectLanguage();
    if (!requireAuth() || !requireSameOrigin()) return;
    String action = server.arg("action");
    bool ok = true;
    char err[160] = "";
    if (action == "start") {
        long minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 15;
        String f = server.arg("filter");
        RfTraceFilter filter = f == "problems" ? RF_TRACE_PROBLEMS : f == "id" ? RF_TRACE_ONE_ID : RF_TRACE_ALL;
        uint32_t id = (uint32_t)strtoul(server.arg("id").c_str(), nullptr, 16);
        if (minutes < 1 || minutes > (long)RF_TRACE_MAX_MIN) { ok = false; strlcpy(err, TF(T_tr_minutes_range, String((unsigned long)RF_TRACE_MAX_MIN)).c_str(), sizeof(err)); }
        else if (!gCfg.rfEn) { ok = false; strlcpy(err, T(T_st_wireless_sensors_are_off), sizeof(err)); }
        else if (filter == RF_TRACE_ONE_ID && !id) { ok = false; strlcpy(err, T(T_st_enter_the_sensor_id_hex), sizeof(err)); }
        else {
            ok = rfTraceStart((uint32_t)minutes * 60000UL, filter, id, false, err, sizeof(err));
            // rf_trace.cpp's reasons are English (trace file, log): shown in the page language
            String why = !strncmp(err, "not enough free memory", 22)
                       ? TF(T_tr_err_heap, String((unsigned)(rfTraceFreeHeap() / 1024)),
                            String((unsigned)((RF_TRACE_SIZE + RF_TRACE_HEAP_RESERVE) / 1024 + 2)))
                       : !strncmp(err, "memory allocation", 17) ? String(T(T_tr_err_alloc))
                       : !strncmp(err, "invalid", 7) ? String(T(T_tr_err_invalid)) : String(err);
            if (!ok) strlcpy(err, why.c_str(), sizeof(err));
        }
        strlcpy(rfTraceMessage, (ok ? TF(T_tr_started, String(minutes)) : TF(T_tr_not_started, err)).c_str(), sizeof(rfTraceMessage));
        dbgSerial.printf("Radio trace: %s\n", rfTraceMessage);
    } else if (action == "stop") {
        rfTraceStop("stopped by the user");
        strlcpy(rfTraceMessage, T(T_st_capture_stopped_download_the_file), sizeof(rfTraceMessage));
    } else if (action == "clear") {
        rfTraceClear();
        strlcpy(rfTraceMessage, T(T_st_capture_discarded_memory_freed), sizeof(rfTraceMessage));
    } else {
        ok = false;
        snprintf(err, sizeof(err), "action must be start, stop or clear");
    }
    if (server.hasArg("redirect")) {
        server.sendHeader("Location", "/api/log#rftrace");
        server.send(303, "text/plain", "see /api/log");
        return;
    }
    rfTraceMessage[0] = '\0';
    sendRfTraceStatusJson(ok, err);
}

// POST /api/rf/sensor: id=<hex>, action=mine|received|hide|up|down (status
// page buttons, scripts). Saved at once.
void handleRfSensorPost()
{
    selectLanguage();
    if (!requireWizardAuth() || !requireSameOrigin()) return;
    uint32_t id = (uint32_t)strtoul(server.arg("id").c_str(), nullptr, 16);
    String a = server.arg("action");
    bool ok = id != 0;
    if (ok) {
        if      (a == "mine")     ok = rfSetState(id, RF_STATE_MINE);
        else if (a == "received") ok = rfSetState(id, RF_STATE_RECEIVED);
        else if (a == "hide")     ok = rfSetState(id, RF_STATE_HIDDEN);
        else if (a == "up")       ok = rfMoveMine(id, -1);
        else if (a == "down")     ok = rfMoveMine(id, 1);
        else ok = false;
    }
    if (ok) saveConfig(gCfg);
    if (server.hasArg("redirect")) {
        server.sendHeader("Location", server.arg("back") == "setup" ? "/setup?step=2" : "/status");
        server.send(303, "text/plain", "see /status");
        return;
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"state\":\"%s\"}", ok ? "true" : "false",
             !id ? "" : rfStateOf(id) == RF_STATE_MINE ? "mine" : rfStateOf(id) == RF_STATE_HIDDEN ? "hidden" : "received");
    server.send(ok ? 200 : 409, "application/json", body);
}

void handleLogClear()
{
    selectLanguage();
    if (!requireAuth() || !requireSameOrigin()) return;
    dbgSerial.clear();
    server.send(200, "text/plain", "cleared");
}

// Rolling history JSON, served by handleApiHistory() below. history.cpp
// keeps its own two-tier in-RAM ring buffer (fine/24h + coarse/7-day); this
// only serves whichever tier ?range=7d selects (default: the fine tier).
void handleApiHistory()
{
    bool coarse = server.hasArg("range") && server.arg("range") == "7d";
    // static: see handleApiData()'s comment above — buildHistoryJson()
    // clears the content (not the buffer) at its own top (see buildHistoryJson()).
    static JsonDocument doc;
    buildHistoryJson(doc, coarse);
    // Full history (up to HISTORY_CAPACITY samples) is big — streamed
    // straight to the client via ChunkedPrint instead of holding the whole
    // serialized text in RAM at once, in a single serialize pass (see
    // ChunkedPrint's comment above).
    sendJsonDoc(doc, "history");
}


// The dashboard (its own five-language texts in chart_page.h) with the
// device's Language setting filled in at the marker @@L@@ ('' = Auto).
void handleChartPage()
{
    // first visits after a new start / factory reset: the setup wizard (once per browser session)
    if (wizardAutoStart(gCfg, server.header("Cookie").indexOf("wnSetup=1") >= 0)) {
        server.sendHeader("Location", "/setup");
        server.send(302);
        return;
    }
    // the device language for the page's script (the page itself is a fixed gzip file)
    if (gCfg.uiLang > 0 && gCfg.uiLang <= LANG_COUNT)
        server.sendHeader("Set-Cookie", String("wnDev=") + langCode((uint8_t)(gCfg.uiLang - 1)) + "; Path=/; SameSite=Lax");
    else
        server.sendHeader("Set-Cookie", "wnDev=; Path=/; Max-Age=0; SameSite=Lax");
    sendGzipAsset(server, CHART_PAGE_GZ, CHART_PAGE_GZ_LEN, CHART_PAGE_LEN, "text/html; charset=utf-8", "no-store");
}

// /chart itself moved to "/" (it's the page most visitors actually want) —
// redirect rather than 404 for anything that bookmarked/linked the old path.
void handleChartRedirect()
{
    server.sendHeader("Location", "/");
    server.send(302);
}

// ---------------------------------------------------------------------------
// Settings page: every setting except the WiFi network itself (SSID/
// password live in ESP-IDF's own storage and are changed via the config
// portal). The guided wind-direction calibration (/calibrate) and the
// decoder download (/decoder) are the portal's own pages (portal_pages.h),
// linked from here. Gated by the admin password (requireAuth).
//
// Two kinds of fields, marked per section:
//  - live:    applied immediately, no restart (operational parameters via
//             applyParam() — same validation as LoRaWAN downlink 0x10 / the
//             MQTT "set" command — plus calibration, weather networks, time,
//             admin password)
//  - restart: LoRaWAN keys/region, network/MQTT, PCB revision — only read at
//             boot, so the node saves and restarts (losing the in-RAM chart
//             history) when any of them actually changed
//
// Passwords/keys are never sent back to the browser: secret fields render
// empty, an empty submission keeps the stored value, and optional secrets
// get an explicit "remove" checkbox. wifiEn/webApiEn can only be switched
// off (System, advanced) with a confirmation and while LoRaWAN is joined
// (radioOffCheck(), net_check.h) - the page locks itself out.
// ---------------------------------------------------------------------------

// Same list as isSupportedRegion() (config.cpp), for the <select>.
const char* const LORA_REGIONS[] = { "EU868", "US915", "AU915", "AS923", "IN865", "KR920", "CN470", "RU864" };

// Set by handleSettingsPost() after a restart-requiring save; webApiTask
// restarts once the response has gone out.
volatile unsigned long restartAtMs = 0;
const char* volatile restartReason = "settings saved";   // for systemRestart()

// Settings fields rendered while advField is set get class 'adv': hidden
// until "Show advanced options" is ticked (CSS only, see PAGE_CSS).
bool advField = false;

// tag = false: an action section without settings (e.g. the firmware upload),
// so neither "live" nor "restart" applies.
void sectionStart(HtmlOut& html, const char* title, bool restart, const char* note = nullptr, bool adv = false,
                  bool tag = true)
{
    html += adv ? "<section class='adv'><h4>" : "<section><h4>"; html += title;
    if (tag) {
        html += restart ? "<span class='tag restart'>" : "<span class='tag live'>";
        html += T(restart ? T_c_tag_restart : T_c_tag_live);
        html += "</span>";
    }
    html += "</h4>";
    if (note) { html += "<p class='dim'>"; html += note; html += "</p>"; }
}

// Dependent fields: while gDep is set, the fields get data-dep and are shown
// only when the condition holds (script in pageStart(); without JS all stay
// visible). Syntax: "name=v1|v2", "name!=v" (v may be empty: "not empty"),
// conditions joined with "&", alternatives with ",". Hidden fields are still
// posted with their (unchanged) values - the POST handler works as before.
const char* gDep = nullptr;
struct DepScope {
    const char* prev;
    explicit DepScope(const char* d) : prev(gDep) { gDep = d; }
    ~DepScope() { gDep = prev; }
};

void labelStart(HtmlOut& html, const char* label, const char* hint)
{
    html += advField ? "<label class='adv'" : "<label";
    if (gDep) { html += " data-dep='"; html += gDep; html += "'"; }
    html += "><span>"; html += label;
    if (hint) { html += "<small>"; html += hint; html += "</small>"; }
    html += "</span>";
}

void fieldText(HtmlOut& html, const char* name, const char* label, const String& value,
               size_t maxLen, const char* hint = nullptr, const char* type = "text")
{
    capture(name, value);
    labelStart(html, label, hint);
    html += "<input type='"; html += type; html += "' name='"; html += name; formAttr(html); html += "' value='";
    appendEscaped(html, value.c_str());
    html += "' maxlength='"; html += (unsigned)maxLen; html += "'";
    if (!strcmp(type, "number")) html += " step='any'";
    html += "></label>";
}

void fieldCheck(HtmlOut& html, const char* name, const char* label, bool checked, const char* hint = nullptr)
{
    capture(name, checked ? "1" : "0");
    labelStart(html, label, hint);
    html += "<input type='checkbox' name='"; html += name; formAttr(html); html += "'";
    if (checked) html += " checked";
    // sent after the checkbox: arg() returns the first value ("on" if
    // checked), so a missing field means "not in this form", not "off"
    html += "><input type='hidden' name='"; html += name; formAttr(html); html += "' value='0'></label>";
}

struct Option { int value; const char* text; };

void fieldSelect(HtmlOut& html, const char* name, const char* label, int current,
                 const Option* opts, size_t count, const char* hint = nullptr)
{
    capture(name, String(current));
    labelStart(html, label, hint);
    html += "<select name='"; html += name; formAttr(html); html += "'>";
    for (size_t i = 0; i < count; i++) {
        html += "<option value='"; html += opts[i].value; html += "'";
        if (opts[i].value == current) html += " selected";
        html += ">"; html += opts[i].text; html += "</option>";
    }
    html += "</select></label>";
}

// The stored value of a write-only field - for a backup that includes secrets.
String secretValue(const char* name)
{
    static const struct { const char* name; const char* value; } secrets[] = {
        { "webadminpw", gCfg.webAdminPass }, { "mqttpass", gCfg.mqttPass }, { "awkpass", gCfg.awekasPass },
        { "wukey", gCfg.wuStationKey }, { "wckey", gCfg.wcloudKey }, { "wnpkey", gCfg.wnetproKey },
        { "windypw", gCfg.windyPass }, { "osemtok", gCfg.osemToken } };
    for (const auto& e : secrets) if (!strcmp(e.name, name)) return e.value;
    if (!strcmp(name, "appkey")) {
        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02X", gCfg.appKey[i]);
        return hex;
    }
    if (!strcmp(name, "wifipass")) return wifiCurrentPassword();
    if (!strcmp(name, "wowpin")) return gCfg.wowPin;
    String v;
    alarmsLock();
    const AlarmChannels& ch = alarmsChannels();
    if      (!strcmp(name, "nttok"))  v = ch.ntfyToken;
    else if (!strcmp(name, "potok"))  v = ch.poToken;
    else if (!strcmp(name, "pouser")) v = ch.poUser;
    else if (!strcmp(name, "tgtok"))  v = ch.tgToken;
    else if (!strcmp(name, "whurl"))  v = ch.whUrl;
    alarmsUnlock();
    if (v.length()) return v;
    if (!strncmp(name, "cu", 2) && name[2] >= '0' && name[2] < '0' + CUSTOM_UPLOADS && !strcmp(name + 3, "url")) {
        customUploadLock();
        v = customUploads()[name[2] - '0'].url;
        customUploadUnlock();
    }
    return v;
    return String();
}

void fieldSecret(HtmlOut& html, const char* name, const char* label, bool isSet, bool clearable,
                 size_t maxLen, const char* hint = nullptr)
{
    if (gCaptureSecrets && backupIsSecret(name)) { String v = secretValue(name); if (v.length()) capture(name, v); }
    labelStart(html, label, hint);
    html += "<span><input type='password' autocomplete='new-password' name='"; html += name; formAttr(html);
    html += "' maxlength='"; html += (unsigned)maxLen;
    html += "' placeholder='"; html += T(isSet ? T_set_pw_set : T_set_pw_not_set); html += "'>";
    if (clearable && isSet) {
        html += "<span class='clear'><input type='checkbox' name='"; html += name; html += "_clear";
        formAttr(html); html += "'>"; html += T(T_set_remove); html += "</span>";
    }
    html += "</span></label>";
}

void fieldReadonly(HtmlOut& html, const char* label, const String& value, const char* hint = nullptr)
{
    labelStart(html, label, hint);
    html += "<span class='ro'>"; html += value; html += "</span></label>";
}

String hexOf(uint64_t v)
{
    char buf[17];
    uint64ToHex(v, buf, sizeof(buf));
    return buf;
}

// ---- Wireless sensor table (settings, tab Sensors) ----

// "WS90", "Bresser type 1", "IT+" ...
String rfKindText(const RfSensorReading& r)
{
    if (r.model) return r.model;
    if (r.proto == RF_PROTO_BRESSER) return TF(T_set_bresser_type, String(r.type));
    return rfProtoName(r.proto);
}

// Short values for the table: "12.3 &deg;C &middot; 70 % &middot; ..."
String rfValuesShort(const RfSensorReading& r)
{
    String v;
    auto add = [&v](const String& t) { if (v.length()) v += " &middot; "; v += t; };
    if (r.hasTemp)      add(String(temperatureForOutput(r.tempC, gCfg), 1) + "&deg;" + temperatureUnitLabel(gCfg));
    if (r.hasHumidity)  add(String(r.humidity) + " %");
    if (r.hasWind)      add(TF(T_set_tv_wind, String(windSpeedForOutput(r.windAvgMs, gCfg), 1) + " " + windSpeedUnitLabel(gCfg)));
    RfStat* st = (r.hasRain || r.hasLightning) ? rfStatFor(r.id, false) : nullptr;   // today (rf_stats.h)
    uint32_t t = 0, ymd = 0;
    if (st) { rfStatsClock(t, ymd); rfStatRoll(*st, t, ymd); }
    if (r.hasRain && st && st->ymd) add(TF(T_st_rf_rain_today, String(rainForOutput(st->rainTodayMm, gCfg), 1) + " " + rainUnitLabel(gCfg)));
    else if (r.hasRain) add(TF(T_st_rain_total, String(rainForOutput(r.rainTotalMm, gCfg), 1) + " " + rainUnitLabel(gCfg)));
    if (r.hasPressure)  add(String(pressureForOutput(r.pressureHpa, gCfg), gCfg.unitSystem == 1 ? 2 : 1) + " " + pressureUnitLabel(gCfg));
    if (r.hasLight)     add(String((long)lroundf(r.lightLux)) + " lx");
    if (r.hasMoisture)  add(TF(T_set_tv_soil, String(r.moisturePct)));
    if (r.hasPm25)      add("PM2.5 " + String(r.pm25, 0));
    if (r.hasCo2)       add("CO2 " + String(r.co2Ppm));
    if (r.hasLeak)      add(r.leak ? String("<b>") + T(T_st_leak) + "</b>" : String(T(T_st_dry)));
    if (r.hasLightning && st && st->ymd) add(TF(T_st_strikes_today, String(st->strikesToday)));
    if (!r.batteryOk)   add(T(T_set_battery_low));
    return v;
}

void rfStateSelect(HtmlOut& html, const char* id, RfSensorState st)
{
    static const char* const vals[3]  = { "r", "m", "h" };
    const char* const texts[3] = { T(T_opt_9675_received), T(T_set_9733_mine), T(T_opt_10005_hidden) };
    html += "<select name='rfst_"; html += id; html += "'>";
    for (int k : { 1, 0, 2 }) {
        html += "<option value='"; html += vals[k]; html += "'";
        if (k == (int)st) html += " selected";
        html += ">"; html += texts[k]; html += "</option>";
    }
    html += "</select><input type='hidden' name='rfso_"; html += id; html += "' value='"; html += vals[st]; html += "'>";
}

void addRfSensorTable(HtmlOut& html)
{
    sectionStart(html, T(T_set_wireless_sensors), false,
                 T(T_set_9733_mine_numbered_first_in));
    html += "<table class='rft'><tr><th>#</th><th>"; html += T(T_set_th_state); html += "</th><th>ID</th><th>";
    html += T(T_set_th_name); html += "</th><th>";
    html += T(T_set_th_sensor); html += "</th><th class='hm'>"; html += T(T_set_th_signal); html += "</th><th class='hm'>";
    html += T(T_set_th_values); html += "</th><th></th></tr>";
    const std::vector<uint32_t> mine = rfParseIds(gCfg.rfIds);
    const unsigned long now = millis();
    bool any = false;
    auto row = [&](int slot, uint32_t id, int idx, RfSensorState st, size_t minePos) {
        any = true;
        char ids[12];
        snprintf(ids, sizeof(ids), "%08lX", (unsigned long)id);
        html += "<tr><td>";
        if (slot >= 0) html += String(slot + 1);
        html += "</td><td>";
        rfStateSelect(html, ids, st);
        html += "</td><td class='num'>"; html += ids;
        uint32_t eco;
        if (rfEcowittId(id, eco) && (idx < 0 || gRfSensors[idx].proto == RF_PROTO_FINEOFFSET)) {
            char e[8]; snprintf(e, sizeof(e), "%lX", (unsigned long)eco);
            html += "<br><small class='dim'>Ecowitt "; html += e; html += "</small>";
        }
        // user-defined name (rf_names.h)
        html += "</td><td><input name='rfnm_"; html += ids; html += "' maxlength='24' size='10' value='";
        appendEscaped(html, rfNameOf(id)); html += "'></td><td>";
        if (idx >= 0) {
            const RfSensorReading& r = gRfSensors[idx];
            html += rfKindText(r) + "</td><td class='hm'>" + String(r.rssi, 0) + " dBm &middot; " + agoText(r.lastMs) +
                    "</td><td class='hm'>" + rfValuesShort(r);
        } else {
            html += "<span class='dim'>"; html += T(T_set_not_received); html += "</span></td><td class='hm'></td><td class='hm'>";
        }
        html += "</td><td>";
        if (st == RF_STATE_MINE) {
            if (minePos > 0)
                { html += "<button class='sb' name='rfmove' value='"; html += ids; html += ":-1' title='"; html += T(T_set_up); html += "'>&uarr;</button>"; }
            if (minePos + 1 < mine.size())
                { html += "<button class='sb' name='rfmove' value='"; html += ids; html += ":1' title='"; html += T(T_set_down); html += "'>&darr;</button>"; }
        }
        html += "</td></tr>";
    };
    for (size_t k = 0; k < mine.size(); k++) {
        int idx = -1;
        for (int i = 0; i < RF_MAX_SENSORS; i++) if (gRfSensors[i].used && gRfSensors[i].id == mine[k]) idx = i;
        row((int)k, mine[k], idx, RF_STATE_MINE, k);
    }
    for (int slot = (int)mine.size(); slot < RF_MAX_SENSORS; slot++) {
        int idx = rfSensorIndexForSlot(slot);
        if (idx >= 0) row(slot, gRfSensors[idx].id, idx, RF_STATE_RECEIVED, 0);
    }
    for (int i = 0; i < RF_MAX_SENSORS; i++)   // beyond number 32 (no slot): still listed
        if (gRfSensors[i].used && rfSlotOfIndex(i) < 0 && rfStateOf(gRfSensors[i].id) != RF_STATE_MINE)
            row(-1, gRfSensors[i].id, i, RF_STATE_RECEIVED, 0);
    html += "</table>";
    if (!any) { html += "<p class='dim' id='rfnone'>"; html += T(T_set_no_sensor_yet); html += "</p>"; }
    if (rfOnlyMineActive()) { html += "<p class='dim'>"; html += T(T_set_only_mine_note); html += "</p>"; }

    int hiddenNear = 0;
    for (const RfHiddenSeen& h : gRfHidden)
        if (h.id && now - h.lastMs <= RF_HIDDEN_RANGE_MS && rfStateOf(h.id) == RF_STATE_HIDDEN) hiddenNear++;
    html += "<details><summary>" + TF(T_set_hidden_in_range, String(hiddenNear)) + "</summary>";
    if (hiddenNear) {
        html += "<table class='rft'>";
        for (const RfHiddenSeen& h : gRfHidden) {
            if (!h.id || now - h.lastMs > RF_HIDDEN_RANGE_MS || rfStateOf(h.id) != RF_STATE_HIDDEN) continue;
            char ids[12];
            snprintf(ids, sizeof(ids), "%08lX", (unsigned long)h.id);
            html += "<tr><td>";
            rfStateSelect(html, ids, RF_STATE_HIDDEN);
            html += "</td><td class='num'>"; html += ids; html += "</td><td>"; html += h.model ? h.model : "";
            html += "</td><td>" + String(h.rssi, 0) + " dBm &middot; " + agoText(h.lastMs) + "</td></tr>";
        }
        html += "</table><p class='dim'>"; html += T(T_set_unhide_note); html += "</p>";
    }
    html += "</details></section>";
}

// ---- Settings tab "Alarms" (alarms.h, docs/alarms.md) ----
// Field names: a<i>_<x> per rule slot, nt* / po* / tg* / wh* for the channels.
void renderAlarmsTab(HtmlOut& html)
{
    static const TextId srcText[AS_COUNT] = { T_al_src_off, T_al_src_temp, T_al_src_feels, T_al_src_hum, T_al_src_dew,
        T_al_src_qnh, T_al_src_ptend, T_al_src_wind, T_al_src_gust, T_al_src_rainrate, T_al_src_rainday, T_al_src_rain1h,
        T_al_src_uv, T_al_src_frost, T_al_src_fog, T_al_src_sultry, T_al_src_rf };
    static const TextId rfText[AF_COUNT] = { T_al_rf_temp, T_al_rf_hum, T_al_rf_wind, T_al_rf_gust, T_al_rf_uv, T_al_rf_lux,
        T_al_rf_leak, T_al_rf_lightning, T_al_rf_moisture, T_al_rf_co2, T_al_rf_pm25, T_al_rf_silent, T_al_rf_battery };
    static const char* const chName[AC_COUNT] = { "ntfy", "Pushover", "Telegram", "MQTT", "Webhook" };
    std::unique_ptr<AlarmRule[]> rules(new (std::nothrow) AlarmRule[ALARM_RULES]);
    std::unique_ptr<AlarmState[]> st(new (std::nothrow) AlarmState[ALARM_RULES]);
    if (!rules || !st) return;
    alarmsSnapshot(rules.get(), st.get());
    std::unique_ptr<AlarmChannels> chBuf(new (std::nothrow) AlarmChannels());
    if (!chBuf) return;
    alarmsLock();
    *chBuf = alarmsChannels();
    alarmsUnlock();
    const AlarmChannels& ch = *chBuf;

    sectionStart(html, T(T_al_rules), false, T(T_al_rules_note));
    Option srcOpts[AS_COUNT];
    for (int k = 0; k < AS_COUNT; k++) srcOpts[k] = { k, T(srcText[k]) };
    Option rfOpts[AF_COUNT];
    for (int k = 0; k < AF_COUNT; k++) rfOpts[k] = { k, T(rfText[k]) };
    const Option cmpOpts[] = { { 1, T(T_al_above) }, { 0, T(T_al_below) } };
    const Option prioOpts[] = { { 1, "1" }, { 2, "2" }, { 3, "3" }, { 4, "4" }, { 5, T(T_al_prio5) } };
    char n[16];
    for (int i = 0; i < ALARM_RULES; i++) {
        const AlarmRule& r = rules[i];
        const PhKind k = alarmKind(r);
        const char* unit = alarmUnitLabel(k, r.src, r.rfField, gCfg);
        html += "<details class='al'><summary>";
        html += String(i + 1); html += " &middot; ";
        if (r.src == AS_OFF) { html += "<span class='dim'>"; html += T(T_al_free); html += "</span>"; }
        else {
            appendEscaped(html, r.name[0] ? r.name : T(srcText[r.src]));
            html += st[i].active ? " &middot; <b class='warnt'>&#9888; " : " &middot; <span class='dim'>";
            html += T(st[i].active ? T_al_state_active : (r.enabled ? T_al_state_ok : T_al_state_off));
            html += st[i].active ? "</b>" : "</span>";
        }
        html += "</summary>";
        auto nm = [&n, i](const char* x) { snprintf(n, sizeof(n), "a%d_%s", i, x); return (const char*)n; };
        // dependent fields (DepScope): a free slot shows only "Monitor"; sensor fields only for
        // "Wireless sensor …"; thresholds not for yes/no values (warnings, leak, battery low)
        char dUsed[24], dRf[24], dManual[48], dNum[96];
        snprintf(dUsed, sizeof(dUsed), "a%d_src!=0", i);
        snprintf(dRf, sizeof(dRf), "a%d_src=%d", i, AS_RF);
        snprintf(dManual, sizeof(dManual), "a%d_src=%d&a%d_rfsel=", i, AS_RF, i);
        snprintf(dNum, sizeof(dNum), "a%d_src!=0|%d|%d|%d|%d,a%d_src=%d&a%d_rff!=%d|%d", i, AS_FROST, AS_FOG, AS_SULTRY, AS_RF,
                 i, AS_RF, i, AF_LEAK, AF_BATTERY_LOW);
        fieldCheck (html, nm("en"),   T(T_al_enabled), r.enabled);
        fieldSelect(html, nm("src"),  T(T_al_value), r.src, srcOpts, AS_COUNT);
        DepScope usedDep(dUsed);
        fieldText  (html, nm("name"), T(T_al_name), r.name, sizeof(r.name) - 1);
        char id[12] = "";
        if (r.rfId) snprintf(id, sizeof(id), "%08lX", (unsigned long)r.rfId);
        {   // known sensors (★ first, then the received ones, plus the rule's own) - or a manual ID
            DepScope rfDep(dRf);
            labelStart(html, T(T_al_sensor_id), nullptr);
            html += "<select name='"; html += nm("rfsel"); formAttr(html); html += "' class='alsel' data-rule='"; html += i; html += "'>";
            html += "<option value=''>"; html += T(T_al_sensor_manual); html += "</option>";
            auto opt = [&](uint32_t sid, const RfSensorReading* sr) {
                char hex[12];
                snprintf(hex, sizeof(hex), "%08lX", (unsigned long)sid);
                html += "<option value='"; html += hex; html += "'";
                if (sid == r.rfId) html += " selected";
                html += ">";
                const char* nmv = rfNameOf(sid);
                if (nmv[0]) appendEscaped(html, nmv); else html += sr ? rfKindText(*sr) : String(hex);
                uint32_t eco;
                html += " &middot; ";
                if (sr && sr->proto == RF_PROTO_FINEOFFSET && rfEcowittId(sid, eco)) { char e[8]; snprintf(e, sizeof(e), "%lX", (unsigned long)eco); html += e; }
                else html += hex;
                html += "</option>";
            };
            std::vector<uint32_t> shown = rfParseIds(gCfg.rfIds);
            for (uint32_t sid : shown) {
                const RfSensorReading* sr = nullptr;
                for (const RfSensorReading& x : gRfSensors) if (x.used && x.id == sid) sr = &x;
                opt(sid, sr);
            }
            for (const RfSensorReading& x : gRfSensors) {
                if (!x.used) continue;
                bool dup = false;
                for (uint32_t sid : shown) if (sid == x.id) dup = true;
                if (!dup) { opt(x.id, &x); shown.push_back(x.id); }
            }
            bool ruleListed = !r.rfId;
            for (uint32_t sid : shown) if (sid == r.rfId) ruleListed = true;
            if (!ruleListed) opt(r.rfId, nullptr);   // not in range now
            html += "</select></label>";
        }
        {   // manual hex ID (not in range now)
            DepScope manDep(dManual);
            fieldText  (html, nm("rfid"), T(T_al_sensor_id_manual), id, 8, T(T_al_sensor_id_hint));
        }
        {
            DepScope rfDep(dRf);
            fieldSelect(html, nm("rff"),  T(T_al_sensor_value), r.rfField, rfOpts, AF_COUNT);
        }
        fieldSelect(html, nm("cmp"),  T(T_al_cmp), r.above, cmpOpts, 2);
        const String u = unit[0] ? String(" (") + unit + ")" : String();
        {
            DepScope numDep(dNum);
            fieldText  (html, nm("raise"), (String(T(T_al_raise)) + u).c_str(), String(alarmToDisplay(k, r.raise, gCfg), 2), 10, nullptr, "number");
            fieldText  (html, nm("clear"), (String(T(T_al_clear)) + u).c_str(), String(alarmToDisplay(k, r.clear, gCfg), 2), 10, T(T_al_clear_hint), "number");
        }
        fieldText  (html, nm("dur"),  T(T_al_dur), String(r.minDurationMin), 5, nullptr, "number");
        fieldText  (html, nm("rep"),  T(T_al_rep), String(r.repeatMin), 5, T(T_al_rep_hint), "number");
        fieldSelect(html, nm("prio"), T(T_al_prio), r.priority, prioOpts, 5);
        for (int c = 0; c < AC_COUNT; c++) {
            char cn[8];
            snprintf(cn, sizeof(cn), "ch%d", c);
            fieldCheck(html, nm(cn), (String(T(T_al_channel)) + ": " + chName[c]).c_str(), r.channels & (1 << c));
        }
        fieldCheck (html, nm("clr"),  T(T_al_allclear), r.allClear);
        fieldText  (html, nm("msg"),  T(T_al_msg), r.msg, sizeof(r.msg) - 1, T(T_al_msg_hint));
        fieldText  (html, nm("cmsg"), T(T_al_cmsg), r.clearMsg, sizeof(r.clearMsg) - 1);
        html += "</details>";
    }
    {   // which measurements each known sensor provides (filters the "Measurement" list; silent / battery: always)
        html += "<script>var ALCAP={";
        bool first = true;
        for (const RfSensorReading& x : gRfSensors) {
            if (!x.used) continue;
            char hex[12];
            snprintf(hex, sizeof(hex), "%08lX", (unsigned long)x.id);
            html += first ? "'" : ",'"; first = false;
            html += hex; html += "':[";
            const bool has[AF_COUNT] = { x.hasTemp, x.hasHumidity, x.hasWind, x.hasWind, x.hasUv, x.hasLight, x.hasLeak, x.hasLightning,
                                         x.hasMoisture, x.hasCo2, x.hasPm25, true, true };
            bool f2 = true;
            for (int k = 0; k < AF_COUNT; k++) if (has[k]) { if (!f2) html += ","; html += k; f2 = false; }
            html += "]";
        }
        html += "};document.addEventListener('DOMContentLoaded',function(){document.querySelectorAll('select.alsel').forEach(function(s){"
                "var f=document.querySelector('[name=\"a'+s.getAttribute('data-rule')+'_rff\"]');"
                "function upd(){var c=ALCAP[s.value];if(!f)return;for(var o of f.options)o.hidden=!!c&&c.indexOf(+o.value)<0;}"
                "s.addEventListener('change',upd);upd();});});</script>";
    }
    const Option tplOpts[] = { { -1, "&ndash;" }, { 0, T(T_al_src_frost) }, { 1, T(T_al_src_fog) }, { 2, T(T_al_src_sultry) },
                               { 3, T(T_al_tpl_storm) }, { 4, T(T_al_tpl_rain) } };
    fieldSelect(html, "altpl", T(T_al_tpl), -1, tplOpts, 6, T(T_al_tpl_hint));
    html += "</section>";

    sectionStart(html, T(T_al_channels), false, T(T_al_channels_note));
    auto testBtn = [&html](int c) {
        html += "<p><button class='sb' form='aform' formaction='/api/alarms/test' name='testch' value='"; html += c; html += "'>";
        html += T(T_al_test); html += "</button></p>";
    };
    fieldText  (html, "nturl",   T(T_al_ntfy_url), ch.ntfyUrl, sizeof(ch.ntfyUrl) - 1);
    fieldText  (html, "nttopic", T(T_al_ntfy_topic), ch.ntfyTopic, sizeof(ch.ntfyTopic) - 1);
    fieldSecret(html, "nttok",   T(T_al_ntfy_token), ch.ntfyToken[0], true, sizeof(ch.ntfyToken) - 1, "optional");
    fieldText  (html, "nttags",  T(T_al_ntfy_tags), ch.ntfyTags, sizeof(ch.ntfyTags) - 1);
    testBtn(AC_NTFY);
    fieldSecret(html, "potok",   T(T_al_po_token), ch.poToken[0], true, sizeof(ch.poToken) - 1);
    fieldSecret(html, "pouser",  T(T_al_po_user), ch.poUser[0], true, sizeof(ch.poUser) - 1);
    fieldText  (html, "poretry", T(T_al_po_retry), String(ch.poRetrySec), 5, nullptr, "number");
    fieldText  (html, "poexp",   T(T_al_po_expire), String(ch.poExpireSec), 5, nullptr, "number");
    testBtn(AC_PUSHOVER);
    fieldSecret(html, "tgtok",   T(T_al_tg_token), ch.tgToken[0], true, sizeof(ch.tgToken) - 1);
    fieldText  (html, "tgchat",  T(T_al_tg_chat), ch.tgChat, sizeof(ch.tgChat) - 1);
    testBtn(AC_TELEGRAM);
    fieldCheck (html, "almqtt",  T(T_al_mqtt), ch.mqttEn, T(T_al_mqtt_hint));
    testBtn(AC_MQTT);
    const Option methods[] = { { 0, "GET" }, { 1, "POST" } };
    fieldText  (html, "whurl",   T(T_al_wh_url), ch.whUrl, sizeof(ch.whUrl) - 1, T(T_al_wh_url_hint));
    fieldSelect(html, "whpost",  T(T_al_wh_method), ch.whPost, methods, 2);
    { DepScope dep("whpost=1"); fieldText(html, "whbody", T(T_al_wh_body), ch.whBody, sizeof(ch.whBody) - 1, T(T_al_wh_body_hint)); }
    testBtn(AC_WEBHOOK);
    html += "</section>";
}

// Custom uploads (custom_upload.h), in the Outputs tab
void renderCustomUploads(HtmlOut& html)
{
    std::unique_ptr<CustomUpload[]> u(new (std::nothrow) CustomUpload[CUSTOM_UPLOADS]);
    if (!u) return;
    customUploadLock();
    for (int i = 0; i < CUSTOM_UPLOADS; i++) u[i] = customUploads()[i];
    customUploadUnlock();
    sectionStart(html, T(T_cu_title), false, T(T_cu_note));
    const Option methods[] = { { 0, "GET" }, { 1, "POST" } };
    char n[12];
    for (int i = 0; i < CUSTOM_UPLOADS; i++) {
        auto nm = [&n, i](const char* x) { snprintf(n, sizeof(n), "cu%d%s", i, x); return (const char*)n; };
        fieldText  (html, nm("url"),  TF(T_cu_url, String(i + 1)).c_str(), u[i].url, sizeof(u[i].url) - 1, T(T_cu_url_hint));
        fieldSelect(html, nm("post"), T(T_al_wh_method), u[i].post, methods, 2);
        char dPost[16];
        snprintf(dPost, sizeof(dPost), "cu%dpost=1", i);
        { DepScope dep(dPost); fieldText(html, nm("body"), T(T_cu_body), u[i].body, sizeof(u[i].body) - 1, T(T_al_wh_body_hint)); }
        fieldText  (html, nm("int"),  T(T_cu_int), String(u[i].intervalSec), 4, "60&ndash;3600", "number");
    }
    html += "</section>";
}

String gRestoreReport;   // what a settings restore changed: shown once on the next settings page

void renderSettings(const String& message, bool isError, int httpCode)
{
    const Option unitSystems[] = { { 0, T(T_opt_metric_c_mm_hpa) }, { 1, T(T_opt_imperial_f_in_inhg_mph) } };
    const Option windUnits[]  = { { 1, "m/s" }, { 0, "km/h" } };
    const Option windSamples[] = { { 1, "1 s" }, { 3, T(T_opt_3_s_less_wake_ups) } };
    const Option windAggs[]   = { { WIND_AGG_INTERVAL, T(T_opt_interval_since_the_last_send) },
                                         { WIND_AGG_10MIN, T(T_opt_10_min_wmo_ready_to) } };
    const Option cpuFreqs[]   = { { 80, T(T_opt_80_mhz_low_power) }, { 240, "240 MHz" } };
    const Option dirTypes[]   = { { 0, T(T_opt_potentiometer_davis) }, { 1, T(T_opt_calibrated_table_reed_resistor_vane) } };
    const Option payFmts[]    = { { 1, T(T_opt_compact_binary_fport_3_recommended) }, { 0, T(T_opt_cayennelpp_fport_1_about_twice) } };
    const Option tlsModes[]   = { { MQTT_TLS_NONE, T(T_opt_plain_no_tls) }, { MQTT_TLS_INSECURE, T(T_opt_tls_trust_any_certificate) },
                                         { MQTT_TLS_VERIFY, T(T_opt_tls_verify_against_ca_below) } };
    const Option boards[]     = { { BOARD_SEL_AUTO, T(T_opt_auto_by_flash_size_8) },
                                         { BOARD_V3, "Heltec WiFi LoRa 32 V3" }, { BOARD_V4, T(T_opt_heltec_wifi_lora_32_v4) } };
    const Option pcbRevs[]    = { { PCB_REV_1_2, "Rev 1.2" }, { PCB_REV_1_3, "Rev 1.3" }, { PCB_REV_2_0, "v2.0" } };
    const Option heatModes[]  = { { 0, T(T_opt_off) }, { 1, T(T_opt_on) }, { 2, T(T_opt_auto_below_the_threshold) } };
    const Option fanModes[]   = { { 0, T(T_opt_off) }, { 1, T(T_opt_on) }, { 2, T(T_opt_auto_above_the_threshold) } };
    const Option wind2Modes[] = { { 0, T(T_opt_off_aux_inputs_free) }, { 1, T(T_opt_speed_anemometer_at_aux_pulse) },
                                         { 2, T(T_opt_speed_direction_vane_at_aux) } };
    #define OPTS(a) a, sizeof(a) / sizeof(a[0])

    // One contiguous ~26 KB block. If the heap can't provide it (or a later
    // grow fails), String silently drops the appends - answer 503 instead of
    // serving a truncated form that would save half the settings.
    HtmlOut html(server, httpCode);
    pageStart(html, T(T_c_settings), "/api/settings");
    html += "<p class='sub'>"; html += T(T_set_intro); html += "</p>";
    if (message.length()) {
        html += "<div class='banner "; html += isError ? "badb" : "okb"; html += "'>"; html += message; html += "</div>";
    }
    if (gRestoreReport.length() && gCapture.isNull()) {   // once, after a restore (handleSettingsRestore)
        html += "<div class='banner okb'>"; html += gRestoreReport; html += "</div>";
        gRestoreReport = "";
    }
    if (wifiChangeBusy()) {
        html += "<div class='banner okb'>"; html += T(T_set_testing_wifi); html += "</div>";
    } else {
        const WifiChangeOutcome wr = wifiChangeResult();
        if (wr.kind != WIFI_CHG_NONE) {
            String ssid, extra;
            appendEscaped(ssid, wr.ssid);
            appendEscaped(extra, wr.extra);
            static const TextId texts[] = { T_set_wifi_chg_stored, T_set_wifi_chg_aborted, T_set_wifi_chg_switched,
                                            T_set_wifi_chg_unreachable, T_set_wifi_chg_no_net };
            const bool bad = wr.kind == WIFI_CHG_ABORTED || wr.kind >= WIFI_CHG_FAIL_UNREACHABLE;
            html += "<div class='banner "; html += bad ? "badb" : "okb"; html += "'>";
            html += TF(texts[wr.kind - 1], ssid, extra); html += "</div>";
        }
    }
    // Tabs: one form (POST /api/settings only changes the fields it gets, all
    // tabs are posted together by Save); without JavaScript every tab is shown.
    // "Show advanced options": a CSS checkbox (works without JS), remembered in
    // localStorage by the script at the end.
    html += "<input type='checkbox' id='advtgl' class='advtgl'><label class='advlbl' for='advtgl'><span>";
    html += T(T_set_show_advanced); html += "<small>"; html += T(T_set_advanced_hint); html += "</small></span></label>";
    html += "<div class='tabs'>";
    const struct { const char* id; TextId t; } tabs[] = {   // hook: a new tab = one entry here and in the script ids
        { "basics", T_set_tab_basics }, { "sensors", T_set_tab_sensors }, { "outputs", T_set_tab_outputs }, { "alarms", T_set_tab_alarms },
        { "display", T_set_tab_display }, { "system", T_set_tab_system } };
    for (auto& t : tabs) { html += "<a href='#"; html += t.id; html += "'>"; html += T(t.t); html += "</a>"; }
    html += "</div>";
    html += "<form method='POST' action='/api/settings' id='sform'><input type='hidden' name='form' value='1'>"
            // the default button (Enter in a text field): Save, not the first arrow of the sensor table
            "<button type='submit' tabindex='-1' aria-hidden='true' style='position:absolute;left:-9999px'>";
    html += T(T_set_save); html += "</button>";

    // ======== Basics ========
    html += "<div class='tab' id='basics'>";
    sectionStart(html, T(T_set_language), false);
    {
        const Option langs[] = { { 0, T(T_set_lang_auto) }, { 1, "English" }, { 2, "Deutsch" }, { 3, "Fran&ccedil;ais" },
                                 { 4, "Espa&ntilde;ol" }, { 5, "Portugu&ecirc;s" } };
        fieldSelect(html, "uilang", T(T_set_language), gCfg.uiLang, langs, sizeof(langs) / sizeof(langs[0]), T(T_set_language_hint));
    }
    html += "</section>";

    sectionStart(html, T(T_set_device), true);
    fieldText  (html, "devname",    T(T_set_device_name), gCfg.deviceName, sizeof(gCfg.deviceName) - 1, T(T_set_wi_fi_mdns_hostname_sanitised));
    html += "</section>";

    sectionStart(html, T(T_set_location), false, T(T_set_altitude_for_the_sea_level));
    fieldText  (html, "altitude",  T(T_set_altitude_above_sea_level_m), String(gCfg.altitudeM), 5, T(T_set_0_4000_for_sea_level), "number");
    wxUploadLock();
    fieldText  (html, "lat", T(T_set_latitude), gCfg.latitude, sizeof(gCfg.latitude) - 1, T(T_set_decimal_degrees_e_g_47));
    fieldText  (html, "lon", T(T_set_longitude), gCfg.longitude, sizeof(gCfg.longitude) - 1, T(T_set_decimal_degrees_e_g_11));
    {
        double la, lo;
        if (!astroLocation(la, lo)) fieldReadonly(html, T(T_set_astro), T(T_set_astro_needs_loc));
    }
    html += "</section>";
    sectionStart(html, T(T_set_time), false);
    fieldText(html, "tz",  T(T_set_time_zone_posix_tz), gCfg.timeZone, sizeof(gCfg.timeZone) - 1,
              T(T_set_central_europe_cet_1cest_m3));
    fieldText(html, "ntp", T(T_set_ntp_server), gCfg.ntpServer, sizeof(gCfg.ntpServer) - 1);
    html += "</section>";
    wxUploadUnlock();

    sectionStart(html, T(T_set_units), false, T(T_set_device_wide_also_affects_mqtt));
    fieldSelect(html, "unitsys",  T(T_set_unit_system), gCfg.unitSystem, OPTS(unitSystems));
    fieldSelect(html, "windunit", T(T_set_wind_speed_unit_metric_only), gCfg.windUnitMs, OPTS(windUnits));
    html += "</section>";


    sectionStart(html, T(T_set_web_access), false);
    fieldSecret  (html, "webadminpw", T(T_set_admin_password), true, false, sizeof(gCfg.webAdminPass) - 1, T(T_set_user_name_admin));
    html += "</section>";

    html += "</div>";

    // ======== Sensors ========
    html += "<div class='tab' id='sensors'>";
    sectionStart(html, T(T_set_wireless_receiver), false);
    fieldCheck (html, "rfen",      T(T_set_wireless_sensors_868_mhz), gCfg.rfEn,
                T(T_set_bresser_tfa_lacrosse_it_or));
    {
        // the last entry shows a bit mask set by downlink / MQTT that has no own
        // entry, so saving the form keeps it
        static char customText[48];
        static Option rfProtos[RF_TYPE_COUNT + 1];
        for (size_t i = 0; i < RF_TYPE_COUNT; i++) rfProtos[i] = { RF_TYPES[i].value, T(RF_TYPES[i].text) };
        int cur = gCfg.rfProto;
        bool listed = false;
        for (size_t i = 0; i < RF_TYPE_COUNT; i++) listed |= rfProtos[i].value == cur;
        size_t count = RF_TYPE_COUNT;
        if (!listed && rfProtoValid(cur)) {
            strlcpy(customText, TF(T_set_other_combination, String(cur)).c_str(), sizeof(customText));
            rfProtos[RF_TYPE_COUNT] = { cur, customText };
            count++;
        } else if (!listed) {
            cur = RF_PROTO_AUTO;
        }
        fieldSelect(html, "rfproto", T(T_set_wireless_sensor_type), cur, rfProtos, count,
                    T(T_set_auto_listens_to_bresser_it));
    }
    fieldCheck (html, "rfonly", T(T_set_only_my_sensors), gCfg.rfOnlyMine,
                T(T_set_on_only_the_sensors_marked));
    advField = true;
    fieldText  (html, "rfids",    T(T_set_mine_ids_as_text), gCfg.rfIds, sizeof(gCfg.rfIds) - 1,
                T(T_set_hex_comma_separated_max_32));
    fieldText  (html, "rfignore", T(T_set_hidden_ids_as_text), gCfg.rfIgnore, sizeof(gCfg.rfIgnore) - 1,
                T(T_set_hex_comma_separated_max_322));
    fieldText  (html, "rfnames",  T(T_set_names_as_text), gCfg.rfNames, sizeof(gCfg.rfNames) - 1, T(T_set_names_as_text_hint));
    advField = false;
    html += "</section>";
    addRfSensorTable(html);
    // Live table: every 5 s new sensors are appended and the signal / values
    // of the listed ones refreshed (GET /api/settings/rfsensors). Cells with
    // a field are never replaced, so a name being typed or a changed state
    // stays as it is; sensors that went silent stay until the page is reloaded.
    // Only while the Sensors tab is shown and no firmware upload runs: the
    // WebServer keeps ONE digest nonce, and a request that gets challenged
    // renews it - a firmware upload in flight then fails its auth, the
    // browser sends it again and finally asks for the password.
    html += "<script>(function(){var T=document.querySelector('table.rft');if(!T||!window.fetch||!window.DOMParser)return;"
            "function k(tr){var i=tr.querySelector('input[name^=\"rfnm_\"]');return i?i.name:null;}"
            "function upd(){if(document.hidden||location.hash!='#sensors'||window.wnBusy)return;fetch('/api/settings/rfsensors',{cache:'no-store'})"
            ".then(function(r){return r.ok?r.text():null;}).then(function(h){if(!h)return;"
            "var N=new DOMParser().parseFromString(h,'text/html').querySelector('table.rft');if(!N)return;"
            "var o={};Array.prototype.forEach.call(T.rows,function(tr){var x=k(tr);if(x)o[x]=tr;});"
            "Array.prototype.forEach.call(N.rows,function(tr){var x=k(tr);if(!x)return;var e=o[x];"
            "if(!e){(T.tBodies[0]||T).appendChild(document.importNode(tr,true));var n=document.getElementById('rfnone');if(n)n.remove();return;}"
            "for(var c=0;c<tr.cells.length&&c<e.cells.length;c++)if(!e.cells[c].querySelector('input,select,button'))e.cells[c].innerHTML=tr.cells[c].innerHTML;"
            "});}).catch(function(){});}"
            "setInterval(upd,5000);})();</script>";
    sectionStart(html, T(T_set_value_sources), false, T(T_set_value_sources_note));
    {
        // value sources (sources.h): one select per group; the wireless sensors that
        // exist (received or ★, up to 32) with their name and ID - an empty number
        // only while it is chosen
        std::vector<uint32_t> ids = rfParseIds(gCfg.rfIds);
        std::vector<String> rfLabel;
        std::vector<Option> srcOpts;
        rfLabel.reserve(RF_MAX_SENSORS);
        srcOpts.reserve(RF_MAX_SENSORS + 3);
        srcOpts.push_back({ SRC_AUTO, T(T_opt_auto_board_first_then_wireless) });
        srcOpts.push_back({ SRC_BOARD, T(T_opt_board_sensor) });
        for (int k = 0; k < RF_MAX_SENSORS; k++) {
            const int idx = rfSensorIndexForSlot(k);
            bool chosen = false;
            for (int g = 0; g < SG_COUNT; g++) chosen |= srcSelSlot(gCfg.srcSel[g]) == k;
            // "Garten · 900181A2" with a user-defined name (rf_names.h), else the ID
            auto who = [](uint32_t id) {
                char h[12]; snprintf(h, sizeof(h), "%08lX", (unsigned long)id);
                String w;
                const char* nm = rfNameOf(id);
                if (nm && *nm) { appendEscaped(w, nm); w += " &middot; "; }
                return w + h;
            };
            if (idx >= 0) {
                rfLabel.push_back(TF(T_set_wireless_slot, String(k + 1), who(gRfSensors[idx].id)));
            } else if (k < (int)ids.size()) {
                rfLabel.push_back(TF(T_set_wireless_slot_not_received, String(k + 1), who(ids[k])));
            } else if (chosen) {
                rfLabel.push_back(TF(T_set_wireless_slot_empty, String(k + 1)));
            } else {
                continue;
            }
            srcOpts.push_back({ srcSelForSlot(k), rfLabel.back().c_str() });
        }
        srcOpts.push_back({ SRC_OFF, T(T_opt_off_not_reported) });
        static const char* const names[SG_COUNT]  = { "srcair", "srcwind", "srcrain", "srcuv", "srclight", "srcpress" };
        const char* const labels[SG_COUNT] = { T(T_opt_source_temperature_humidity), T(T_set_source_wind),
                                                      T(T_set_source_rain), T(T_set_source_uv), T(T_set_source_light), T(T_opt_source_pressure) };
        for (int g = 0; g < SG_COUNT; g++)
            fieldSelect(html, names[g], labels[g], gCfg.srcSel[g], srcOpts.data(), srcOpts.size(),
                        g == SG_AIR   ? T(T_set_used_for_lorawan_mqtt_weather) :
                        g == SG_PRESS ? T(T_set_station_pressure_bme280_or_a) :
                        g == SG_LIGHT ? T(T_set_only_wireless_sensors_e_g) :
                        g == SG_RAIN  ? T(T_set_switching_keeps_the_rain_totals) : nullptr);
    }
    html += "</section>";

    sectionStart(html, T(T_set_measurement), false);
    fieldText  (html, "uplinksec", T(T_set_lorawan_measurement_interval_s), String(gCfg.uplinkIntervalSec), 5,
                T(T_set_interval_hint), "number");
    fieldSelect(html, "samplesec", T(T_set_wind_sample_interval), windSampleSecNormalized(gCfg.sampleSec), OPTS(windSamples),
                T(T_set_gust_highest_3_s_mean));
    if (boardPcbAllowed()) {
        fieldCheck (html, "winden",    T(T_set_wind_sensor_board), gCfg.windEn, T(T_set_davis_anemometer_vane_on_the));
        fieldCheck (html, "rainen",    T(T_set_rain_sensor_board), gCfg.rainEn, T(T_set_davis_rain_gauge_on_the));
    } else {
        fieldReadonly(html, T(T_set_wind_rain_sensor_board), T(T_set_not_available_on_heltec_v4),
                      T(T_set_wireless_build_only_pcb_inputs));
    }
    fieldCheck (html, "rainpersist", T(T_set_keep_rain_statistics_over_a), gCfg.rainPersist,
                T(T_set_saves_rain_sunshine_today_and));
    fieldCheck (html, "lipoen",    T(T_set_lipo_battery_measurement), gCfg.lipoEn);
    {   // sunshine duration (astro.h)
        const Option sunOpts[] = { { 0, T(T_set_sun_lux) }, { 1, T(T_set_sun_radiation) } };
        fieldSelect(html, "sunmethod", T(T_set_sun_method), gCfg.sunMethod, OPTS(sunOpts), T(T_set_sun_method_hint));
    }
    html += "</section>";

    sectionStart(html, T(T_set_sensor_calibration), false, T(T_set_defaults_match_the_davis_6410));
    fieldText  (html, "rainmmtip",  T(T_set_rain_per_tip_mm), String(gCfg.rainMmPerTip, 4), 10, T(T_set_davis_0_2), "number");
    fieldText  (html, "windmspp",   T(T_set_wind_speed_per_pulse_s), String(gCfg.windMsPerPps, 5), 10, T(T_set_davis_1_00584), "number");
    fieldText  (html, "windoff",    T(T_set_wind_direction_offset), String(gCfg.windDirOffsetDeg), 4, T(T_set_180_to_180_added_to), "number");
    // main values (calibration.h): after the value source, board and wireless alike
    fieldText  (html, "caltemp",    T(T_set_cal_temp), String(gCfg.calTempDc / 10.0f, 1), 6, T(T_set_cal_temp_hint), "number");
    fieldText  (html, "calhum",     T(T_set_cal_hum), String(gCfg.calHumDpct / 10.0f, 1), 6, T(T_set_cal_hum_hint), "number");
    fieldText  (html, "calpress",   T(T_set_cal_press), String(gCfg.calPressDhpa / 10.0f, 1), 6, T(T_set_cal_press_hint), "number");
    fieldText  (html, "calrain",    T(T_set_cal_rain), String(gCfg.calRainPermil / 1000.0f, 3), 6, T(T_set_cal_factor_hint), "number");
    fieldText  (html, "calwind",    T(T_set_cal_wind), String(gCfg.calWindPermil / 1000.0f, 3), 6, T(T_set_cal_factor_hint), "number");
    {
        constexpr size_t RFC_LEN = 2400;   // the text form with gains is longer than the stored one; heap while rendering
        std::unique_ptr<char[]> rfc(new (std::nothrow) char[RFC_LEN]);
        if (rfc) {
            rfCalFormatText(gCfg.rfCal, rfc.get(), RFC_LEN);
            fieldText(html, "rfcal", T(T_set_cal_rf), rfc.get(), RFC_LEN - 1, T(T_set_cal_rf_hint));
        }
    }
    fieldSelect(html, "winddirtyp", T(T_set_wind_direction_sensor), gCfg.windDirSensorType, OPTS(dirTypes));
    {   // the portal's guided wizard (portal_pages.h): only with a vane input
        String v = gCfg.windDirCalCount ? TF(T_cal_n_positions, String(gCfg.windDirCalCount)) : String(T(T_set_not_calibrated));
        if (boardPcbAllowed()) { v += " &middot; <a href='/calibrate'>"; v += T(T_set_cal_wizard_open); v += "</a>"; }
        fieldReadonly(html, T(T_set_calibration_table), v, T(T_set_cal_wizard_hint));
    }
    html += "</section>";

    if (ioV2()) {
        sectionStart(html, T(T_set_inputs_outputs_pcb_v2_0), false,
                     T(T_set_heater_max_3_a_and));
        fieldCheck (html, "auxainen",    T(T_set_aux_analog_input_gpio4), gCfg.auxAinEn);
        advField = true;
        fieldText  (html, "auxainscale", T(T_set_aux_analog_scale), String(gCfg.auxAinScale, 3), 8,
                    T(T_set_v_at_terminal_per_v), "number");
        advField = false;
        fieldCheck (html, "auxpulseen",  T(T_set_aux_pulse_counter_gpio48), gCfg.auxPulseEn);
        fieldSelect(html, "wind2",       T(T_set_2nd_wind_sensor_aux), gCfg.wind2Mode, OPTS(wind2Modes),
                    T(T_set_uses_the_aux_pulse_and));
        fieldText  (html, "wind2off",    T(T_set_2nd_wind_vane_offset), String(gCfg.wind2DirOffsetDeg), 4, T(T_set_180_to_180), "number");
        fieldSelect(html, "heatmode",    T(T_set_heater_j12), gCfg.heaterMode, OPTS(heatModes));
        fieldText  (html, "heatduty",    T(T_set_heater_power), String(gCfg.heaterDutyPct), 3, T(T_set_1_100_10_s_heating), "number");
        fieldText  (html, "heatbelow",   T(T_set_heater_auto_on_below_c), String(gCfg.heaterOnBelowDc / 10.0f, 1), 6, T(T_set_off_again_1_k_above), "number");
        fieldText  (html, "heatminduty", T(T_set_heater_power_at), String(gCfg.heaterMinDutyPct), 3, T(T_set_rises_linearly_towards_the_full), "number");
        fieldText  (html, "heatfull",    T(T_set_heater_auto_full_power_at), String(gCfg.heaterFullBelowDc / 10.0f, 1), 6, T(T_set_and_colder), "number");
        fieldSelect(html, "fanmode",     T(T_set_fan_j13), gCfg.fanMode, OPTS(fanModes));
        fieldText  (html, "fanduty",     T(T_set_fan_speed), String(gCfg.fanDutyPct), 3, T(T_set_1_100_pwm_tacho_and), "number");
        fieldText  (html, "fanabove",    T(T_set_fan_auto_on_from_c), String(gCfg.fanOnAboveDc / 10.0f, 1), 6, T(T_set_off_again_1_k_below), "number");
        fieldText  (html, "fanminpv",    T(T_set_fan_auto_minimum_pv_power), String(gCfg.fanMinPvW), 5, T(T_set_0_ignore_otherwise_missing_disabled), "number");
        fieldCheck (html, "tachoen",     T(T_set_fan_tacho_gpio40), gCfg.tachoEn);
        advField = true;
        fieldText  (html, "tachoppr",    T(T_set_tacho_pulses_per_revolution), String(gCfg.tachoPpr), 1, T(T_set_pc_fans_2), "number");
        advField = false;
        fieldCheck (html, "vedirecten",  T(T_set_victron_ve_direct), gCfg.veDirectEn, T(T_set_j14_j15_isolated));
        fieldText  (html, "outminbatt",  T(T_set_heater_fan_minimum_battery_voltage), String(gCfg.outMinBattDv / 10.0f, 1), 5,
                    T(T_set_0_no_limit_otherwise_missing), "number");
        fieldCheck (html, "uven",        T(T_set_uv_sensor_gpio3), gCfg.uvEn,
                    T(T_set_raw_sensor_voltage_is_reported));
        advField = true;
        fieldText  (html, "uvscale",     T(T_set_uv_divider_scale), String(gCfg.uvAinScale, 5), 10,
                    T(T_set_terminal_v_per_adc_v), "number");
        advField = false;
        fieldCheck (html, "uvindexen",   T(T_set_publish_calculated_uv_index), gCfg.uvIndexEn);
        fieldText  (html, "uvslope",     T(T_set_uv_index_slope_index_v), String(gCfg.uvIndexSlope, 3), 10,
                    T(T_set_linear_calibration_valid_range_0), "number");
        fieldText  (html, "uvoffset",    T(T_set_uv_index_offset), String(gCfg.uvIndexOffset, 3), 10,
                    T(T_set_linear_calibration_valid_range_10), "number");
        fieldCheck (html, "uvcal",       T(T_set_calibration_explicitly_verified), gCfg.uvCalibrated,
                    T(T_set_index_remains_omitted_until_this));
        html += "</section>";
    }

    if (rfDavisMode(gCfg.rfEn, gCfg.rfProto)) {
        const Option dvIds[] = { { 0, T(T_opt_0_the_first_transmitter_heard) }, { 1, "1" }, { 2, "2" }, { 3, "3" }, { 4, "4" },
                                        { 5, "5" }, { 6, "6" }, { 7, "7" }, { 8, "8" } };
        const Option dvModels[]  = { { 0, T(T_opt_vantage_pro2_6332_potentiometer_vane) }, { 1, T(T_opt_vantage_vue_hall_vane) } };
        const Option dvBuckets[] = { { 0, T(T_opt_0_2_mm_eu) }, { 1, T(T_opt_0_01_in_0_254) } };
        const Option dvTables[]  = { { 0, T(T_opt_measured_2026_868_0589_2993) },
                                            { 1, T(T_opt_classic_davisrfm69_868_0667_2971) } };
        const Option dvBws[]     = { { 586, "58.6 kHz" }, { 782, "78.2 kHz" }, { 938, T(T_opt_93_8_khz_default) },
                                            { 1173, "117.3 kHz" }, { 1562, "156.2 kHz" }, { 2343, "234.3 kHz" } };
        const Option dvChs[]     = { { 0, "ch0" }, { 1, "ch1" }, { 2, "ch2" }, { 3, "ch3" }, { 4, "ch4" } };
        sectionStart(html, T(T_set_davis_diagnostics), false,
                     T(T_set_experimental_all_fields_apply_at), true);
        fieldSelect(html, "dvid",     T(T_set_transmitter_id), gCfg.davisTxId, OPTS(dvIds),
                    T(T_set_dip_switch_setting_of_the));
        fieldSelect(html, "dvmodel",  T(T_set_station), gCfg.davisModel, OPTS(dvModels), T(T_set_wind_direction_formula));
        fieldSelect(html, "dvbucket", T(T_set_rain_collector), gCfg.davisBucket, OPTS(dvBuckets));
        fieldText  (html, "dvoff",    T(T_set_frequency_offset_khz), String(gCfg.davisOffsetHz / 1000.0f, 1), 7,
                    T(T_set_100_to_100_added_to), "number");
        fieldText  (html, "dvscan",   T(T_set_search_offset_scan_khz), String(gCfg.davisScanKhz), 3,
                    T(T_set_0_60_while_searching_try), "number");
        fieldCheck (html, "dvafc",    T(T_set_afc_while_tracking), gCfg.davisAfc,
                    T(T_set_every_4th_packet_is_listened));
        fieldText  (html, "dvwin",    T(T_set_rx_window_late_ms), String(gCfg.davisWindowMs), 4,
                    T(T_set_5_500_a_packet_may), "number");
        fieldText  (html, "dvguard",  T(T_set_rx_window_early_guard_ms), String(gCfg.davisGuardMs), 4,
                    T(T_set_5_500_a_packet_may2), "number");
        fieldCheck (html, "dvrfilt",  T(T_set_dv_rain_filter), gCfg.davisRainFilter, T(T_set_dv_rain_filter_hint));
        fieldText  (html, "dvrpkt",   T(T_set_dv_rain_pkt), String(gCfg.davisRainMaxPkt), 3, T(T_set_dv_rain_pkt_hint), "number");
        fieldText  (html, "dvrmin",   T(T_set_dv_rain_min), String(gCfg.davisRainMaxMin), 3, T(T_set_dv_rain_min_hint), "number");
        fieldSelect(html, "dvtable",  T(T_set_channel_table), gCfg.davisTable, OPTS(dvTables));
        fieldSelect(html, "dvbw",     T(T_set_receiver_bandwidth), gCfg.davisRxBw, OPTS(dvBws), T(T_set_narrower_more_sensitive_wider_more));
        fieldCheck (html, "dvsniff",  T(T_set_sniffer_every_packet_to_the), gCfg.davisSniffer,
                    T(T_set_raw_bytes_crc_decoded_values));
        fieldCheck (html, "dvraw",    T(T_set_raw_capture_no_hopping), gCfg.davisRaw,
                    T(T_set_parks_on_the_channel_below));
        fieldSelect(html, "dvrawch",  T(T_set_raw_capture_channel), gCfg.davisRawCh, OPTS(dvChs));
        html += "</section>";
    }

    html += "</div>";

    // ======== Outputs ========
    html += "<div class='tab' id='outputs'>";
    sectionStart(html, T(T_set_send_intervals_and_wind_values), false);
    fieldText  (html, "mqttsec",   T(T_set_mqtt_interval_s), String(gCfg.mqttIntervalSec), 5, T(T_set_0_like_the_lorawan_interval), "number");
    fieldSelect(html, "mqttwagg",  T(T_set_mqtt_wind_values), gCfg.mqttWindAgg, OPTS(windAggs));
    fieldSelect(html, "lorawagg",  T(T_set_lorawan_wind_values), gCfg.loraWindAgg, OPTS(windAggs),
                T(T_set_interval_mean_3_s_gust));
    html += "</section>";

    sectionStart(html, T(T_set_mqtt_home_assistant), true);
    fieldCheck (html, "mqtten",     T(T_set_mqtt_enabled), gCfg.mqttEn);
    { DepScope dep("mqtten=1");
    fieldCheck (html, "mqttctrlen", T(T_set_mqtt_command_topic), gCfg.mqttCtrlEn, T(T_set_accept_commands_on_prefix_cmd));
    fieldText  (html, "mqtthost",   T(T_set_broker_host), gCfg.mqttHost, sizeof(gCfg.mqttHost) - 1);
    fieldText  (html, "mqttport",   T(T_set_broker_port), String(gCfg.mqttPort), 5, T(T_set_1883_plain_8883_tls), "number");
    fieldText  (html, "mqttuser",   T(T_set_user_name), gCfg.mqttUser, sizeof(gCfg.mqttUser) - 1, "optional");
    fieldSecret(html, "mqttpass",   T(T_set_password), gCfg.mqttPass[0], true, sizeof(gCfg.mqttPass) - 1, "optional");
    fieldText  (html, "mqtttopic",  T(T_set_topic_prefix), gCfg.mqttTopic, sizeof(gCfg.mqttTopic) - 1, T(T_set_optional_defaults_to_the_device));
    fieldSelect(html, "mqtttls",    T(T_set_security), gCfg.mqttTlsMode, OPTS(tlsModes));
    }
    { DepScope dep("mqtten=1&mqtttls=2");
    labelStart (html, T(T_set_ca_certificate_pem), T(T_set_only_for_verify));
    }
    capture("mqttcacert", gCfg.mqttCaCert);
    html += "<textarea name='mqttcacert' rows='6' maxlength='"; html += (unsigned)(sizeof(gCfg.mqttCaCert) - 1);
    html += "' placeholder='-----BEGIN CERTIFICATE-----'>";
    appendEscaped(html, gCfg.mqttCaCert);
    html += "</textarea></label></section>";


    wxUploadLock();
    sectionStart(html, T(T_set_weather_networks), false,
                 T(T_set_direct_uploads_over_wifi_each));
    fieldText  (html, "awkuser", T(T_set_awekas_user_name), gCfg.awekasUser, sizeof(gCfg.awekasUser) - 1);
    { DepScope dep("awkuser!=");
    fieldSecret(html, "awkpass", T(T_set_awekas_password), gCfg.awekasPass[0], true, sizeof(gCfg.awekasPass) - 1);
    fieldText  (html, "awkint",  T(T_set_awekas_interval_s), String(gCfg.awekasIntervalSec), 4, "300&ndash;3600", "number");
    }
    fieldText  (html, "wuid",    T(T_set_weather_underground_station_id), gCfg.wuStationId, sizeof(gCfg.wuStationId) - 1);
    { DepScope dep("wuid!=");
    fieldSecret(html, "wukey",   T(T_set_weather_underground_station_key), gCfg.wuStationKey[0], true, sizeof(gCfg.wuStationKey) - 1);
    fieldText  (html, "wuint",   T(T_set_weather_underground_interval_s), String(gCfg.wuIntervalSec), 4, "60&ndash;3600", "number");
    }
    fieldText  (html, "wcid",    T(T_set_weathercloud_id), gCfg.wcloudId, sizeof(gCfg.wcloudId) - 1);
    { DepScope dep("wcid!=");
    fieldSecret(html, "wckey",   T(T_set_weathercloud_key), gCfg.wcloudKey[0], true, sizeof(gCfg.wcloudKey) - 1);
    fieldText  (html, "wcint",   T(T_set_weathercloud_interval_s), String(gCfg.wcloudIntervalSec), 4, T(T_set_60_3600_free_accounts_600), "number");
    }
    fieldText  (html, "wnpid",   T(T_set_wetternetzwerk_pro_station_id), gCfg.wnetproId, sizeof(gCfg.wnetproId) - 1);
    { DepScope dep("wnpid!=");
    fieldSecret(html, "wnpkey",  T(T_set_wetternetzwerk_pro_station_key), gCfg.wnetproKey[0], true, sizeof(gCfg.wnetproKey) - 1);
    fieldText  (html, "wnpint",  T(T_set_wetternetzwerk_pro_interval_s), String(gCfg.wnetproIntervalSec), 4, "60&ndash;3600", "number");
    }
    fieldText  (html, "windyid", T(T_set_windy_station_id), gCfg.windyId, sizeof(gCfg.windyId) - 1, T(T_set_stations_windy_com_my_stations));
    { DepScope dep("windyid!=");
    fieldSecret(html, "windypw", T(T_set_windy_station_password), gCfg.windyPass[0], true, sizeof(gCfg.windyPass) - 1);
    fieldText  (html, "windyint",T(T_set_windy_interval_s), String(gCfg.windyIntervalSec), 4, "300&ndash;3600", "number");
    }
    fieldText  (html, "osemid",  T(T_set_opensensemap_sensebox_id), gCfg.osemBoxId, sizeof(gCfg.osemBoxId) - 1);
    { DepScope dep("osemid!=");
    fieldSecret(html, "osemtok", T(T_set_opensensemap_access_token), gCfg.osemToken[0], true, sizeof(gCfg.osemToken) - 1,
                T(T_set_only_for_boxes_with_authentication));
    fieldText  (html, "osemmap", T(T_set_opensensemap_sensor_ids), gCfg.osemSensors, sizeof(gCfg.osemSensors) - 1,
                T(T_set_osem_hint));
    fieldText  (html, "osemint", T(T_set_opensensemap_interval_s), String(gCfg.osemIntervalSec), 4, "60&ndash;3600", "number");
    }
    fieldText  (html, "wowid",   T(T_set_wow_id), gCfg.wowSiteId, sizeof(gCfg.wowSiteId) - 1, T(T_set_wow_hint));
    { DepScope dep("wowid!=");
    fieldSecret(html, "wowpin",  T(T_set_wow_pin), gCfg.wowPin[0], true, sizeof(gCfg.wowPin) - 1);
    fieldText  (html, "wowint",  T(T_set_wow_int), String(gCfg.wowIntervalSec), 4, "300&ndash;3600", "number");
    }
    fieldText  (html, "cwopcall", T(T_set_cwop_call), gCfg.cwopCall, sizeof(gCfg.cwopCall) - 1, T(T_set_cwop_hint));
    { DepScope dep("cwopcall!=");
    fieldText  (html, "cwoppass", T(T_set_cwop_pass), String(gCfg.cwopPasscode), 6, T(T_set_cwop_pass_hint), "number");
    fieldText  (html, "cwopint",  T(T_set_cwop_int), String(gCfg.cwopIntervalSec), 4, "300&ndash;3600", "number");
    }
    html += "</section>";

    wxUploadUnlock();
    renderCustomUploads(html);

    sectionStart(html, T(T_set_loxone_udp), false,
                 T(T_set_sends_every_value_as_its));
    fieldCheck (html, "loxen",     T(T_set_loxone_udp_output), gCfg.loxEn);
    loxoneLock();
    { DepScope dep("loxen=1");
    fieldText  (html, "loxhost",   T(T_set_miniserver_ip_or_host_name), gCfg.loxHost, sizeof(gCfg.loxHost) - 1,
                T(T_set_a_host_name_is_resolved));
    fieldText  (html, "loxprefix", T(T_set_name_prefix), gCfg.loxPrefix, sizeof(gCfg.loxPrefix) - 1,
                T(T_set_optional_a_z_a_z));
    loxoneUnlock();
    fieldText  (html, "loxport",   T(T_set_udp_port), String(gCfg.loxPort), 5, T(T_set_1_65535_default_7000), "number");
    fieldText  (html, "loxsec",    T(T_set_loxone_interval_s), String(gCfg.loxSec), 5,
                T(T_set_0_every_measurement_cycle_else), "number");
    }
    html += "</section>";

    sectionStart(html, T(T_set_lorawan), true);
    fieldCheck (html, "loraen", T(T_set_lorawan_enabled), gCfg.loraEn);
    { DepScope dep("loraen=1");
    fieldCheck (html, "lowpower", T(T_set_low_power_mode), gCfg.lowPowerEn, T(T_set_light_sleep_between_samples_only));
    fieldCheck (html, "vanesave", T(T_set_low_power_switch_wind_vane), gCfg.vanePowerSave, T(T_set_pcb_v2_0_off_for));
    fieldText  (html, "deveui",  T(T_set_deveui), hexOf(gCfg.devEui), 16, T(T_set_16_hex_characters));
    fieldText  (html, "joineui", T(T_set_joineui), hexOf(gCfg.joinEui), 16, T(T_set_16_hex_characters));
    fieldSecret(html, "appkey",  T(T_set_appkey), true, false, 32, T(T_set_32_hex_characters));
    labelStart (html, T(T_set_region), nullptr);
    capture("region", gCfg.region);
    html += "<select name='region'>";
    for (const char* r : LORA_REGIONS) {
        html += "<option"; if (!strcmp(r, gCfg.region)) html += " selected"; html += ">"; html += r; html += "</option>";
    }
    html += "</select></label>";
    fieldSelect(html, "payfmt", T(T_set_payload_format), gCfg.payloadFormat, OPTS(payFmts),
                T(T_set_applies_immediately_slow_data_rates));
    fieldReadonly(html, T(T_set_decoder), String("<a href='/decoder' download>") + T(T_set_decoder_download) + "</a>",
                  TF(T_set_decoder_hint, FIRMWARE_VERSION).c_str());
    }
    html += "</section>";

    html += "</div>";

    // ======== Alarms ========
    html += "<div class='tab' id='alarms'>";
    gFieldForm = "aform";
    renderAlarmsTab(html);
    gFieldForm = nullptr;
    html += "<p><button form='aform' type='submit'>"; html += T(T_set_save); html += "</button></p>";
    html += "</div>";

    // ======== Display (built-in OLED, oled_screens.h) ========
    html += "<div class='tab' id='display'>";
    {
        const Option modes[] = { { 0, T(T_set_disp_mode0) }, { 1, T(T_set_disp_mode1) }, { 2, T(T_set_disp_mode2) } };
        sectionStart(html, T(T_set_disp_title), false, T(T_set_disp_note));
        fieldSelect(html, "dispmode", T(T_set_disp_mode), gCfg.dispMode, OPTS(modes));
        { DepScope dep("dispmode=2"); fieldText(html, "dispmin", T(T_set_disp_min), String(gCfg.dispMinutes), 3, "1&ndash;120", "number"); }
        static const TextId scr[OS_COUNT] = { T_set_disp_s0, T_oled_temp, T_oled_wind, T_oled_rain, T_oled_press, T_oled_status };
        for (int k = 0; k < OS_COUNT; k++) {
            char n[8];
            snprintf(n, sizeof(n), "dsp%d", k);
            fieldCheck(html, n, (String(T(T_set_disp_screen)) + ": " + T(scr[k])).c_str(), gCfg.dispScreens & (1 << k));
        }
        fieldText  (html, "disprot",  T(T_set_disp_rot), String(gCfg.dispRotateSec), 3, "2&ndash;60", "number");
        fieldText  (html, "dispcon",  T(T_set_disp_con), String(gCfg.dispContrast), 3, "1&ndash;255", "number");
        {   // night off: only with "always on"
            DepScope dep("dispmode=1");
            fieldText  (html, "dispnf",   T(T_set_disp_nf), String(gCfg.dispNightFrom), 2, T(T_set_disp_night_hint), "number");
            fieldText  (html, "dispnt",   T(T_set_disp_nt), String(gCfg.dispNightTo), 2, nullptr, "number");
        }
        html += "</section>";
    }
    html += "</div>";

    // ======== System ========
    html += "<div class='tab' id='system'>";
    sectionStart(html, T(T_set_network), true);
    fieldText  (html, "wifissid",   T(T_set_wifi_network_ssid), WiFi.SSID(), 32,
                T(T_set_a_change_is_tested_for));
    fieldSecret(html, "wifipass",   T(T_set_wifi_password), true, true, 63,
                T(T_set_leave_empty_to_keep_the));
    fieldCheck (html, "wifinotest", T(T_set_store_wifi_without_test_pre), false,
                T(T_set_for_a_network_that_is));
    fieldCheck (html, "staticipen", T(T_set_static_ip), gCfg.staticIpEn, T(T_set_off_dhcp));
    { DepScope dep("staticipen=1");
    fieldText  (html, "staticip",   T(T_set_ip_address), gCfg.staticIp, 15);
    fieldText  (html, "staticgw",   T(T_set_gateway), gCfg.staticGateway, 15);
    fieldText  (html, "staticsn",   T(T_set_subnet_mask), gCfg.staticSubnet, 15);
    fieldText  (html, "staticdns",  T(T_set_dns_server), gCfg.staticDns, 15, T(T_set_optional_defaults_to_the_gateway));
    }
    html += "</section>";

    sectionStart(html, T(T_set_firmware_update_check), false);
    fieldCheck (html, "updchk", T(T_set_check_daily_for_newer_firmware), gCfg.updateCheckEn,
                T(T_set_shown_on_the_status_page));
    advField = true;
    fieldText  (html, "updurl", T(T_set_version_list_url), gCfg.updateUrl, sizeof(gCfg.updateUrl) - 1,
                T(T_set_tags_gitea_or_releases_github));
    advField = false;
    html += "</section>";

    sectionStart(html, T(T_set_hardware), true);
    {
        String boardHint = TF(boardIsR8() ? T_set_board_hint_r8 : T_set_board_hint, boardName(gBoard.detected),
                              String((unsigned long)(gBoard.flashBytes >> 20)));
        fieldSelect(html, "board", T(T_set_heltec_board), gCfg.boardSel, OPTS(boards), boardHint.c_str());
    }
    if (boardPcbAllowed())
        fieldSelect(html, "pcbver", T(T_set_pcb_revision), gCfg.pcbVersion, OPTS(pcbRevs));
    else
        fieldReadonly(html, T(T_set_pcb_revision), T(T_set_ignored_on_heltec_v4), T(T_set_wireless_build_only_pcb_inputs2));
    advField = true;
    fieldSelect(html, "cpufreq",   T(T_set_cpu_frequency), gCfg.cpuFreqMhz, OPTS(cpuFreqs));
    fieldCheck (html, "debugmode", T(T_set_debug_mode), gCfg.debugMode, T(T_set_adds_diagnostic_fields_to_every));
    advField = false;
    html += "</section>";
    // Wi-Fi / web interface off (advanced): refused by handleSettingsPost()
    // unless confirmed and LoRaWAN is joined (radioOffCheck(), net_check.h)
    sectionStart(html, T(T_set_radio_off_title), true, T(T_set_radio_off_note), true);
    if (!loraReady()) fieldReadonly(html, T(T_set_lorawan), T(T_set_radio_off_not_joined));
    fieldCheck (html, "wifien",   T(T_set_wifi_enabled), gCfg.wifiEn, T(T_set_wifi_enabled_hint));
    fieldCheck (html, "webapien", T(T_set_webapi_enabled), gCfg.webApiEn, T(T_set_webapi_enabled_hint));
    labelStart (html, T(T_set_radio_off_confirm), nullptr);   // not a setting: never captured into a backup
    html += "<input type='checkbox' name='radiooffok' value='1'></label>";
    html += "</section>";
    sectionStart(html, T(T_wz_title), false);
    fieldReadonly(html, T(T_wz_title), String("<a href='/setup'>") + T(T_wz_run) + "</a>", T(T_wz_run_hint));
    html += "</section>";
    {   // backup / restore (handleSettingsBackup / handleSettingsRestore): own forms after the settings form
        sectionStart(html, T(T_set_bk_title), false);
        html += "<p><small>"; html += T(T_set_bk_hint); html += "</small></p>";
        html += "<p><label><input type='checkbox' id='bksec'> "; html += T(T_set_bk_secrets); html += "</label><br><small><b>";
        html += T(T_set_bk_secrets_warn); html += "</b></small></p><p><a id='bkdl' href='/api/settings/backup'>";
        html += T(T_set_bk_download); html += "</a></p><p><label>"; html += T(T_set_bk_restore);
        html += " <input type='file' id='bkfile' accept='.json,application/json'></label></p>";
        html += "</section>";
    }
    {   // firmware upload (handleOtaDone / handleOtaChunk): its form after the settings form
        sectionStart(html, T(T_ota_title), false, nullptr, false, false);   // action, no settings: no live/restart tag
        html += "<p><small>"; html += T(T_set_ota_hint); html += "</small></p>";
        html += "<p><input type='file' name='firmware' form='oform' accept='.bin,application/octet-stream' required> "
                "<button type='submit' form='oform' id='otabtn'>"; html += T(T_set_ota_upload); html += "</button></p>"
                "<p><progress id='otaprog' max='100' value='0' hidden></progress> <span id='otamsg'></span></p><p id='otarc'></p>";
        html += "</section>";
    }
    {   // records (records.h) and chart history (history.h): buttons, each posts the
        // form 'rform' (POST /api/settings/reset) after a confirmation - no Save needed
        sectionStart(html, T(T_set_records), false, nullptr, false, false);   // actions: no live/restart tag
        html += "<p>";
        const struct { const char* what; TextId text; } acts[] = {
            { "records", T_set_rec_reset_periods }, { "recordsall", T_set_rec_reset_all }, { "history", T_set_hist_reset },
            { "rain", T_set_rain_reset }, { "sunshine", T_set_sun_reset }, { "aux", T_set_aux_reset } };
        for (const auto& a : acts) {
            if (!strcmp(a.what, "aux") && !ioV2()) continue;   // AUX pulse input: PCB v2.0 only
            html += "<button type='submit' form='rform' name='what' value='"; html += a.what;
            html += "' onclick='return confirm(this.dataset.q)' data-q='"; html += T(T_set_reset_confirm); html += "'>";
            html += T(a.text); html += "</button> ";
        }
        html += "</p><p><small>"; html += T(T_set_rec_reset_hint); html += "</small></p></section>";
    }
    sectionStart(html, T(T_set_diagnostics), false);
    fieldReadonly(html, T(T_set_log), T(T_set_open_the_device_log));
    fieldReadonly(html, T(T_set_radio_trace), T(T_set_status_page_wireless_sensors_radio),
                  T(T_set_records_every_packet_the_receiver));
    html += "</section>";
    html += "</div>";
    #undef OPTS

    html += "<button type='submit' id='ssave'>"; html += T(T_set_save); html += "</button></form>"
            "<form id='aform' method='POST' action='/api/alarms/save'><input type='hidden' name='form' value='1'></form>"
            "<form id='oform' method='POST' action='ota' enctype='multipart/form-data'></form>"
            "<form id='rform' method='POST' action='/api/settings/reset'></form>"
            "<script>(function(){var d=document,ids=['basics','sensors','outputs','alarms','display','system'];"
            "d.documentElement.className+=' js';"
            "function show(){var h=location.hash.slice(1);if(ids.indexOf(h)<0)h='basics';"
            "ids.forEach(function(t){d.getElementById(t).classList.toggle('on',t==h);});"
            "d.querySelectorAll('.tabs a').forEach(function(a){a.classList.toggle('on',a.getAttribute('href')=='#'+h);});"
            "d.getElementById('ssave').style.display=h=='alarms'?'none':'';}"
            "window.addEventListener('hashchange',show);show();"
            "d.getElementById('sform').addEventListener('submit',function(){this.action='/api/settings'+(location.hash||'#basics');});"
            "var t=d.getElementById('advtgl');try{t.checked=localStorage.getItem('wnAdv')=='1';}catch(e){}"
            "t.addEventListener('change',function(){try{localStorage.setItem('wnAdv',t.checked?'1':'0');}catch(e){}});"
            // backup: secrets only when ticked; restore: the file's text posted as one field
            "var bs=d.getElementById('bksec');if(bs)bs.addEventListener('change',function(){"
            "d.getElementById('bkdl').href='/api/settings/backup'+(bs.checked?'?secrets=1':'');});"
            "var bf=d.getElementById('bkfile');if(bf)bf.addEventListener('change',function(){var f=bf.files[0];if(!f)return;"
            "var r=new FileReader();r.onload=function(){var fm=d.createElement('form');fm.method='POST';"
            "fm.action='/api/settings/restore#system';var i=d.createElement('input');i.type='hidden';i.name='backup';"
            "i.value=r.result;fm.appendChild(i);d.body.appendChild(fm);fm.submit();};r.readAsText(f);});";
    // firmware upload with a progress bar (XHR); without XHR / FormData the
    // form is posted normally and the node answers with a page
    html += "var O={up:"; jsString(html, T(T_ota_uploading)); html += ",rs:"; jsString(html, T(T_ota_restart_in));
    html += ",lost:"; jsString(html, T(T_ota_lost));
    html += ",rc:"; jsString(html, T(T_ota_reconnect)); html += "};";
    html += "var of=d.getElementById('oform');if(of&&window.FormData&&window.XMLHttpRequest)of.addEventListener('submit',function(e){"
            "var fi=d.querySelector('input[form=oform]');if(!fi||!fi.files[0])return;e.preventDefault();"
            "var fd=new FormData();fd.append('firmware',fi.files[0]);"
            "var x=new XMLHttpRequest(),p=d.getElementById('otaprog'),m=d.getElementById('otamsg'),b=d.getElementById('otabtn');"
            "b.disabled=true;p.hidden=false;p.value=0;window.wnBusy=1;"
            "x.upload.onprogress=function(ev){if(ev.lengthComputable){var v=Math.round(ev.loaded*100/ev.total);p.value=v;m.textContent=O.up.replace('{0}',v);}};"
            "x.onload=function(){m.textContent=x.responseText;if(x.status==200){d.getElementById('otarc').innerHTML=O.rc;var n=30,t=setInterval(function(){"
            "m.textContent=x.responseText+' '+O.rs.replace('{0}',n);if(--n<0){clearInterval(t);location.href='/status';}},1000);}"
            "else{b.disabled=false;p.hidden=true;window.wnBusy=0;}};"
            "x.onerror=function(){m.textContent=O.lost;b.disabled=false;window.wnBusy=0;};"
            "x.open('POST','ota?js=1');x.send(fd);});"
            "})();</script></body></html>";
    html.finish();
}

// ---------------------------------------------------------------------------
// Setup wizard (/setup, setup_wizard.h): seven short pages, each a form that
// posts its fields to POST /api/settings (wizard=1: back here instead of the
// settings page; restart fields are saved but applied by one restart at
// "Finish"). Step 2 reloads itself every 10 s to show new sensors.
// ---------------------------------------------------------------------------
bool wizardRestartPending = false;   // a step saved a restart field (device name, MQTT, ...)

void wizardNav(HtmlOut& html, int step)
{
    // Next = submit; Back / Skip = plain links (nothing posted)
    html += "<p class='wzn'>";
    if (step > 1) { html += "<a href='/setup?step="; html += step - 1; html += "'>&larr; "; html += T(T_wz_back); html += "</a> "; }
    html += "<a href='/setup?step="; html += step + 1; html += "'>"; html += T(T_wz_skip); html += "</a> ";
    html += "<button type='submit'>"; html += T(T_wz_next); html += "</button></p>";
}

// candidates of a running auto search, short (wizard step 2)
void addAutoSearchRowsWizard(HtmlOut& html)
{
    const AutoSearch& as = rfAutoSearchState();
    const float thr = autoRssiThreshold(boardIsV4());
    for (const AutoCand& c : as.c) {
        if (!c.id) continue;
        html += "<p class='dim'>"; html += TF(T_st_auto_cand_info, rfProtoName(c.proto), String(c.inWindow), String(AUTO_MIN_PACKETS),
                                             String(c.meanRssi(), 0));
        if (c.meanRssi() < thr) { html += " &middot; "; html += T(T_st_auto_weak); }
        html += "</p>";
    }
}

void wizardSensorRows(HtmlOut& html)
{
    bool any = false;
    html += "<table class='rft'>";
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (!r.used) continue;
        any = true;
        char id[12];
        snprintf(id, sizeof(id), "%08lX", (unsigned long)r.id);
        html += "<tr><td>";
        html += rfStateOf(r.id) == RF_STATE_MINE ? "&#9733;" : "&#9675;";
        html += "</td><td><b>"; html += rfKindText(r); html += "</b> <span class='num dim'>"; html += id;
        uint32_t eco;
        if (r.proto == RF_PROTO_FINEOFFSET && rfEcowittId(r.id, eco)) {
            char e[8]; snprintf(e, sizeof(e), "%lX", (unsigned long)eco);
            html += TF(T_st_ecowitt_id, e);
        }
        html += "</span><br><small>" + String(r.rssi, 0) + " dBm &middot; " + agoText(r.lastMs) + " &middot; " + rfValuesShort(r) + "</small>";
        // name (rf_names.h), saved with "Save and next"
        html += "<br><input name='rfnm_"; html += id; html += "' maxlength='24' size='14' placeholder='";
        html += T(T_set_th_name); html += "' value='"; appendEscaped(html, rfNameOf(r.id)); html += "'></td><td>";
        html += rfQuickActions(r.id, "setup");
        html += "</td></tr>";
    }
    html += "</table>";
    if (!any) {
        html += "<p><b>";
        html += T(gCfg.rfProto == RF_PROTO_AUTO ? T_wz_searching : T_wz_waiting);
        html += "</b></p>";
    }
    if (rfSensorsDetectedProto()) {   // found by Auto (search): confirm, or search again (back to Auto)
        html += "<p>"; html += TF(T_wz_found_type, rfSensorTypeText(rfSensorsDetectedProto()));
        if (rfAutoLockedWeak()) { html += " &middot; <b>"; html += T(T_st_auto_weak); html += "</b>"; }
        html += " &mdash; "; html += T(T_wz_found_confirm);
        html += " <button class='sb' formaction='/api/settings?wstay=1' name='rfproto' value='"; html += RF_PROTO_AUTO; html += "'>";
        html += T(T_wz_search_again);
        html += "</button></p>";
    } else if (gCfg.rfProto == RF_PROTO_AUTO) {
        addAutoSearchRowsWizard(html);
    }
    const int hidden = rfHiddenInRange(millis());
    if (hidden) { html += "<p class='dim'>"; html += TF(T_st_hidden_in_range, String(hidden)); html += "</p>"; }
}

void renderWizard(int step, const String& message, int httpCode)
{
    step = wizardClampStep(step);
    // the dashboard sends a browser here only once per session (wizardAutoStart())
    server.sendHeader("Set-Cookie", "wnSetup=1; Path=/; SameSite=Lax");
    HtmlOut html(server, httpCode);
    pageStart(html, T(T_wz_title), "/setup", 0);
    // step 2: the sensor list reloads every 10 s - not while a name is being typed
    if (step == 2)
        html += "<script>(function(){var dirty=false;document.addEventListener('input',function(){dirty=true;});"
                "setInterval(function(){if(!dirty&&!document.querySelector('input:focus'))location.reload();},10000);})();</script>";
    html += "<style>.wzn{display:flex;gap:1rem;align-items:center;flex-wrap:wrap;justify-content:flex-end;}"
            ".wzn a{margin-right:auto;}.wzn a+a{margin-right:0;}details{margin:0.4rem 0;}summary{cursor:pointer;font-weight:600;}</style>";
    static const TextId titles[WIZ_STEPS] = { T_set_language, T_wz_t_station, T_set_location, T_set_units, T_set_admin_password,
                                              T_wz_t_outputs, T_wz_t_summary };
    html += "<p class='sub'>"; html += TF(T_wz_step, String(step), String(WIZ_STEPS)); html += " &middot; <b>";
    html += T(titles[step - 1]); html += "</b></p>";
    if (message.length()) { html += "<div class='banner badb'>"; html += message; html += "</div>"; }
    if (wizardOpenAccess(gCfg) && step < 5) {
        html += "<div class='banner badb'>"; html += T(T_wz_open_warning); html += "</div>";
    }

    if (step < WIZ_STEPS) {
        html += "<form method='POST' action='/api/settings'><input type='hidden' name='form' value='1'>"
                "<input type='hidden' name='wizard' value='1'><input type='hidden' name='wstep' value='";
        html += step; html += "'><section>";
    }
    switch (step) {
        case 1: {
            html += "<p>"; html += T(T_wz_intro); html += "</p>";
            const Option langs[] = { { 0, T(T_set_lang_auto) }, { 1, "English" }, { 2, "Deutsch" }, { 3, "Fran&ccedil;ais" },
                                     { 4, "Espa&ntilde;ol" }, { 5, "Portugu&ecirc;s" } };
            // preselected: the setting (Auto = follows the browser - the page is already in its language)
            fieldSelect(html, "uilang", T(T_set_language), gCfg.uiLang, langs,
                        sizeof(langs) / sizeof(langs[0]), T(T_set_language_hint));
            break;
        }
        case 2:
            html += "<p>"; html += T(T_wz_station_intro); html += "</p>";
            if (!gCfg.rfEn) {
                html += "<p><b>"; html += T(T_wz_rf_off); html += "</b> <button name='rfen' value='1'>";
                html += T(T_wz_rf_on); html += "</button></p>";
            } else {
                wizardSensorRows(html);
                html += "<p><button name='rfen' value='0' class='sb'>"; html += T(T_wz_no_sensors); html += "</button></p>";
            }
            break;
        case 3:
            html += "<p>"; html += T(T_wz_location_intro); html += "</p>";
            fieldText(html, "devname", T(T_set_device_name), gCfg.deviceName, sizeof(gCfg.deviceName) - 1, T(T_wz_name_hint));
            fieldText(html, "altitude", T(T_set_altitude_above_sea_level_m), String(gCfg.altitudeM), 5, T(T_wz_altitude_hint), "number");
            wxUploadLock();
            fieldText(html, "lat", T(T_set_latitude), gCfg.latitude, sizeof(gCfg.latitude) - 1, T(T_set_decimal_degrees_e_g_47));
            fieldText(html, "lon", T(T_set_longitude), gCfg.longitude, sizeof(gCfg.longitude) - 1, T(T_set_decimal_degrees_e_g_11));
            fieldText(html, "tz", T(T_set_time_zone_posix_tz), gCfg.timeZone, sizeof(gCfg.timeZone) - 1, T(T_set_central_europe_cet_1cest_m3));
            wxUploadUnlock();
            // position: only where the browser allows it (secure context: https or localhost - mostly not on http://<ip>/);
            // time zone: browser zone -> POSIX TZ for the common zones
            html += "<p id='geo' style='display:none'><button type='button' class='sb' onclick='wzGeo()'>"; html += T(T_wz_geo);
            html += "</button> <span id='geom' class='dim'></span></p><p id='tzb' class='dim'></p>"
                    "<script>(function(){var f=document.forms[0];"
                    "if(window.isSecureContext&&navigator.geolocation)document.getElementById('geo').style.display='';"
                    "window.wzGeo=function(){navigator.geolocation.getCurrentPosition(function(p){f.lat.value=p.coords.latitude.toFixed(4);"
                    "f.lon.value=p.coords.longitude.toFixed(4);if(p.coords.altitude!=null&&f.altitude.value=='560')f.altitude.value=Math.round(p.coords.altitude);},"
                    "function(){document.getElementById('geom').textContent=";
            jsString(html, T(T_wz_geo_fail));
            html += ";});};"
                    "var z='';try{z=Intl.DateTimeFormat().resolvedOptions().timeZone||'';}catch(e){}"
                    "var C='CET-1CEST,M3.5.0,M10.5.0/3',E='EET-2EEST,M3.5.0/3,M10.5.0/4',G='GMT0BST,M3.5.0/1,M10.5.0',"
                    "m={'Europe/Lisbon':'WET0WEST,M3.5.0/1,M10.5.0','Europe/London':G,'Europe/Dublin':'IST-1GMT0,M10.5.0,M3.5.0/1',"
                    "'Europe/Helsinki':E,'Europe/Athens':E,'Europe/Bucharest':E,'Europe/Sofia':E,'Europe/Kiev':E,'Europe/Kyiv':E,'Europe/Riga':E,"
                    "'Europe/Tallinn':E,'Europe/Vilnius':E,'America/New_York':'EST5EDT,M3.2.0,M11.1.0','America/Chicago':'CST6CDT,M3.2.0,M11.1.0',"
                    "'America/Denver':'MST7MDT,M3.2.0,M11.1.0','America/Phoenix':'MST7','America/Los_Angeles':'PST8PDT,M3.2.0,M11.1.0',"
                    "'America/Sao_Paulo':'<-03>3','Australia/Sydney':'AEST-10AEDT,M10.1.0,M4.1.0/3'};"
                    "var t=m[z]||(/^Europe\\/(Berlin|Vienna|Zurich|Paris|Madrid|Rome|Amsterdam|Brussels|Prague|Warsaw|Stockholm|Oslo|Copenhagen|Budapest|Luxembourg|Ljubljana|Zagreb|Bratislava|Belgrade|Monaco|Vaduz|Malta|Andorra)$/.test(z)?C:'');"
                    "if(z)document.getElementById('tzb').textContent=";
            jsString(html, T(T_wz_tz_browser));
            html += ".replace('{0}',z);if(t&&f.tz.value!=t&&f.tz.value==C&&t!=C)f.tz.value=t;})();</script>";
            break;
        case 4: {
            html += "<p>"; html += T(T_wz_units_intro); html += "</p>";
            const Option unitSystems[] = { { 0, T(T_opt_metric_c_mm_hpa) }, { 1, T(T_opt_imperial_f_in_inhg_mph) } };
            const Option windUnits[]   = { { 1, "m/s" }, { 0, "km/h" } };
            fieldSelect(html, "unitsys",  T(T_set_unit_system), gCfg.unitSystem, unitSystems, 2);
            fieldSelect(html, "windunit", T(T_set_wind_speed_unit_metric_only), gCfg.windUnitMs, windUnits, 2);
            break;
        }
        case 5:
            html += "<p>"; html += T(gCfg.webAdminPass[0] ? T_wz_pw_set : T_wz_pw_intro); html += "</p>";
            fieldSecret(html, "webadminpw", T(T_set_admin_password), gCfg.webAdminPass[0], false, sizeof(gCfg.webAdminPass) - 1,
                        T(T_set_user_name_admin));
            break;
        case 6: {
            html += "<p>"; html += T(T_wz_outputs_intro); html += "</p>";
            const Option tlsModes[] = { { MQTT_TLS_NONE, T(T_opt_plain_no_tls) }, { MQTT_TLS_INSECURE, T(T_opt_tls_trust_any_certificate) },
                                        { MQTT_TLS_VERIFY, T(T_opt_tls_verify_against_ca_below) } };
            html += gCfg.mqttEn ? "<details open><summary>" : "<details><summary>"; html += T(T_set_mqtt_home_assistant); html += "</summary>";
            fieldCheck (html, "mqtten",   T(T_set_mqtt_enabled), gCfg.mqttEn);
            {   // wizard step 6: the broker fields only with MQTT on
                DepScope dep("mqtten=1");
                fieldText  (html, "mqtthost", T(T_set_broker_host), gCfg.mqttHost, sizeof(gCfg.mqttHost) - 1);
                fieldText  (html, "mqttport", T(T_set_broker_port), String(gCfg.mqttPort), 5, T(T_set_1883_plain_8883_tls), "number");
                fieldText  (html, "mqttuser", T(T_set_user_name), gCfg.mqttUser, sizeof(gCfg.mqttUser) - 1);
                fieldSecret(html, "mqttpass", T(T_set_password), gCfg.mqttPass[0], true, sizeof(gCfg.mqttPass) - 1);
                fieldSelect(html, "mqtttls",  T(T_set_security), gCfg.mqttTlsMode, tlsModes, 3);
            }
            html += "</details>";
            wxUploadLock();
            const bool wx = gCfg.awekasUser[0] || gCfg.wuStationId[0] || gCfg.wcloudId[0] || gCfg.wnetproId[0] || gCfg.windyId[0] || gCfg.osemBoxId[0];
            html += wx ? "<details open><summary>" : "<details><summary>"; html += T(T_set_weather_networks); html += "</summary>";
            fieldText  (html, "awkuser", T(T_set_awekas_user_name), gCfg.awekasUser, sizeof(gCfg.awekasUser) - 1);
            fieldSecret(html, "awkpass", T(T_set_awekas_password), gCfg.awekasPass[0], true, sizeof(gCfg.awekasPass) - 1);
            fieldText  (html, "wuid",    T(T_set_weather_underground_station_id), gCfg.wuStationId, sizeof(gCfg.wuStationId) - 1);
            fieldSecret(html, "wukey",   T(T_set_weather_underground_station_key), gCfg.wuStationKey[0], true, sizeof(gCfg.wuStationKey) - 1);
            fieldText  (html, "wcid",    T(T_set_weathercloud_id), gCfg.wcloudId, sizeof(gCfg.wcloudId) - 1);
            fieldSecret(html, "wckey",   T(T_set_weathercloud_key), gCfg.wcloudKey[0], true, sizeof(gCfg.wcloudKey) - 1);
            fieldText  (html, "wnpid",   T(T_set_wetternetzwerk_pro_station_id), gCfg.wnetproId, sizeof(gCfg.wnetproId) - 1);
            fieldSecret(html, "wnpkey",  T(T_set_wetternetzwerk_pro_station_key), gCfg.wnetproKey[0], true, sizeof(gCfg.wnetproKey) - 1);
            fieldText  (html, "windyid", T(T_set_windy_station_id), gCfg.windyId, sizeof(gCfg.windyId) - 1);
            fieldSecret(html, "windypw", T(T_set_windy_station_password), gCfg.windyPass[0], true, sizeof(gCfg.windyPass) - 1);
            fieldText  (html, "osemid",  T(T_set_opensensemap_sensebox_id), gCfg.osemBoxId, sizeof(gCfg.osemBoxId) - 1);
            fieldSecret(html, "osemtok", T(T_set_opensensemap_access_token), gCfg.osemToken[0], true, sizeof(gCfg.osemToken) - 1);
            fieldText  (html, "osemmap", T(T_set_opensensemap_sensor_ids), gCfg.osemSensors, sizeof(gCfg.osemSensors) - 1, T(T_set_osem_hint));
            wxUploadUnlock();
            html += "</details>";
            html += gCfg.loxEn ? "<details open><summary>" : "<details><summary>"; html += T(T_set_loxone_udp); html += "</summary>";
            fieldCheck(html, "loxen",   T(T_set_loxone_udp_output), gCfg.loxEn);
            loxoneLock();
            fieldText (html, "loxhost", T(T_set_miniserver_ip_or_host_name), gCfg.loxHost, sizeof(gCfg.loxHost) - 1);
            loxoneUnlock();
            fieldText (html, "loxport", T(T_set_udp_port), String(gCfg.loxPort), 5, T(T_set_1_65535_default_7000), "number");
            html += "</details>";
            break;
        }
        default: {   // 7: summary
            html += "<section><table>";
            String v;
            addRow(html, T(T_set_language), gCfg.uiLang ? String(langCode((uint8_t)(gCfg.uiLang - 1))) : String(T(T_set_lang_auto)));
            const size_t mine = rfParseIds(gCfg.rfIds).size();
            addRow(html, T(T_st_wireless_box), !gCfg.rfEn ? String(T(T_c_off)) : TF(T_wz_sum_mine, String((unsigned)mine)));
            v = String(); appendEscaped(v, gCfg.deviceName[0] ? gCfg.deviceName : "&ndash;");
            addRow(html, T(T_set_device_name), v);
            v = String(gCfg.altitudeM) + " m";
            if (gCfg.latitude[0]) { v += " &middot; "; appendEscaped(v, gCfg.latitude); v += ", "; appendEscaped(v, gCfg.longitude); }
            addRow(html, T(T_set_location), v);
            v = String(); appendEscaped(v, gCfg.timeZone);
            addRow(html, T(T_set_time_zone_posix_tz), v);
            addRow(html, T(T_set_units), T(gCfg.unitSystem ? T_opt_imperial_f_in_inhg_mph : T_opt_metric_c_mm_hpa));
            addRow(html, T(T_set_admin_password), gCfg.webAdminPass[0] ? String(T(T_wz_sum_set)) : String("<b>") + T(T_wz_sum_not_set) + "</b>");
            String out;
            if (gCfg.mqttEn) out += "MQTT";
            if (gCfg.awekasUser[0] || gCfg.wuStationId[0] || gCfg.wcloudId[0] || gCfg.wnetproId[0] || gCfg.windyId[0] || gCfg.osemBoxId[0])
                { if (out.length()) out += ", "; out += T(T_set_weather_networks); }
            if (gCfg.loxEn) { if (out.length()) out += ", "; out += "Loxone"; }
            addRow(html, T(T_wz_t_outputs), out.length() ? out : String("&ndash;"));
            html += "</table></section>";
            if (wizardRestartPending) { html += "<div class='banner okb'>"; html += T(T_wz_restart_note); html += "</div>"; }
            html += "<form method='POST' action='/setup/done'><input type='hidden' name='action' value='finish'><p class='wzn'>"
                    "<a href='/setup?step=6'>&larr; "; html += T(T_wz_back); html += "</a><button type='submit'>";
            html += T(T_wz_finish); html += "</button></p></form>";
            html += "<p class='dim'>"; html += T(T_wz_links); html += " <a href='/'>"; html += T(T_c_dashboard);
            html += "</a> &middot; <a href='/status'>"; html += T(T_c_status); html += "</a> &middot; <a href='/api/settings'>";
            html += T(T_c_settings); html += "</a></p>";
            break;
        }
    }
    if (step < WIZ_STEPS) {
        html += "</section>";
        wizardNav(html, step);
        html += "</form>";
    }
    // closes the wizard for good (also "later": it can be started again in Settings -> System)
    html += "<form method='POST' action='/setup/done'><input type='hidden' name='action' value='dismiss'>"
            "<p class='dim'><button class='sb' type='submit'>"; html += T(T_wz_later); html += "</button> ";
    html += T(T_wz_later_hint); html += "</p></form></body></html>";
    html.finish();
}

void handleSetupPage()
{
    selectLanguage();
    if (webBusyRefused()) return;   // tls_gate.h: an HTTPS request runs and the heap is low
    WebHeavyGuard heavy(millis());   // meanwhile no new HTTPS request starts
    if (!requireWizardAuth()) return;
    renderWizard(server.hasArg("step") ? server.arg("step").toInt() : 1, "", 200);
}

// POST /setup/done: action=finish (end of the wizard) or dismiss ("later").
// Both set the setup-done flag; finish restarts once if a step saved a
// restart field.
void handleSetupDone()
{
    selectLanguage();
    if (!requireWizardAuth() || !requireSameOrigin()) return;
    gCfg.setupDone = 1;
    saveConfig(gCfg);
    if (server.arg("action") == "finish" && wizardRestartPending) {
        HtmlOut html(server, 200);
        pageStart(html, T(T_wz_title), "/setup", 25);
        html += "<div class='banner okb'>"; html += T(T_wz_restarting); html += "</div></body></html>";
        html.finish();
        dbgSerial.println("Setup wizard finished - restarting to apply the network/MQTT settings.");
        restartAtMs = millis() + 1000;
        if (restartAtMs == 0) restartAtMs = 1;
        return;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleSettingsGet()
{
    selectLanguage();
    if (webBusyRefused()) return;   // tls_gate.h: an HTTPS request runs and the heap is low
    WebHeavyGuard heavy(millis());   // meanwhile no new HTTPS request starts
    if (!requireAuth()) return;
    renderSettings(server.hasArg("test") ? T(T_al_test_queued) : server.hasArg("saved") ? T(T_set_saved) : "", false, 200);
}

// --- POST helpers. `rejected` collects the labels of invalid fields: they
// keep their stored value, everything else is still applied. ---

void addRejected(String& rejected, const char* label)
{
    if (rejected.length()) rejected += ", ";
    rejected += label;
}

// Settings restore (handleSettingsRestore): the uploaded backup's
// "settings" object stands in for the submitted form, so it goes through
// exactly the same checks as a manual save. Null = the HTTP request.
JsonObjectConst gFormOverride;
bool formHas(const char* name) { return gFormOverride.isNull() ? server.hasArg(name) : !gFormOverride[name].isNull(); }
bool formHas(const String& name) { return formHas(name.c_str()); }
String formRaw(const char* name)
{
    if (gFormOverride.isNull()) return server.arg(name);
    JsonVariantConst v = gFormOverride[name];
    if (v.is<const char*>()) return v.as<const char*>();
    char b[32];
    backupValue(v, b, sizeof(b));   // a number or true / false typed into the file by hand
    return b;
}
String formRaw(const String& name) { return formRaw(name.c_str()); }

String formArg(const char* name)
{
    String v = formRaw(name);
    v.trim();
    return v;
}

bool isHex(const String& s, size_t len)
{
    if (s.length() != len) return false;
    for (size_t i = 0; i < len; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

// Plain text field; sets `changed` if the stored value differs.
// Only if the field is part of the request (a partial POST keeps the rest).
template <size_t N>
void storeText(char (&dst)[N], const char* name, bool& changed)
{
    if (!formHas(name)) return;
    String v = formArg(name);
    if (v.length() >= N) v.remove(N - 1);
    if (strcmp(dst, v.c_str()) != 0) { strlcpy(dst, v.c_str(), N); changed = true; }
}

// Write-only secret: empty keeps, "<name>_clear" (optional secrets) wipes.
template <size_t N>
void storeSecret(char (&dst)[N], const char* name, bool& changed)
{
    String clearName = String(name) + "_clear";
    if (formHas(clearName)) {
        if (dst[0]) { dst[0] = '\0'; changed = true; }
        return;
    }
    String v = formRaw(name);   // not trimmed: spaces may be part of a password
    if (v.isEmpty()) return;
    if (v.length() >= N) v.remove(N - 1);
    if (strcmp(dst, v.c_str()) != 0) { strlcpy(dst, v.c_str(), N); changed = true; }
}

void storeInt(int& dst, const char* name, long minV, long maxV, const char* label, String& rejected, bool& changed)
{
    if (!formHas(name)) return;
    String v = formArg(name);
    long n = v.toInt();
    if (v.isEmpty() || n < minV || n > maxV) { addRejected(rejected, label); return; }
    if (dst != n) { dst = (int)n; changed = true; }
}

// Decimal-degree coordinate or empty (= not set).
template <size_t N>
void storeCoord(char (&dst)[N], const char* name, float limit, const char* label, String& rejected)
{
    if (!formHas(name)) return;
    String v = formArg(name);
    if (v.length()) {
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (*end != '\0' || f < -limit || f > limit || v.length() >= N) { addRejected(rejected, label); return; }
    }
    strlcpy(dst, v.c_str(), N);
}


// Applies the "Alarms" tab fields present in the form (settings POST, restore, test button).
void applyAlarmForm(String& rejected)
{
    bool changed = false;
    alarmsLock();
    AlarmRule* rules = alarmsRules();
    char n[16];
    for (int i = 0; i < ALARM_RULES; i++) {
        auto nm = [&n, i](const char* x) { snprintf(n, sizeof(n), "a%d_%s", i, x); return (const char*)n; };
        if (!formHas(nm("src"))) continue;
        AlarmRule r = rules[i];
        const long src = formRaw(nm("src")).toInt();
        if (src < 0 || src >= AS_COUNT) { addRejected(rejected, T(T_al_value)); continue; }
        r.src = (uint8_t)src;
        if (formHas(nm("en")))   { String v = formRaw(nm("en")); r.enabled = v.length() && v != "0"; }
        if (formHas(nm("name"))) strlcpy(r.name, formRaw(nm("name")).c_str(), sizeof(r.name));
        if (formHas(nm("rff")))  { long f = formRaw(nm("rff")).toInt(); if (f >= 0 && f < AF_COUNT) r.rfField = (uint8_t)f; }
        String sel = formHas(nm("rfsel")) ? formRaw(nm("rfsel")) : String();
        if (sel.length()) {   // a known sensor chosen from the list
            r.rfId = (uint32_t)strtoul(sel.c_str(), nullptr, 16);
        } else if (formHas(nm("rfid"))) {
            String id = formRaw(nm("rfid")); id.trim();
            char* end = nullptr;
            unsigned long v = strtoul(id.c_str(), &end, 16);
            if (id.length() && (*end || id.length() > 8)) addRejected(rejected, T(T_al_sensor_id));
            else r.rfId = v;
        }
        if (formHas(nm("cmp")))  r.above = formRaw(nm("cmp")).toInt() ? 1 : 0;
        const PhKind k = alarmKind(r);
        if (formHas(nm("raise"))) r.raise = alarmFromDisplay(k, formRaw(nm("raise")).toFloat(), gCfg);
        if (formHas(nm("clear"))) r.clear = alarmFromDisplay(k, formRaw(nm("clear")).toFloat(), gCfg);
        if (formHas(nm("dur")))  { long v = formRaw(nm("dur")).toInt(); if (v >= 0 && v <= 1440) r.minDurationMin = (uint16_t)v; else addRejected(rejected, T(T_al_dur)); }
        if (formHas(nm("rep")))  { long v = formRaw(nm("rep")).toInt(); if (v >= 0 && v <= 1440) r.repeatMin = (uint16_t)v; else addRejected(rejected, T(T_al_rep)); }
        if (formHas(nm("prio"))) { long v = formRaw(nm("prio")).toInt(); if (v >= 1 && v <= 5) r.priority = (uint8_t)v; }
        for (int c = 0; c < AC_COUNT; c++) {
            char cn[8];
            snprintf(cn, sizeof(cn), "ch%d", c);
            if (!formHas(nm(cn))) continue;
            String v = formRaw(nm(cn));
            if (v.length() && v != "0") r.channels |= 1 << c; else r.channels &= ~(1 << c);
        }
        if (formHas(nm("clr")))  { String v = formRaw(nm("clr")); r.allClear = v.length() && v != "0"; }
        if (formHas(nm("msg")))  strlcpy(r.msg, formRaw(nm("msg")).c_str(), sizeof(r.msg));
        if (formHas(nm("cmsg"))) strlcpy(r.clearMsg, formRaw(nm("cmsg")).c_str(), sizeof(r.clearMsg));
        if (r.src == AS_RF && !r.rfId) { addRejected(rejected, T(T_al_sensor_id)); r.enabled = 0; }
        if (r.src == AS_RF && r.rfId) {   // a sensor in range: only a measurement it provides (alarms.h)
            for (const RfSensorReading& x : gRfSensors) {
                if (!x.used || x.id != r.rfId) continue;
                const uint16_t cap = alarmCapMask(x.hasTemp, x.hasHumidity, x.hasWind, x.hasUv, x.hasLight, x.hasLeak, x.hasLightning,
                                                  x.hasMoisture, x.hasCo2, x.hasPm25);
                if (!alarmFieldAvailable(r.rfField, cap)) { addRejected(rejected, T(T_al_sensor_value)); r.rfField = rules[i].rfField; }
            }
        }
        if (memcmp(&r, &rules[i], sizeof(r)) != 0) {
            const bool cond = r.src != rules[i].src || r.rfId != rules[i].rfId || r.rfField != rules[i].rfField;
            rules[i] = r;
            if (cond) alarmsResetState(i);
            changed = true;
        }
    }
    if (formHas("altpl")) {   // a template into the first free slot
        const long t = formRaw("altpl").toInt();
        if (t >= 0 && t < ALARM_TEMPLATES) {
            int i = 0;
            while (i < ALARM_RULES && rules[i].src != AS_OFF) i++;
            if (i < ALARM_RULES && alarmTemplate((int)t, rules[i])) { alarmsResetState(i); changed = true; }
            else if (i == ALARM_RULES) addRejected(rejected, T(T_al_tpl));
        }
    }
    AlarmChannels& ch = alarmsChannels();
    auto text = [&changed](char* dst, size_t len, const char* name) {
        if (!formHas(name)) return;
        String v = formRaw(name); v.trim();
        if (strcmp(dst, v.c_str()) != 0) { strlcpy(dst, v.c_str(), len); changed = true; }
    };
    auto secret = [&changed](char* dst, size_t len, const char* name) {
        if (formHas(String(name) + "_clear")) { if (dst[0]) { dst[0] = '\0'; changed = true; } return; }
        String v = formRaw(name);
        if (v.length() && strcmp(dst, v.c_str()) != 0) { strlcpy(dst, v.c_str(), len); changed = true; }
    };
    text(ch.ntfyUrl, sizeof(ch.ntfyUrl), "nturl");
    if (!ch.ntfyUrl[0]) strcpy(ch.ntfyUrl, "https://ntfy.sh");
    text(ch.ntfyTopic, sizeof(ch.ntfyTopic), "nttopic");
    secret(ch.ntfyToken, sizeof(ch.ntfyToken), "nttok");
    text(ch.ntfyTags, sizeof(ch.ntfyTags), "nttags");
    secret(ch.poToken, sizeof(ch.poToken), "potok");
    secret(ch.poUser, sizeof(ch.poUser), "pouser");
    if (formHas("poretry")) { long v = formRaw("poretry").toInt(); if (v >= 30 && v <= 10800) { if (ch.poRetrySec != v) changed = true; ch.poRetrySec = (uint16_t)v; } else addRejected(rejected, T(T_al_po_retry)); }
    if (formHas("poexp"))   { long v = formRaw("poexp").toInt(); if (v >= 60 && v <= 10800) { if (ch.poExpireSec != v) changed = true; ch.poExpireSec = (uint16_t)v; } else addRejected(rejected, T(T_al_po_expire)); }
    secret(ch.tgToken, sizeof(ch.tgToken), "tgtok");
    text(ch.tgChat, sizeof(ch.tgChat), "tgchat");
    if (formHas("almqtt")) { String v = formRaw("almqtt"); const uint8_t on = v.length() && v != "0"; if (on != ch.mqttEn) { ch.mqttEn = on; changed = true; } }
    if (formHas("whurl")) {
        String v = formRaw("whurl"); v.trim();
        if (v.length() && !v.startsWith("http://") && !v.startsWith("https://")) addRejected(rejected, T(T_al_wh_url));
        else text(ch.whUrl, sizeof(ch.whUrl), "whurl");
    }
    if (formHas("whpost")) { const uint8_t p = formRaw("whpost").toInt() ? 1 : 0; if (p != ch.whPost) { ch.whPost = p; changed = true; } }
    if (formHas("whbody")) { String v = formRaw("whbody"); if (strcmp(ch.whBody, v.c_str())) { strlcpy(ch.whBody, v.c_str(), sizeof(ch.whBody)); changed = true; } }
    if (changed) alarmsSave();
    alarmsUnlock();
}

void applyCustomUploadForm(String& rejected)
{
    bool changed = false;
    customUploadLock();
    char n[12];
    for (int i = 0; i < CUSTOM_UPLOADS; i++) {
        CustomUpload& u = customUploads()[i];
        auto nm = [&n, i](const char* x) { snprintf(n, sizeof(n), "cu%d%s", i, x); return (const char*)n; };
        if (formHas(nm("url"))) {
            String v = formRaw(nm("url")); v.trim();
            if (v.length() && !v.startsWith("http://") && !v.startsWith("https://")) addRejected(rejected, TF(T_cu_url, String(i + 1)).c_str());
            else if (strcmp(u.url, v.c_str())) { strlcpy(u.url, v.c_str(), sizeof(u.url)); changed = true; }
        }
        if (formHas(nm("post"))) { const uint8_t p = formRaw(nm("post")).toInt() ? 1 : 0; if (p != u.post) { u.post = p; changed = true; } }
        if (formHas(nm("body"))) { String v = formRaw(nm("body")); if (strcmp(u.body, v.c_str())) { strlcpy(u.body, v.c_str(), sizeof(u.body)); changed = true; } }
        if (formHas(nm("int"))) {
            long v = formRaw(nm("int")).toInt();
            if (v < 60 || v > 3600) addRejected(rejected, T(T_cu_int));
            else if (v != u.intervalSec) { u.intervalSec = (uint16_t)v; changed = true; }
        }
    }
    if (changed) customUploadSave();
    customUploadUnlock();
}

void handleSettingsPost()
{
    selectLanguage();
    if (webBusyRefused()) return;   // tls_gate.h: an HTTPS request runs and the heap is low
    WebHeavyGuard heavy(millis());   // meanwhile no new HTTPS request starts
    // wizard=1: a step of the setup wizard (open while setup_wizard.h allows it)
    const bool wizard = formHas("wizard");
    const int  wstep  = formRaw("wstep").toInt();
    if (!(wizard ? requireWizardAuth() : requireAuth()) || !requireSameOrigin()) return;
    if (!formHas("form")) {
        renderSettings(T(T_set_incomplete_form_submission_nothing_w), true, 400);
        return;
    }

    // Only fields present in the request are changed, so a partial POST
    // (curl, scripts) leaves everything else alone. Checkboxes: the page
    // sends a hidden "0" after each one (fieldCheck), so "unchecked" is
    // distinguishable from "not part of the request".
    String rejected;
    auto present = [](const char* name) { return formHas(name); };
    auto checked = [](const char* name) -> long { String v = formRaw(name); return (v.length() && v != "0") ? 1 : 0; };
    auto apply = [&rejected](uint8_t id, long value, const char* label) {
        if (!applyParam(id, clampToParamValue(value))) addRejected(rejected, label);
    };
    auto applyNum = [&](uint8_t id, const char* name, const char* label) {
        if (present(name)) apply(id, formArg(name).toInt(), label);
    };
    auto applyCheck = [&](uint8_t id, const char* name, const char* label) {
        if (present(name)) apply(id, checked(name), label);
    };
    auto scaled1000 = [](const char* name) -> long {
        String text = formArg(name);
        char* end = nullptr;
        float value = strtof(text.c_str(), &end);
        if (text.isEmpty() || *end != '\0' || !isfinite(value) ||
            value < -32.768f || value > 32.767f) {
            return -32768;
        }
        return lroundf(value * 1000.0f);
    };

    // ---- live: operational parameters (uplink before sample interval:
    // the sample interval is validated against the uplink interval) ----
    applyNum(PARAM_UPLINK_INTERVAL_SEC,      "uplinksec",   T(T_set_measurement_interval_5_3600_s));
    applyNum(PARAM_SAMPLE_SEC,               "samplesec",   T(T_set_wind_sample_interval_1_or));
    applyNum(PARAM_MQTT_INTERVAL_SEC,        "mqttsec",     T(T_set_mqtt_interval_0_or_2));
    applyNum(PARAM_LORA_WIND_AGG,            "lorawagg",    T(T_set_lorawan_wind_values));
    applyNum(PARAM_MQTT_WIND_AGG,            "mqttwagg",    T(T_set_mqtt_wind_values));
    applyCheck(PARAM_LOX_EN,                 "loxen",       T(T_set_loxone_udp_output));
    applyNum(PARAM_LOX_INTERVAL_SEC,         "loxsec",      T(T_set_loxone_interval_0_or_2));
    applyCheck(PARAM_RF_EN,                  "rfen",        T(T_set_wireless_sensors2));
    applyNum(PARAM_RF_PROTO,                 "rfproto",     T(T_set_wireless_sensor_type2));
    {
        static const char* const names[SG_COUNT] = { "srcair", "srcwind", "srcrain", "srcuv", "srclight", "srcpress" };
        for (int g = 0; g < SG_COUNT; g++)
            if (formArg(names[g]).length())
                applyNum((uint8_t)srcParamId((SrcGroup)g), names[g], T(T_set_value_source));
    }
    applyCheck(PARAM_WIND_EN,                "winden",      T(T_set_wind_sensor));
    applyCheck(PARAM_RAIN_EN,                "rainen",      T(T_set_rain_sensor));
    applyCheck(PARAM_RAIN_PERSIST,           "rainpersist", T(T_set_rain_statistics_in_flash));
    applyCheck(PARAM_LIPO_EN,                "lipoen",      T(T_set_lipo_measurement));
    applyCheck(PARAM_DEBUG_MODE,             "debugmode",   T(T_set_debug_mode2));
    // Only when changed: applyParam() reprograms the CPU clock unconditionally.
    if (present("cpufreq") && formArg("cpufreq").toInt() != gCfg.cpuFreqMhz)
        applyNum(PARAM_CPU_FREQ_MHZ,             "cpufreq",     T(T_set_cpu_frequency));
    applyNum(PARAM_UNIT_SYSTEM,              "unitsys",     T(T_set_unit_system2));
    applyNum(PARAM_WIND_UNIT_MS,             "windunit",    T(T_set_wind_speed_unit));
    applyNum(PARAM_WIND_DIR_OFFSET_DEG,      "windoff",     T(T_set_wind_direction_offset_180_to));
    applyNum(PARAM_WIND_DIR_SENSOR_TYPE,     "winddirtyp",  T(T_set_wind_direction_sensor2));
    applyNum(PARAM_PAYLOAD_FORMAT,           "payfmt",      T(T_set_payload_format2));
    if (ioV2()) {   // the section is only rendered on v2.0
        auto x10 = [](const char* name) -> long { return lroundf(formArg(name).toFloat() * 10.0f); };
        // 2nd wind sensor first: it takes over the AUX inputs it uses, the
        // AUX checkboxes then only apply to the inputs that are still free.
        applyNum(PARAM_WIND2_MODE,               "wind2",       T(T_set_2nd_wind_sensor));
        applyNum(PARAM_WIND2_DIR_OFFSET_DEG,     "wind2off",    T(T_set_2nd_wind_vane_offset_180));
        if (gCfg.wind2Mode != 2)
            applyCheck(PARAM_AUX_AIN_EN,             "auxainen",    T(T_set_aux_analog_input));
        if (present("auxainscale")) apply(PARAM_AUX_AIN_SCALE_X1000, lroundf(formArg("auxainscale").toFloat() * 1000.0f), T(T_set_aux_analog_scale_0_001));
        if (!gCfg.wind2Mode)
            applyCheck(PARAM_AUX_PULSE_EN,           "auxpulseen",  T(T_set_aux_pulse_counter));
        applyNum(PARAM_HEATER_MODE,              "heatmode",    T(T_set_heater_mode));
        applyNum(PARAM_HEATER_DUTY_PCT,          "heatduty",    T(T_set_rej_heater_power));
        if (present("heatbelow")) apply(PARAM_HEATER_ON_BELOW_DC,  x10("heatbelow"),             T(T_set_heater_threshold_40_to_40));
        applyNum(PARAM_HEATER_MIN_DUTY_PCT,      "heatminduty", T(T_set_rej_heater_power_at));
        if (present("heatfull")) apply(PARAM_HEATER_FULL_BELOW_DC, x10("heatfull"),             T(T_set_heater_full_power_temperature_40));
        applyNum(PARAM_FAN_MODE,                 "fanmode",     T(T_set_fan_mode));
        applyNum(PARAM_FAN_DUTY_PCT,             "fanduty",     T(T_set_rej_fan_speed));
        if (present("fanabove")) apply(PARAM_FAN_ON_ABOVE_DC,     x10("fanabove"),              T(T_set_fan_threshold_40_to_60));
        applyNum(PARAM_FAN_MIN_PV_W,             "fanminpv",    T(T_set_fan_minimum_pv_power_0));
        applyCheck(PARAM_TACHO_EN,               "tachoen",     T(T_set_fan_tacho));
        applyNum(PARAM_TACHO_PPR,                "tachoppr",    T(T_set_tacho_pulses_per_revolution_1));
        applyCheck(PARAM_VEDIRECT_EN,            "vedirecten",  T(T_set_ve_direct));
        if (present("outminbatt")) apply(PARAM_OUT_MIN_BATT_DV,     x10("outminbatt"),            T(T_set_minimum_battery_voltage_0_60));
        applyCheck(PARAM_UV_EN,                  "uven",        T(T_set_uv_sensor));
        if (present("uvscale")) apply(PARAM_UV_AIN_SCALE_X1000,  scaled1000("uvscale"),
                                                                        T(T_set_uv_divider_scale_1_5));
        applyCheck(PARAM_UV_INDEX_EN,            "uvindexen",   T(T_set_uv_index_enable));
        if (present("uvslope")) apply(PARAM_UV_INDEX_SLOPE_X1000,scaled1000("uvslope"),
                                                                        T(T_set_uv_index_slope_0_30));
        if (present("uvoffset")) apply(PARAM_UV_INDEX_OFFSET_X1000,scaled1000("uvoffset"),
                                                                        T(T_set_uv_index_offset_10_to));
        applyCheck(PARAM_UV_CALIBRATED,          "uvcal",       T(T_set_uv_calibration_flag));
    }

    // Calibration factors directly as float — applyParam()'s x1000 integer
    // encoding (made for the 2-byte downlink value) would round 1.00584 to 1.006.
    if (present("rainmmtip")) {
        float rainTip = formArg("rainmmtip").toFloat();
        if (rainTip > 0.0f && rainTip <= 5.0f) gCfg.rainMmPerTip = rainTip;
        else addRejected(rejected, T(T_set_rain_per_tip_0_5));
    }
    if (present("windmspp")) {
        float windPps = formArg("windmspp").toFloat();
        if (windPps > 0.0f && windPps <= 10.0f) gCfg.windMsPerPps = windPps;
        else addRejected(rejected, T(T_set_wind_speed_per_pulse_s2));
    }

    // main-value calibration (calibration.h): offsets in tenths, factors in thousandths
    auto tenths = [](const char* name) -> long { return lroundf(formArg(name).toFloat() * 10.0f); };
    if (present("caltemp"))  apply(PARAM_CAL_TEMP_DC,     tenths("caltemp"),     T(T_set_cal_temp));
    if (present("calhum"))   apply(PARAM_CAL_HUM_DPCT,    tenths("calhum"),      T(T_set_cal_hum));
    if (present("calpress")) apply(PARAM_CAL_PRESS_DHPA,  tenths("calpress"),    T(T_set_cal_press));
    if (present("calrain"))  apply(PARAM_CAL_RAIN_PERMIL, scaled1000("calrain"), T(T_set_cal_rain));
    if (present("calwind"))  apply(PARAM_CAL_WIND_PERMIL, scaled1000("calwind"), T(T_set_cal_wind));
    if (present("rfcal")) {
        char list[sizeof(gCfg.rfCal)];
        if (rfCalParseText(formArg("rfcal").c_str(), list, sizeof(list))) strlcpy(gCfg.rfCal, list, sizeof(gCfg.rfCal));
        else addRejected(rejected, T(T_set_cal_rf));
    }
    applyNum(PARAM_SUN_METHOD, "sunmethod", T(T_set_sun_method));
    {   // built-in OLED (live)
        bool dummy = false;
        int v;
        v = gCfg.dispMode;      storeInt(v, "dispmode", 0, 2, T(T_set_disp_mode), rejected, dummy);   gCfg.dispMode = v;
        v = gCfg.dispMinutes;   storeInt(v, "dispmin", 1, 120, T(T_set_disp_min), rejected, dummy);   gCfg.dispMinutes = v;
        v = gCfg.dispRotateSec; storeInt(v, "disprot", 2, 60, T(T_set_disp_rot), rejected, dummy);    gCfg.dispRotateSec = v;
        v = gCfg.dispContrast;  storeInt(v, "dispcon", 1, 255, T(T_set_disp_con), rejected, dummy);   gCfg.dispContrast = v;
        v = gCfg.dispNightFrom; storeInt(v, "dispnf", 0, 23, T(T_set_disp_nf), rejected, dummy);      gCfg.dispNightFrom = v;
        v = gCfg.dispNightTo;   storeInt(v, "dispnt", 0, 23, T(T_set_disp_nt), rejected, dummy);      gCfg.dispNightTo = v;
        if (present("dsp0")) {
            int mask = 0;
            for (int k = 0; k < OS_COUNT; k++) { char n[8]; snprintf(n, sizeof(n), "dsp%d", k); if (checked(n)) mask |= 1 << k; }
            gCfg.dispScreens = mask ? mask : 1;   // none ticked: the classic screen
        }
    }

    bool ignored = false;   // "changed" sink for live fields (no restart needed)
    int altitude = gCfg.altitudeM;
    storeInt(altitude, "altitude", 0, 4000, T(T_set_altitude_0_4000_m), rejected, ignored);
    gCfg.altitudeM = altitude;

    // ---- live: weather networks, time, admin password ----
    bool timeChanged = false;
    wxUploadLock();
    storeCoord(gCfg.latitude,  "lat", 90.0f,  "latitude", rejected);
    storeCoord(gCfg.longitude, "lon", 180.0f, "longitude", rejected);
    storeText  (gCfg.awekasUser,   "awkuser", ignored);
    storeSecret(gCfg.awekasPass,   "awkpass", ignored);
    storeInt   (gCfg.awekasIntervalSec, "awkint", 300, 3600, T(T_set_awekas_interval_300_3600_s), rejected, ignored);
    storeText  (gCfg.wuStationId,  "wuid",    ignored);
    storeSecret(gCfg.wuStationKey, "wukey",   ignored);
    storeInt   (gCfg.wuIntervalSec, "wuint", 60, 3600, T(T_set_weather_underground_interval_60_3600), rejected, ignored);
    storeText  (gCfg.wcloudId,     "wcid",    ignored);
    storeSecret(gCfg.wcloudKey,    "wckey",   ignored);
    storeInt   (gCfg.wcloudIntervalSec, "wcint", 60, 3600, T(T_set_weathercloud_interval_60_3600_s), rejected, ignored);
    storeText  (gCfg.wnetproId,    "wnpid",   ignored);
    storeSecret(gCfg.wnetproKey,   "wnpkey",  ignored);
    storeInt   (gCfg.wnetproIntervalSec, "wnpint", 60, 3600, T(T_set_wetternetzwerk_pro_interval_60_3600), rejected, ignored);
    storeText  (gCfg.windyId,      "windyid", ignored);
    storeSecret(gCfg.windyPass,    "windypw", ignored);
    storeInt   (gCfg.windyIntervalSec, "windyint", 300, 3600, T(T_set_windy_interval_300_3600_s), rejected, ignored);
    storeText  (gCfg.osemBoxId,    "osemid",  ignored);
    storeSecret(gCfg.osemToken,    "osemtok", ignored);
    storeText  (gCfg.osemSensors,  "osemmap", ignored);
    storeInt   (gCfg.osemIntervalSec, "osemint", 60, 3600, T(T_set_opensensemap_interval_60_3600_s), rejected, ignored);
    storeText  (gCfg.wowSiteId,    "wowid",   ignored);
    storeSecret(gCfg.wowPin,       "wowpin",  ignored);
    storeInt   (gCfg.wowIntervalSec, "wowint", 300, 3600, T(T_set_wow_int), rejected, ignored);
    if (present("cwopcall")) {   // CWOP ID or callsign: A-Z 0-9 and "-", stored upper case
        String v = formArg("cwopcall"); v.toUpperCase();
        bool okCall = v.length() <= 9;
        for (size_t k = 0; k < v.length(); k++) if (!isalnum((unsigned char)v[k]) && v[k] != '-') okCall = false;
        if (okCall) strlcpy(gCfg.cwopCall, v.c_str(), sizeof(gCfg.cwopCall));
        else addRejected(rejected, T(T_set_cwop_call));
    }
    storeInt   (gCfg.cwopPasscode,  "cwoppass", -1, 32767, T(T_set_cwop_pass), rejected, ignored);
    storeInt   (gCfg.cwopIntervalSec, "cwopint", 300, 3600, T(T_set_cwop_int), rejected, ignored);
    applyAlarmForm(rejected);
    applyCustomUploadForm(rejected);
    if (present("updchk")) gCfg.updateCheckEn = checked("updchk");
    if (formArg("updurl").startsWith("http")) storeText(gCfg.updateUrl, "updurl", ignored);
    else if (present("updurl") && gCfg.updateCheckEn) addRejected(rejected, T(T_set_version_list_url_http_https));
    if (present("tz")) {
        if (formArg("tz").isEmpty()) addRejected(rejected, T(T_set_time_zone));
        else                         storeText(gCfg.timeZone, "tz", timeChanged);
    }
    if (present("ntp")) {
        if (formArg("ntp").isEmpty()) addRejected(rejected, T(T_set_ntp_server));
        else                          storeText(gCfg.ntpServer, "ntp", timeChanged);
    }
    wxUploadUnlock();
    if (timeChanged) timeSyncReconfigure();
    // Not clearable: an empty admin password would lock this page out.
    storeSecret(gCfg.webAdminPass, "webadminpw", ignored);
    // Wireless sensor states (rf_sensors.cpp picks up changes on its next loop):
    // the text lists (advanced) first, then the table's selects - each only
    // if changed against the state the page showed (rfso_*), so an edit of
    // the text field is not undone by the unchanged selects - then a move.
    storeText(gCfg.rfIds, "rfids", ignored);
    storeText(gCfg.rfIgnore, "rfignore", ignored);
    if (present("rfnames") && formArg("rfnames") != gCfg.rfNames) {   // the names as text (also: settings restore)
        String all = formArg("rfnames");
        char tmp[sizeof(gCfg.rfNames)] = "";
        bool okNames = true;
        for (int p = 0; p < (int)all.length();) {   // "ID:name|ID:name", each entry cleaned
            int bar = all.indexOf('|', p);
            if (bar < 0) bar = all.length();
            String e = all.substring(p, bar);
            p = bar + 1;
            int c = e.indexOf(':');
            if (c <= 0) continue;
            char* end = nullptr;
            const uint32_t nid = (uint32_t)strtoul(e.substring(0, c).c_str(), &end, 16);
            if (!nid || *end) { okNames = false; continue; }
            if (!rfNameSet(tmp, sizeof(tmp), nid, e.substring(c + 1).c_str())) okNames = false;
        }
        if (okNames) strlcpy(gCfg.rfNames, tmp, sizeof(gCfg.rfNames));
        else addRejected(rejected, T(T_set_names_as_text));
    }
    for (int i = 0; i < server.args(); i++) {   // a name typed into the sensor table
        String n = server.argName(i);
        if (!n.startsWith("rfnm_")) continue;
        const uint32_t nid = (uint32_t)strtoul(n.c_str() + 5, nullptr, 16);
        String v = server.arg(i);
        if (!nid || v == rfNameOf(nid)) continue;
        if (!rfNameSet(gCfg.rfNames, sizeof(gCfg.rfNames), nid, v.c_str())) addRejected(rejected, T(T_set_th_name));
    }
    applyNum(PARAM_UI_LANG,                  "uilang",      T(T_set_language));
    applyCheck(PARAM_RF_ONLY_MINE,           "rfonly",      T(T_set_only_my_sensors2));
    for (int i = 0; i < server.args(); i++) {
        String n = server.argName(i);
        if (!n.startsWith("rfst_")) continue;
        String id = n.substring(5), v = server.arg(i);
        if (v == formRaw("rfso_" + id)) continue;
        RfSensorState st = v == "m" ? RF_STATE_MINE : v == "h" ? RF_STATE_HIDDEN : RF_STATE_RECEIVED;
        if (!rfSetState((uint32_t)strtoul(id.c_str(), nullptr, 16), st)) addRejected(rejected, T(T_set_wireless_sensor_list_max_32));
    }
    if (present("rfmove")) {
        String m = formArg("rfmove");
        int c = m.indexOf(':');
        if (c > 0) rfMoveMine((uint32_t)strtoul(m.substring(0, c).c_str(), nullptr, 16), m.substring(c + 1).toInt());
    }
    {   // Davis (diagnostics): live, davis_rx.cpp compares the settings every loop
        auto liveInt = [&](volatile int& field, const char* name, long lo, long hi, const char* label) {
            int v = field;
            storeInt(v, name, lo, hi, label, rejected, ignored);
            field = v;
        };
        liveInt(gCfg.davisTxId,     "dvid",     0, 8,   T(T_set_davis_transmitter_id_0_8));
        liveInt(gCfg.davisModel,    "dvmodel",  0, 1,   T(T_set_davis_station));
        liveInt(gCfg.davisBucket,   "dvbucket", 0, 1,   T(T_set_davis_rain_collector));
        liveInt(gCfg.davisScanKhz,  "dvscan",   0, 60,  T(T_set_davis_offset_scan_0_60));
        liveInt(gCfg.davisWindowMs, "dvwin",    5, 500, T(T_set_davis_rx_window_5_500));
        liveInt(gCfg.davisGuardMs,  "dvguard",  5, 500, T(T_set_davis_guard_5_500_ms));
        liveInt(gCfg.davisRainMaxPkt, "dvrpkt", 1, 127, T(T_set_dv_rain_pkt));
        liveInt(gCfg.davisRainMaxMin, "dvrmin", 1, 127, T(T_set_dv_rain_min));
        if (present("dvrfilt")) gCfg.davisRainFilter = checked("dvrfilt");
        liveInt(gCfg.davisTable,    "dvtable",  0, 1,   T(T_set_davis_channel_table));
        liveInt(gCfg.davisRawCh,    "dvrawch",  0, 4,   T(T_set_davis_raw_capture_channel_0));
        if (present("dvbw")) {
            int bw = formArg("dvbw").toInt();
            if (davisRxBwValid(bw)) gCfg.davisRxBw = bw; else addRejected(rejected, T(T_set_davis_receiver_bandwidth));
        }
        if (present("dvoff")) {
            String t = formArg("dvoff");
            char* end = nullptr;
            float khz = strtof(t.c_str(), &end);
            if (t.length() && *end == '\0' && isfinite(khz) && khz >= -100.0f && khz <= 100.0f) gCfg.davisOffsetHz = (int)lroundf(khz * 1000.0f);
            else addRejected(rejected, T(T_set_davis_frequency_offset_100_to));
        }
        if (present("dvafc"))   gCfg.davisAfc = (int)checked("dvafc");
        if (present("dvsniff")) gCfg.davisSniffer = (int)checked("dvsniff");
        if (present("dvraw"))   gCfg.davisRaw = (int)checked("dvraw");
    }
    {   // Loxone: live, read by the main loop under the same lock
        int loxPort = gCfg.loxPort;
        storeInt(loxPort, "loxport", 1, 65535, T(T_set_loxone_udp_port_1_65535), rejected, ignored);
        gCfg.loxPort = loxPort;
        loxoneLock();
        storeText(gCfg.loxHost, "loxhost", ignored);
        if (present("loxprefix")) {
            String pre = formArg("loxprefix");
            if (loxPrefixValid(pre.c_str())) storeText(gCfg.loxPrefix, "loxprefix", ignored);
            else addRejected(rejected, T(T_set_loxone_name_prefix_max_16));
        }
        loxoneUnlock();
    }

    // ---- restart: only read at boot. Collected into a copy (static: Config
    // is several KB, too big for this task's stack), so the running
    // transports never see a half-applied change before the restart. ----
    applyCheck(PARAM_LOW_POWER_EN,           "lowpower",    T(T_set_low_power_mode2));
    applyCheck(PARAM_VANE_POWER_SAVE,        "vanesave",    T(T_set_wind_vane_power_saving));
    // heap while saving (~5 KB), not static RAM
    std::unique_ptr<Config> nextBuf(new (std::nothrow) Config(gCfg));
    if (!nextBuf) { renderSettings(T(T_tr_err_alloc), true, 503); return; }
    Config& next = *nextBuf;
    bool restart = false;
    if (present("loraen")) {
        int loraEn = (int)checked("loraen");
        if (next.loraEn != loraEn) { next.loraEn = loraEn; restart = true; }
    }
    String eui = formArg("deveui");
    if (isHex(eui, 16)) { uint64_t v = hexToUint64(eui.c_str()); if (v != next.devEui) { next.devEui = v; restart = true; } }
    else if (present("deveui")) addRejected(rejected, T(T_set_deveui_16_hex));
    eui = formArg("joineui");
    if (isHex(eui, 16)) { uint64_t v = hexToUint64(eui.c_str()); if (v != next.joinEui) { next.joinEui = v; restart = true; } }
    else if (present("joineui")) addRejected(rejected, T(T_set_joineui_16_hex));
    String key = formArg("appkey");
    if (key.length()) {
        uint8_t k[16];
        if (isHex(key, 32)) {
            hexToBytes(key.c_str(), k, sizeof(k));
            if (memcmp(k, next.appKey, sizeof(k)) != 0) { memcpy(next.appKey, k, sizeof(k)); restart = true; }
        } else {
            addRejected(rejected, T(T_set_appkey_32_hex));
        }
    }
    String region = formArg("region");
    if (isSupportedRegion(region)) storeText(next.region, "region", restart);
    else if (present("region")) addRejected(rejected, "region");

    storeText(next.deviceName, "devname", restart);
    int staticIpEn = present("staticipen") ? (int)checked("staticipen") : next.staticIpEn;
    auto argOr = [](const char* name, const char* stored) { return formHas(name) ? formArg(name) : String(stored); };
    String ip = argOr("staticip", next.staticIp), gw = argOr("staticgw", next.staticGateway),
           sn = argOr("staticsn", next.staticSubnet), dns = argOr("staticdns", next.staticDns);
    {   // net_check.h: never refused while off or unchanged; an empty address = DHCP
        const bool changed = staticIpEn != next.staticIpEn || ip != next.staticIp || gw != next.staticGateway ||
                             sn != next.staticSubnet || dns != next.staticDns;
        bool en = staticIpEn != 0;
        const StaticIpOutcome o = staticIpCheck(en, ip.c_str(), gw.c_str(), sn.c_str(), dns.c_str(), changed, en);
        if (o == SIP_INVALID) {
            addRejected(rejected, T(T_set_static_ip_invalid_address));
        } else if (o == SIP_APPLY) {
            if (next.staticIpEn != (int)en) { next.staticIpEn = en; restart = true; }
            storeText(next.staticIp,      "staticip",  restart);
            storeText(next.staticGateway, "staticgw",  restart);
            storeText(next.staticSubnet,  "staticsn",  restart);
            storeText(next.staticDns,     "staticdns", restart);
        }
    }
    int mqttEn     = present("mqtten")     ? (int)checked("mqtten")     : next.mqttEn;
    int mqttCtrlEn = present("mqttctrlen") ? (int)checked("mqttctrlen") : next.mqttCtrlEn;
    if (next.mqttEn != mqttEn)         { next.mqttEn = mqttEn; restart = true; }
    if (next.mqttCtrlEn != mqttCtrlEn) { next.mqttCtrlEn = mqttCtrlEn; restart = true; }
    storeText  (next.mqttHost,  "mqtthost",  restart);
    storeInt   (next.mqttPort,  "mqttport", 1, 65535, T(T_set_mqtt_port), rejected, restart);
    storeText  (next.mqttUser,  "mqttuser",  restart);
    storeSecret(next.mqttPass,  "mqttpass",  restart);
    storeText  (next.mqttTopic, "mqtttopic", restart);
    storeInt   (next.mqttTlsMode, "mqtttls", MQTT_TLS_NONE, MQTT_TLS_VERIFY, T(T_set_mqtt_security), rejected, restart);
    if (formRaw("mqttcacert").length() >= sizeof(next.mqttCaCert)) addRejected(rejected, T(T_set_ca_certificate_too_long));
    else storeText(next.mqttCaCert, "mqttcacert", restart);
    int board = present("board") ? formArg("board").toInt() : next.boardSel;
    if (!boardSelValid(board)) addRejected(rejected, T(T_set_heltec_board));
    else if (next.boardSel != board) { next.boardSel = board; restart = true; }
    int pcb = present("pcbver") ? formArg("pcbver").toInt() : next.pcbVersion;
    if (gIo.hwIdDetected && pcb != PCB_REV_2_0) addRejected(rejected, T(T_set_pcb_revision_the_board_identifies));
    else if (pcb == PCB_REV_1_2 || pcb == PCB_REV_1_3 || pcb == PCB_REV_2_0) { if (next.pcbVersion != pcb) { next.pcbVersion = pcb; restart = true; } }
    else addRejected(rejected, T(T_set_pcb_revision));
    {   // Wi-Fi / web interface off: the node then answers only the config
        // portal and LoRaWAN downlinks - so only while LoRaWAN is (and stays)
        // on and joined, and with the confirmation (not in a backup).
        const int wifiEn   = present("wifien")   ? (int)checked("wifien")   : next.wifiEn;
        const int webApiEn = present("webapien") ? (int)checked("webapien") : next.webApiEn;
        const bool off = (next.wifiEn && !wifiEn) || (next.webApiEn && !webApiEn);
        switch (radioOffCheck(off, checked("radiooffok"), next.loraEn, loraReady())) {
            case RADIO_OFF_APPLY:
                next.wifiEn = wifiEn; next.webApiEn = webApiEn; restart = true;
                dbgSerial.printf("Web settings: WiFi %s, web interface %s after the restart\n", wifiEn ? "on" : "off", webApiEn ? "on" : "off");
                break;
            case RADIO_OFF_NOT_CONFIRMED: addRejected(rejected, T(T_set_radio_off_refused_confirm)); break;
            case RADIO_OFF_NO_LORAWAN:    addRejected(rejected, T(T_set_radio_off_refused_lora)); break;
            case RADIO_OFF_NONE:          break;
        }
    }

    String rejectedMsg = rejected.length() ? TF(T_set_invalid_kept, rejected) : String();

    // ---- WiFi network: tested with rollback (wifi_mqtt.cpp), not a restart field ----
    // Another network (SSID) only together with DHCP: a static IP belongs to
    // the current network - kept for the new one it would put the node into
    // the wrong subnet. Switching the static IP off in the same submission
    // is fine (the test then runs with DHCP, the restart applies it).
    bool wifiChange = false;
    bool noTest     = checked("wifinotest");
    if (present("wifissid")) {
        String ssid    = formRaw("wifissid");   // not trimmed: spaces may be part of an SSID
        String pass    = formRaw("wifipass");
        bool   clear   = formHas("wifipass_clear");
        String curSsid = WiFi.SSID(), curPass = wifiCurrentPassword();
        String newPass = clear ? String() : (pass.length() ? pass : curPass);
        bool otherNetwork = ssid != curSsid;
        if (ssid.length() && gCfg.wifiEn && !wifiChangeBusy() && (otherNetwork || newPass != curPass)) {
            if (otherNetwork && next.staticIpEn)
                addRejected(rejected, T(T_set_wifi_network_a_static_ip));
            else if (wifiRequestChange(ssid.c_str(), newPass.c_str(), !noTest, next.staticIpEn == 0)) wifiChange = true;
            else addRejected(rejected, T(T_set_wifi_name_1_32_characters));
        } else if (ssid.isEmpty()) {
            addRejected(rejected, T(T_set_wifi_network_empty));
        }
    }
    if (rejected.length()) rejectedMsg = TF(T_set_invalid_kept, rejected);

    if (wifiChange) {
        if (restart) { saveConfig(next); restartAtMs = millis() + 1000; if (restartAtMs == 0) restartAtMs = 1; }
        else         saveConfig(gCfg);
        HtmlOut html(server, 200);
        pageStart(html, T(T_c_settings), "/api/settings", 75);
        html += "<div class='banner okb'>";
        if (noTest) html += T(restart ? T_set_wifi_notest_restart : T_set_wifi_notest);
        else        html += T(restart ? T_set_wifi_test_restart : T_set_wifi_test);
        html += "</div>";
        // rejectedMsg is built from fixed labels only (already HTML, e.g. &deg;)
        if (rejectedMsg.length()) { html += "<div class='banner badb'>"; html += rejectedMsg; html += "</div>"; }
        html += "</body></html>";
        html.finish();
        dbgSerial.println("Web settings: WiFi network change requested");
        return;
    }

    if (restart && wizard) {
        // saved now, applied by one restart at the wizard's "Finish"; the
        // following steps start from these values
        saveConfig(next);
        gCfg = next;
        wizardRestartPending = true;
        restart = false;
    }
    if (restart) {
        saveConfig(next);
        HtmlOut html(server, 200);
        pageStart(html, T(T_c_settings), "/api/settings", 20);
        html += "<div class='banner okb'>"; html += T(T_set_saved_restarting); html += "</div>";
        if (gRestoreReport.length()) { html += "<div class='banner okb'>"; html += gRestoreReport; html += "</div>"; gRestoreReport = ""; }
        if (rejectedMsg.length()) { html += "<div class='banner badb'>"; html += rejectedMsg; html += "</div>"; }
        html += "</body></html>";
        html.finish();
        dbgSerial.println("Web settings: restart-requiring change saved, restarting...");
        restartAtMs = millis() + 1000;
        if (restartAtMs == 0) restartAtMs = 1;
        return;
    }

    saveConfig(gCfg);
    if (wizard) {   // back to the same step with the problems, else on to the next
        if (rejectedMsg.length()) { renderWizard(wstep, rejectedMsg, 400); return; }
        // "Search again" (step 2) stays on the step
        server.sendHeader("Location", "/setup?step=" + String(wizardClampStep(wstep + (server.hasArg("wstay") ? 0 : 1))));
        server.send(303);
        return;
    }
    if (rejectedMsg.length()) {
        renderSettings(TF(T_set_saved_except, rejectedMsg), true, 400);
        return;
    }
    server.sendHeader("Location", "/api/settings?saved=1");
    server.send(303);
}

// ---- Settings backup / restore (docs/settings.md "Backup and restore",
// settings_backup.h) ----
// The backup holds every field of the settings page; write-only fields only
// on request. Restoring sends it through handleSettingsPost() - same
// validation, same restart rules; LoRaWAN session and DevNonces are not in it.
void captureSettings(JsonObject o, bool secrets)
{
    gCapture = o;
    gCaptureSecrets = secrets;
    renderSettings("", false, 200);   // into nothing: only the field values are recorded
    gCapture = JsonObject();
    gCaptureSecrets = false;
}

void handleSettingsBackup()
{
    selectLanguage();
    if (webBusyRefused()) return;   // tls_gate.h: an HTTPS request runs and the heap is low
    WebHeavyGuard heavy(millis());   // meanwhile no new HTTPS request starts
    if (!requireAuth() || !requireSameOrigin()) return;
    const bool secrets = server.arg("secrets") == "1";
    char host[HOSTNAME_MAX_LEN + 1];
    deviceHostname(host, sizeof(host));
    char date[12] = "undated";
    struct tm lt;
    if (localTimeNow(lt)) strftime(date, sizeof(date), "%Y-%m-%d", &lt);
    JsonDocument doc;
    backupHeader(doc, FIRMWARE_VERSION, host, date, secrets);
    captureSettings(doc["settings"].to<JsonObject>(), secrets);
    if (doc.overflowed()) { server.send(500, "text/plain", "JSON allocation failed"); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition", String("attachment; filename=\"uwn-settings-") + host + "-" + date + ".json\"");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    ChunkedPrint out(server);
    serializeJsonPretty(doc, out);
    out.flush();
}

void handleSettingsRestore()
{
    selectLanguage();
    if (webBusyRefused()) return;   // tls_gate.h: an HTTPS request runs and the heap is low
    WebHeavyGuard heavy(millis());   // meanwhile no new HTTPS request starts
    if (!requireAuth() || !requireSameOrigin()) return;
    JsonDocument in;
    const BackupCheck chk = deserializeJson(in, server.arg("backup")) ? BK_INVALID : backupCheck(in);
    if (chk != BK_OK) {
        renderSettings(chk == BK_NEWER ? TF(T_set_bk_newer, String(in["version"] | 0)) : String(T(T_set_bk_invalid)), true, 400);
        return;
    }
    // what changes, against the settings as they are now
    JsonDocument cur, changes;
    captureSettings(cur.to<JsonObject>(), false);
    JsonObject set = in["settings"].as<JsonObject>();
    const int n = backupPrepare(set, cur.as<JsonObjectConst>(), changes.to<JsonArray>(),
                                [](const char* key, const char* v) { return secretValue(key) == v; });
    String list;
    for (JsonObjectConst c : changes.as<JsonArrayConst>()) {
        if (list.length()) list += ", ";
        list += c["key"].as<const char*>();
        if (c["secret"]) { list += " (***)"; continue; }
        list += ": &ldquo;"; appendEscaped(list, c["old"] | ""); list += "&rdquo; &rarr; &ldquo;";
        appendEscaped(list, c["new"] | ""); list += "&rdquo;";
    }
    set["form"] = "1";
    gRestoreReport = n ? TF(T_set_bk_changed, String(n)) + " " + list : String(T(T_set_bk_nochange));
    gFormOverride = set;
    handleSettingsPost();   // validates, saves, restarts if a restart field changed
    gFormOverride = JsonObjectConst();
}

// Firmware upload from the settings page (System tab), POST /api/ota:
// multipart field "firmware". Same streaming handler as the config portal
// (ota_upload.h: application image, or the -factory image with this node's
// partition table; anything else is refused before a byte is written).
// Only with an admin password (Digest) and from the node's own pages
// (same-origin). ?js=1 (the page's XHR with a progress bar): plain-text
// answer; without JavaScript the browser posts the form and gets a page.
// Success: restart after the answer (systemRestart() saves the rain
// statistics and records).
bool otaWebAuthorized()
{
    return gCfg.webAdminPass[0] && server.authenticate("admin", gCfg.webAdminPass) && sameOriginRequest();
}

void handleOtaChunk()
{
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        const bool ok = otaWebAuthorized();
        if (ok && rfTraceRunning()) rfTraceStop("firmware upload");   // its buffer and the radio log back off
        otaUploadStart(otaEspOps(), ok, "Web OTA");
    }
    else if (up.status == UPLOAD_FILE_WRITE)   otaUploadChunk(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END)     otaUploadFinish();
    else if (up.status == UPLOAD_FILE_ABORTED) otaUploadAbort();
}

void handleOtaDone()
{
    selectLanguage();
    const OtaOutcome o = otaUploadOutcome();
    otaUploadReset();
    if (!gCfg.webAdminPass[0]) {   // never without a password: the portal (own AP, button) is the fallback
        server.send(403, "text/html; charset=utf-8", T(T_ota_need_password));   // the text carries entities
        return;
    }
    if (!requireAuth() || !requireSameOrigin()) return;
    const bool js = server.hasArg("js");
    String msg = o == OTA_OUT_OK      ? String(T(T_ota_ok))
               : o == OTA_OUT_REFUSED ? TF(T_ota_refused, otaUploadError())
               : o == OTA_OUT_FAILED  ? TF(T_ota_failed, otaUploadError())
                                      : String(T(T_ota_no_file));
    const int code = o == OTA_OUT_OK ? 200 : 400;
    if (js) {
        server.send(code, "text/plain; charset=utf-8", msg);
    } else {
        HtmlOut html(server, code);
        pageStart(html, T(T_ota_title), "/api/settings");
        html += "<section><h4>"; html += T(T_ota_title); html += "</h4><p><b>"; appendEscaped(html, msg.c_str()); html += "</b></p>";
        if (o == OTA_OUT_OK) {
            // no JavaScript: the meta refresh opens the status page once the node is back
            html += "<meta http-equiv='refresh' content='30;url=/status'><p>"; html += T(T_ota_reconnect); html += "</p>";
        } else {
            html += "<p><a href='/api/settings#system'>"; html += T(T_ota_back); html += "</a></p>";
        }
        html += "</section></body></html>";
        html.finish();
    }
    if (o == OTA_OUT_OK) {
        restartReason = "firmware update (web)";
        restartAtMs = millis() + 1000;
        if (restartAtMs == 0) restartAtMs = 1;
    }
}

// "Send test" buttons of the Alarms tab: the tab's fields are applied (so an
// unsaved channel works), then one test message is queued for the channel.
void handleAlarmTest()
{
    selectLanguage();
    if (!requireAuth() || !requireSameOrigin()) return;
    String rejected;
    applyAlarmForm(rejected);
    alarmsTest((uint8_t)server.arg("testch").toInt());
    server.sendHeader("Location", "/api/settings?test=1#alarms");
    server.send(303);
}

// The Alarms tab's own form (form='aform').
void handleAlarmSave()
{
    selectLanguage();
    if (!requireAuth() || !requireSameOrigin()) return;
    String rejected;
    applyAlarmForm(rejected);
    if (rejected.length()) { renderSettings(TF(T_set_saved_except, rejected), true, 400); return; }
    server.sendHeader("Location", "/api/settings?saved=1#alarms");
    server.send(303);
}

// GET /api/alarms (docs/api/schemas/alarms.schema.json): rules and state, no secrets.
void handleApiAlarms()
{
    std::unique_ptr<AlarmRule[]> rules(new (std::nothrow) AlarmRule[ALARM_RULES]);
    std::unique_ptr<AlarmState[]> st(new (std::nothrow) AlarmState[ALARM_RULES]);
    std::unique_ptr<AlarmChannels> ch(new (std::nothrow) AlarmChannels());
    if (!rules || !st || !ch) { server.send(500, "text/plain", "JSON allocation failed"); return; }
    alarmsSnapshot(rules.get(), st.get());
    alarmsLock();
    *ch = alarmsChannels();
    alarmsUnlock();
    JsonDocument doc;
    alarmsJson(doc, rules.get(), st.get(), *ch, gCfg, millis() / 1000);
    sendJsonDoc(doc, "alarms");
}

// mDNS (docs/api.md#discovery-mdns): <hostname>.local plus _http._tcp and
// _weathernode._tcp on port 80, TXT api=<API_VERSION>, path=/api/info.
// Started together with the web server, i.e. only once Wi-Fi is up; the
// IDF mDNS responder then follows Wi-Fi reconnects by itself. The host
// name is fixed until the next restart (the device name is a restart field).
// A failed start is retried every 30 s by webApiTask.
bool          mdnsStarted = false;
unsigned long mdnsTriedMs = 0;

void mdnsStart()
{
    if (mdnsStarted) return;
    mdnsTriedMs = millis();
    char host[HOSTNAME_MAX_LEN + 1];
    deviceHostname(host, sizeof(host));   // = the DHCP hostname (wifi_mqtt.cpp)
    if (!MDNS.begin(host)) {
        dbgSerial.println("mDNS: start failed.");
        return;
    }
    char ver[8];
    snprintf(ver, sizeof(ver), "%d", API_VERSION);
    const char* name = gCfg.deviceName[0] ? gCfg.deviceName : host;
    MDNS.setInstanceName(name);
    for (const char* svc : { "http", "weathernode" }) {
        MDNS.addService(svc, "tcp", 80);
        MDNS.addServiceTxt(svc, "tcp", "api", ver);
        MDNS.addServiceTxt(svc, "tcp", "path", "/api/info");
    }
    mdnsStarted = true;
    dbgSerial.printf("mDNS: http://%s.local/ (_http._tcp, _weathernode._tcp)\n", host);
}

void mdnsStop()
{
    if (!mdnsStarted) return;
    MDNS.end();
    mdnsStarted = false;
}

// Runs continuously on its own FreeRTOS task (core 0) once webApiSetup()
// spawns it — this is what used to be webApiPoll(), called once per
// main-loop() iteration on core 1 alongside sensor sampling and the
// (blocking) sendLoRa(). Moving it here means an incoming HTTP request no
// longer has to wait behind LoRa airtime; the tradeoff (cross-task access
// to `server`/history.cpp's ring buffer/Config's volatile int fields) is
// handled by serverMutex here, historyMutex in history.cpp, and `volatile`
// on the relevant Config fields, respectively — see the comments at each.
void webApiTask(void*)
{
    for (;;) {
        xSemaphoreTake(serverMutex, portMAX_DELAY);
        if (!gCfg.wifiEn || !gCfg.webApiEn) {
            if (serverStarted) {
                server.stop();
                serverStarted = false;
                mdnsStop();
            }
        } else if (!serverStarted && !apiStopped && wifiIsConnected()) {
            server.begin();
            serverStarted = true;
            dbgSerial.println("Web API started on port 80 (/, /status, /api/data, /api/info, /api/history, /api/settings, /help).");
            mdnsStart();
        }
        if (serverStarted && !mdnsStarted && millis() - mdnsTriedMs >= 30000 && wifiIsConnected()) mdnsStart();
        if (serverStarted) server.handleClient();
        xSemaphoreGive(serverMutex);

        // Deferred restart after a restart-requiring settings save (see
        // handleSettingsPost()) — the response has been sent by now.
        // A WiFi change test runs first (it would be cut short otherwise).
        if (restartAtMs && (long)(millis() - restartAtMs) >= 0 && !wifiChangeBusy()) systemRestart(restartReason);

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

} // namespace

// Unknown path: a short page in the visitor's language with the links.
void handleNotFound()
{
    selectLanguage();
    server.send(404, "text/plain; charset=utf-8", TF(T_c_not_found, server.uri()));
}

// The portal's decoder download and wind-direction calibration
// (portal_pages.h). The decoder is public, as in the portal (no secrets);
// the calibration is a setting: admin login, same-origin for the save, and
// only on a board with a vane input.
void handleDecoder()
{
    portalSendDecoder(server);
}

bool calibrateAllowed()
{
    selectLanguage();
    if (!boardPcbAllowed()) { handleNotFound(); return false; }
    return requireAuth();
}

void handleCalibratePage()
{
    if (calibrateAllowed()) portalSendCalibratePage(server);
}

void handleCalibrateSample()
{
    if (calibrateAllowed()) portalSendCalibrateSample(server);
}

void handleCalibrateSave()
{
    if (!calibrateAllowed() || !requireSameOrigin()) return;
    if (portalHandleCalibrateSave(server)) dbgSerial.printf("Web: wind-direction calibration saved (%d positions)\n", gCfg.windDirCalCount);
}

void webApiSetup()
{
    server.on("/",             HTTP_GET,  handleChartPage);
    server.on("/status",       HTTP_GET,  handleStatusPage);
    server.on("/chart",        HTTP_GET,  handleChartRedirect);
    server.on("/api/data",     HTTP_GET,  handleApiData);
    server.on("/api/info",     HTTP_GET,  handleApiInfo);
    server.on("/api/records",  HTTP_GET,  handleApiRecords);
    server.on("/manifest.webmanifest", HTTP_GET, handleManifest);
    server.on("/sw.js",                HTTP_GET, handleServiceWorker);
    server.on("/icon-192.png",         HTTP_GET, handleIcon192);
    server.on("/icon-512.png",         HTTP_GET, handleIcon512);
    server.on("/icon-maskable-512.png", HTTP_GET, handleIconMaskable);
    server.on("/apple-touch-icon.png", HTTP_GET, handleAppleTouch);
    server.on("/apple-touch-icon-precomposed.png", HTTP_GET, handleAppleTouch);
    server.on("/favicon-32.png",       HTTP_GET, handleFavicon);
    server.on("/favicon.ico",          HTTP_GET, handleFavicon);
    server.on("/api/alarms",   HTTP_GET,  handleApiAlarms);
    server.on("/api/alarms/test", HTTP_POST, handleAlarmTest);
    server.on("/api/alarms/save", HTTP_POST, handleAlarmSave);
    for (const char* uri : { "/api/data", "/api/info", "/api/history", "/api/records", "/api/alarms" })
        server.on(uri, HTTP_OPTIONS, handlePublicApiOptions);
    server.on("/api/settings", HTTP_GET,  handleSettingsGet);
    server.on("/api/settings", HTTP_POST, handleSettingsPost);
    server.on("/api/settings/backup",  HTTP_GET,  handleSettingsBackup);
    server.on("/api/settings/rfsensors", HTTP_GET, handleRfSensorTable);
    server.on("/api/settings/reset", HTTP_POST, handleSettingsReset);
    server.on("/api/settings/restore", HTTP_POST, handleSettingsRestore);
    server.on("/api/ota",      HTTP_POST, handleOtaDone, handleOtaChunk);
    server.on("/api/log",       HTTP_GET,  handleLogPage);
    // Origin/Referer for the CSRF check (Authorization is always collected)
    // and Cookie / Accept-Language for the page language (selectLanguage())
    static const char* headerKeys[] = { "Origin", "Referer", "Cookie", "Accept-Language", "Accept-Encoding" };
    server.collectHeaders(headerKeys, 5);
    server.on("/api/log/raw",   HTTP_GET,  handleLogRaw);
    server.on("/api/log/clear", HTTP_POST, handleLogClear);
    server.on("/api/davis/log", HTTP_GET,  handleDavisLog);
    server.on("/api/rf/trace",  HTTP_GET,  handleRfTraceGet);
    server.on("/api/rf/trace",  HTTP_POST, handleRfTracePost);
    server.on("/api/rf/sensor", HTTP_POST, handleRfSensorPost);
    server.on("/api/history",        HTTP_GET,  handleApiHistory);
    server.on("/help",         HTTP_GET,  handleHelp);
    server.on("/help.md",      HTTP_GET,  handleHelpMd);
    server.on("/setup",        HTTP_GET,  handleSetupPage);
    server.on("/setup/done",   HTTP_POST, handleSetupDone);
    server.on("/decoder",      HTTP_GET,  handleDecoder);
    server.on("/calibrate",        HTTP_GET,  handleCalibratePage);
    server.on("/calibrate/sample", HTTP_GET,  handleCalibrateSample);
    server.on("/calibrate/save",   HTTP_POST, handleCalibrateSave);
    server.onNotFound(handleNotFound);

    // Priority 1 == the main loop task's own priority (Arduino's default
    // "loopTask") — neither task should be able to starve the other for
    // this workload. 8 KB stack covers WebServer's own request parsing plus
    // our handlers' JSON/String building (matches Arduino's default loop
    // task stack size).
    xTaskCreatePinnedToCore(webApiTask, "webApi", 8192, nullptr, 1, &webApiTaskHandle, 0);
}

void webApiStop()
{
    // serverMutex's initializer runs as part of this file's static
    // construction, before any function in it — including this one — can be
    // called from elsewhere, so it's always valid here even though
    // webApiSetup() (which spawns webApiTask) may not have run yet. That
    // matters because main.cpp documents this exact function as being
    // called on the first-boot portal path before webApiSetup() is reached.
    xSemaphoreTake(serverMutex, portMAX_DELAY);
    apiStopped = true;
    if (serverStarted) {
        server.stop();
        serverStarted = false;
        mdnsStop();
        dbgSerial.println("Web API stopped (port 80 freed for the config portal).");
    }
    xSemaphoreGive(serverMutex);

    // Stopping the server once isn't enough on its own anymore: webApiTask
    // keeps polling every ~5 ms on its own core and would just call
    // server.begin() again the next time it sees wifiEn/webApiEn still set
    // and WiFi still connected (plausible during the portal in AP+STA mode)
    // — reopening exactly the port-80 conflict with WiFiManager's own
    // captive-portal server that this function exists to prevent. Every
    // caller of webApiStop() (button long-press, first boot, downlink 0x03
    // — all via main.cpp's openPortalSaveAndRestart()) heads into
    // ESP.restart() regardless of outcome, so a one-way suspend (never
    // resumed) is correct here, not just a "pause". Guard against a null
    // handle: on the first-boot path this can run before webApiSetup() has
    // created the task at all, and vTaskSuspend(NULL) means "suspend the
    // *calling* task", not "no-op" — passing it unguarded here would freeze
    // the main task instead of the (nonexistent) web task.
    if (webApiTaskHandle) vTaskSuspend(webApiTaskHandle);
}
