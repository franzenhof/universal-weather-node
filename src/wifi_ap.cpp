#include "wifi_ap.h"
#include "globals.h"
#include "psram.h"
#include "serial_log.h"
#include "time_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <string.h>

namespace {

// Main loop writes, web task / OLED read: the strings under apMux.
portMUX_TYPE  apMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool active = false;
volatile uint8_t clients = 0;
char          runSsid[AP_SSID_MAX + 1] = "";   // what the running AP was started with
char          runPass[AP_PASS_MAX + 1] = "";
static_assert(sizeof(Config::apSsid) == AP_SSID_MAX + 1 && sizeof(Config::apPass) == AP_PASS_MAX + 1,
              "WIFI_QR_TEXT_MAX is sized for these fields");
int           runChanCfg = -1;
ApFallback    fallback;
bool          passWarned = false;
unsigned long startFailMs = 0;
bool          startFailed = false;
unsigned long clientsMs = 0;

// captive DNS (web task only)
int           dnsSock = -1;
uint8_t*      dnsBuf = nullptr;   // 2 x 512 bytes: query, answer - PSRAM if there
unsigned long dnsTriedMs = 0;

uint8_t effectiveMode()
{
    return gCfg.wifiEn && gCfg.webApiEn ? (uint8_t)gCfg.apMode : (uint8_t)AP_OFF;
}

void effectiveSsid(char (&out)[33])
{
    if (gCfg.apSsid[0]) { strlcpy(out, gCfg.apSsid, sizeof(out)); return; }
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // = the MAC the hostname / MQTT id use
    apDefaultSsid(mac, out, sizeof(out));
}

void startAp(bool staUp)
{
    char ssid[33];
    effectiveSsid(ssid);
    int ch = gCfg.apChannel;
    if (ch < 1 || ch > 13) ch = staUp ? (int)WiFi.channel() : 1;   // auto: the station's channel (AP+STA share the radio)
    if (ch < 1 || ch > 13) ch = 1;
    const size_t before = internalFree();
    const IPAddress ip(AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
    bool ok = WiFi.softAP(ssid, gCfg.apPass, ch, 0, AP_MAX_CLIENTS);
    // DHCP from .2, DNS = the node (the captive DNS below answers every name)
    ok = ok && WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0), IPAddress(AP_IP[0], AP_IP[1], AP_IP[2], 2), ip);
    if (!ok) {
        dbgSerial.println("Access point: start failed - retry in 30 s.");
        WiFi.softAPdisconnect(!(WiFi.getMode() & WIFI_MODE_STA));
        startFailed = true;
        startFailMs = millis();
        return;
    }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
    WiFi.AP.enableDhcpCaptivePortal();   // DHCP option 114 (RFC 8910): Android 11+ / iOS 14+ open http://192.168.4.1
#endif
    portENTER_CRITICAL(&apMux);
    strlcpy(runSsid, ssid, sizeof(runSsid));
    strlcpy(runPass, gCfg.apPass, sizeof(runPass));
    portEXIT_CRITICAL(&apMux);
    runChanCfg = gCfg.apChannel;
    startFailed = false;
    clients = 0;
    clientsMs = millis();
    active = true;
    dbgSerial.printf("Access point '%s' on channel %d: http://%s/ (internal RAM free %u -> %u bytes)\n",
                     ssid, ch, AP_IP_TEXT, (unsigned)before, (unsigned)internalFree());
}

void stopAp(const char* why)
{
    active = false;
    clients = 0;
    // with the station on, only the AP part goes; without it the radio stops
    WiFi.softAPdisconnect(!(WiFi.getMode() & WIFI_MODE_STA));
    dbgSerial.printf("Access point closed (%s).\n", why);
}

bool runningConfigDiffers()
{
    char ssid[33];
    effectiveSsid(ssid);
    portENTER_CRITICAL(&apMux);
    const bool diff = strcmp(ssid, runSsid) != 0 || strcmp(gCfg.apPass, runPass) != 0;
    portEXIT_CRITICAL(&apMux);
    return diff || gCfg.apChannel != runChanCfg;
}

} // namespace

