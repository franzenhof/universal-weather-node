#pragma once
#include "wind_window.h"

#include <stddef.h>
#include <stdint.h>

// Direct uploads to community weather networks (AWEKAS, Weather Underground,
// Weathercloud, Wetternetzwerk.pro) over WiFi — the same targets and wire
// formats as the separate weather-data-bridge (Python, MQTT-fed), but built
// into the node so a WiFi-connected station needs no extra server. Each
// network is enabled by a non-empty station ID/user in Config and has its
// own interval; uploads run on their own FreeRTOS task, so a slow or
// unreachable server never delays sampling or the LoRaWAN uplink.
//
// Only ever enable a given network in ONE place (here or in the bridge),
// otherwise it receives every reading twice.

// Spawns the upload task. Call once from setup(), after wifiMqttSetup().
void wxUploadSetup();

// Feeds the just-finished measurement cycle's values into every network's
// aggregation window and the daily rain total. Call once per cycle, at the
// same point as pushHistorySample().
// w10: 10-min wind statistics (WindRing::stats) used for every network.
void wxUploadPushCycle(const WindStats& w10);

// Guards the Config fields the upload task reads (station IDs/keys,
// intervals, latitude/longitude). Writers outside the main setup path
// (the web settings page) must hold it while changing them.
void wxUploadLock();
void wxUploadUnlock();

struct WxTargetStatus {
    const char*   name;
    bool          enabled;
    int           intervalSec;
    uint32_t      okCount;
    uint32_t      failCount;
    unsigned long lastAttemptMs;   // millis(); 0 = never
    unsigned long lastOkMs;        // millis(); 0 = never
    int           lastHttpCode;    // HTTPClient code (negative = connection error)
    char          lastReply[48];   // server reply (trimmed) or a local reason
};

constexpr size_t WX_TARGET_COUNT = 6;

// Copies a consistent snapshot of every network's status (WX_TARGET_COUNT
// entries) for the status page.
void wxUploadStatus(WxTargetStatus out[WX_TARGET_COUNT]);

// Rain since local midnight (mm). `valid` is false until NTP has synced at
// least once — before that, the "day" the total belongs to is unknown.
float wxDailyRainMm(bool& valid);

// Daily bookkeeping (local days, RTC RAM: survives restarts, not power loss).
struct WxDailyStats {
    bool    valid = false;            // NTP synced at least once (the day is known)
    float   rainTodayMm = 0, sunTodaySec = 0;
    bool    yesterdayValid = false;   // the "yesterday" values really belong to yesterday
    float   rainYesterdayMm = 0, sunYesterdaySec = 0;
    int64_t lastRainEpoch = 0;        // UTC seconds of the last rain, 0 = unknown
};
void wxDailyStats(WxDailyStats& out);

// Sunshine seconds from the selected light source (sources.cpp).
void wxAddSunshine(float sec);

// Firmware update check (gCfg.updateCheckEn, daily): the newer version
// found (empty = none / not checked) and a short status text.
void wxUpdateAvailable(char* out, size_t len);
void wxUpdateStatus(char* out, size_t len);
