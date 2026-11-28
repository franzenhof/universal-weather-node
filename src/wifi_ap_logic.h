#pragma once
// Own Wi-Fi access point (Settings -> Network, docs/settings.md#own-access-point):
// the pure parts, host-tested in test/firmware_wifiap.cpp. The glue (softAP,
// the DNS socket, status) is wifi_ap.cpp.
//
// Modes: off / fallback (the station network has been unreachable for 2 min
// -> own AP, the station keeps being tried (AP+STA), the AP closes once the
// station has been back for 1 min) / always (the AP always runs; the station
// is used alongside it when a station network is stored).
#include <stddef.h>
#include <stdint.h>
#include "time_source.h"
#include "qr_code.h"

enum ApMode : uint8_t { AP_OFF = 0, AP_FALLBACK = 1, AP_ALWAYS = 2 };

// = CONFIG_LWIP_DHCPS_MAX_STATION_NUM: the DHCP server has 8 leases
constexpr int AP_MAX_CLIENTS = 8;
// AP subnet: 192.168.4.1/24, DHCP from .2
constexpr uint8_t AP_IP[4] = { 192, 168, 4, 1 };
constexpr const char* AP_IP_TEXT = "192.168.4.1";

const char* apModeName(int mode);   // "off", "fallback", "always" (/api/info)

// WPA2 passphrase: 8-63 printable ASCII characters. The AP never starts
// without a valid one (no open AP).
bool apPassValid(const char* pass);
// "WeatherNode-" + the last 4 hex digits (upper case) of the MAC
void apDefaultSsid(const uint8_t mac[6], char* out, size_t len);
// The Wi-Fi join text of a QR code: WIFI:T:WPA;S:<ssid>;P:<pass>;; with
// \ ; , : " escaped by a backslash. Returns its length, 0 = did not fit.
// WIFI_QR_TEXT_MAX holds the longest one (every character escaped) + NUL.
constexpr size_t AP_SSID_MAX = 32, AP_PASS_MAX = 63;
constexpr size_t WIFI_QR_TEXT_MAX = (sizeof("WIFI:T:WPA;S:") - 1) + 2 * AP_SSID_MAX + (sizeof(";P:") - 1) + 2 * AP_PASS_MAX
                                    + (sizeof(";;") - 1) + 1;
size_t wifiQrText(const char* ssid, const char* pass, char* out, size_t cap);

// Fallback timing (mode AP_FALLBACK): AP on after the station has been down
// for DOWN_MS (counted from the start or from the last time it was up), off
// after it has been up again for UP_MS. update() returns whether the AP
// should run; nowMs = uptime.
struct ApFallback {
    static constexpr uint64_t DOWN_MS = 120000;
    static constexpr uint64_t UP_MS   = 60000;
    bool active = false, started = false, staUp = false;
    uint64_t sinceMs = 0;   // the station's state holds since then
    bool update(uint8_t mode, bool staUpNow, uint64_t nowMs);
};
// false = no station at all: mode always and no station network stored
bool apStationWanted(uint8_t mode, bool staConfigured);
// Station retry interval while the AP runs: a connect attempt scans the
// channels and takes the AP off its channel for a moment, so it is tried
// every 2 min instead of every 30 s.
constexpr unsigned long AP_STA_RETRY_MS = 120000;

// ---- phones on the AP: captive-portal checks ----
// A connectivity-check path of Android, Apple, Windows, Firefox ...
bool captiveProbePath(const char* path);
// Request from an AP client: true = answer with a redirect to
// http://192.168.4.1/ (a foreign host name - every name resolves to the
// node - or a connectivity-check path). host = Host header (may carry a
// port), hostname = the node's host name (without .local).
bool captiveRedirect(const char* host, const char* path, const char* hostname);

// Captive DNS: the answer to query q (n bytes) - every A (and ANY) question
// gets ip, TTL 30 s; other types an empty answer (NOERROR). Returns the
// answer's length, 0 = drop (no standard query, malformed, cap too small).
size_t dnsCaptiveAnswer(const uint8_t* q, size_t n, const uint8_t ip[4], uint8_t* out, size_t cap);

// ---- time from phones (POST /api/time) ----
enum PhoneTimeVerdict : uint8_t {
    PT_OFFER,     // hand to the time sources (TSRC_PHONE; they still block it while a better source is fresh)
    PT_REFUSED,   // from the station LAN while the node has a fresh time
    PT_INVALID    // before 2024 or after 2099
};
// onAp = the request came in over the own AP; bestFresh = best fresh time source.
PhoneTimeVerdict phoneTimeCheck(int64_t unixMs, bool onAp, TimeSource bestFresh);

// ---- QR code on the OLED (screen OS_QR) ----
// Draws q into the left 64 x 64 pixels of buf (SSD1306 order: byte
// x + 128 * page, bit = row): lit background, dark modules, at the largest
// whole scale that leaves at least one module of quiet zone. Returns the
// scale (pixels per module), 0 = no code.
int qrOledRender(const QrCode& q, uint8_t buf[1024]);
// SVG of q (viewBox in modules with a 2-module quiet zone, one path):
// emit() is called with consecutive pieces of the text.
void qrSvg(const QrCode& q, void (*emit)(const char* s, void* arg), void* arg);
