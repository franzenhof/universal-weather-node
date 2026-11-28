#pragma once
// Own Wi-Fi access point (Settings -> Network -> Own access point,
// docs/settings.md#own-access-point): softAP 192.168.4.1/24 with DHCP
// (DNS and the captive-portal URI of RFC 8910 offered), a captive DNS that
// answers every name with the node, the state for the status page, /api/info
// and the OLED QR screen. Modes and the pure rules: wifi_ap_logic.h.
// No Arduino types here: api_info.cpp (host-compiled) includes it.
#include <stddef.h>
#include <stdint.h>
#include "wifi_ap_logic.h"

// Main loop (pollWifi, wifi_mqtt.cpp): starts / stops the AP by the mode,
// the station state and the settings (a changed SSID / password / channel
// restarts it).
void apPoll(bool staUp);
// The station is used (false: mode "always" and no station network stored).
bool apStationUsed();
bool apIsActive();

struct ApStatus {
    uint8_t mode = AP_OFF;      // gCfg.apMode (AP_OFF while Wi-Fi or the web interface is off)
    bool    active = false;
    bool    passInvalid = false;   // a mode is set, but no valid password: not started
    uint8_t clients = 0;        // associated stations (updated every 2 s)
    uint8_t channel = 0;        // radio channel while active
    char    ssid[33] = "";      // the AP's SSID (also while not active: what it would be)
};
void apStatus(ApStatus& out);
// The Wi-Fi join text for the QR code (wifiQrText) of the configured AP;
// false = no mode or no valid password.
bool apQrText(char* out, size_t cap);

// Web task (webApiTask, under its server mutex, every pass): keeps the
// captive DNS socket (UDP 192.168.4.1:53) open while run && the AP is
// active and answers what has arrived; closes it otherwise (also before the
// setup portal starts its own DNS server).
void apDnsService(bool run);