void apPoll(bool staUp)
{
    const uint8_t mode = effectiveMode();
    bool want = fallback.update(mode, staUp, uptimeMs64());
    if (want && !apPassValid(gCfg.apPass)) {
        if (!passWarned) dbgSerial.println("Access point: no valid password (8-63 characters) - not started.");
        passWarned = true;
        want = false;
    } else {
        passWarned = false;
    }
    if (active && want && runningConfigDiffers()) stopAp("settings changed");
    if (want && !active) {
        if (!startFailed || millis() - startFailMs >= 30000) startAp(staUp);
    } else if (!want && active) {
        stopAp(mode == AP_FALLBACK ? "Wi-Fi network back" : "mode off");
    }
    if (active && millis() - clientsMs >= 2000) {
        clientsMs = millis();
        clients = WiFi.softAPgetStationNum();
    }
}

bool apStationUsed()
{
    if (effectiveMode() != AP_ALWAYS) return true;
    wifi_config_t c;
    // not started yet (the AP starts first in this mode): assume one
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return true;
    return apStationWanted(AP_ALWAYS, c.sta.ssid[0] != 0);
}

bool apIsActive() { return active; }

void apStatus(ApStatus& out)
{
    out = ApStatus();
    out.mode = effectiveMode();
    out.active = active;
    out.passInvalid = out.mode != AP_OFF && !apPassValid(gCfg.apPass);
    out.clients = clients;
    if (active) {
        out.channel = (uint8_t)WiFi.channel();
        portENTER_CRITICAL(&apMux);
        strlcpy(out.ssid, runSsid, sizeof(out.ssid));
        portEXIT_CRITICAL(&apMux);
    } else {
        char s[33];
        effectiveSsid(s);
        strlcpy(out.ssid, s, sizeof(out.ssid));
    }
}

bool apQrText(char* out, size_t cap)
{
    if (effectiveMode() == AP_OFF) return false;
    char ssid[AP_SSID_MAX + 1], pass[AP_PASS_MAX + 1];
    if (active) {
        portENTER_CRITICAL(&apMux);
        strlcpy(ssid, runSsid, sizeof(ssid));
        strlcpy(pass, runPass, sizeof(pass));
        portEXIT_CRITICAL(&apMux);
    } else {
        effectiveSsid(ssid);
        strlcpy(pass, gCfg.apPass, sizeof(pass));
    }
    return apPassValid(pass) && wifiQrText(ssid, pass, out, cap) > 0;
}

void apDnsService(bool run)
{
    if (!run || !active) {
        if (dnsSock >= 0) { close(dnsSock); dnsSock = -1; }
        return;
    }
    if (dnsSock < 0) {
        if (dnsTriedMs && millis() - dnsTriedMs < 5000) return;
        dnsTriedMs = millis();
        if (!dnsBuf) {
            dnsBuf = (uint8_t*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!dnsBuf) dnsBuf = (uint8_t*)malloc(1024);
            if (!dnsBuf) return;
        }
        const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) return;
        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(53);
        memcpy(&a.sin_addr.s_addr, AP_IP, 4);   // the AP's address only: the station LAN is not answered
        if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
            close(s);
            dbgSerial.println("Access point: DNS port 53 not free - retry in 5 s.");
            return;
        }
        dnsSock = s;
        dnsTriedMs = 0;
    }
    for (int i = 0; i < 16; i++) {   // what has arrived since the last pass, never blocking
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(dnsSock, dnsBuf, 512, MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
        if (n <= 0) break;
        const size_t m = dnsCaptiveAnswer(dnsBuf, (size_t)n, AP_IP, dnsBuf + 512, 512);
        if (m) sendto(dnsSock, dnsBuf + 512, m, MSG_DONTWAIT, (struct sockaddr*)&from, fl);
    }
}
