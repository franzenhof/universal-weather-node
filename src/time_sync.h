#pragma once

#include <stdint.h>
#include <time.h>
#include "time_source.h"

// Wall clock from the time sources (time_source.h): a GNSS module, NTP while
// WiFi is up, the DCF77 time of a WH31 radio-clock sensor, a phone's browser
// time (POST /api/time), LoRaWAN DeviceTimeReq. Everything else in the firmware runs on uptime
// (history.cpp, the uplink cycle) and keeps working without it (history.cpp
// only aligns its buckets to the wall clock once it is valid); "today"
// (rain, records, daily history), the weather-network timestamps and the
// status page's clock need it.

// Call on every loop() iteration (cheap). Starts SNTP once WiFi first comes
// up, using gCfg.ntpServer/gCfg.timeZone.
void timeSyncPoll();

// Uptime in ms, 64-bit (esp_timer: never wraps, runs on in light sleep).
uint64_t uptimeMs64();

// A time from a source, any task: unixMs at uptime rxMs (uptimeMs64()),
// accurate to accMs. Applied unless a better source is fresh (time_source.h).
TimeSources::Result timeSourceOffer(TimeSource s, int64_t unixMs, uint64_t rxMs, uint32_t accMs);
// The source is stale from now on, worse ones are taken again (GNSS time
// switched off).
void timeSourceExpire(TimeSource s);

// The best source that is fresh now (TSRC_NONE = none): the rule for a
// phone's time from the station LAN (wifi_ap_logic.h phoneTimeCheck).
TimeSource timeBestFresh();
// A phone's browser time (POST /api/time, received at uptime rxMs): offered
// as TSRC_PHONE, accurate to 2 s (HTTP latency, the browser's clock).
TimeSources::Result timePhoneOffer(int64_t unixMs, uint64_t rxMs);

// For the status page and /api/info.
struct TimeSyncStatus {
    TimeSource source = TSRC_NONE;   // the clock follows this source (last applied / confirmed)
    bool       fresh = false;        // ... and it is still within its freshness window
    uint32_t   ageSec = 0;           // since that source's last report
    int64_t    loraNextSec = -1;     // next LoRaWAN DeviceTimeReq: 0 = with the next uplink, -1 = none planned
    bool       loraAnswered = false; // a DeviceTimeAns arrived since the start
    uint32_t   loraAnswerAgeSec = 0;
};
void timeSyncStatus(TimeSyncStatus& out);

// LoRaWAN DeviceTimeReq (main loop, around sendReceive() in main.cpp):
// true = add the request to this uplink (counts as requested).
bool loraTimeRequestDue();
void loraTimeJoined();                                  // after every join
void loraTimeAnswer(uint32_t unixSec, uint16_t ms);     // getMacDeviceTimeAns() result

// Re-applies gCfg.timeZone on the next main-loop pass (TZ is only changed on
// the main task, see time_sync.cpp) and restarts SNTP with
// gCfg.ntpServer on the next timeSyncPoll() — for the web settings page.
void timeSyncReconfigure();

// true while the clock shows a plausible time (2024 or later): set by a
// source, or kept by the RTC over a software reset (source still none).
bool timeIsValid();

// Seconds since 1970 (UTC). Returns false (out untouched) if the clock
// isn't valid yet.
bool epochNow(int64_t& out);

// Current local time (gCfg.timeZone). Returns false (out untouched) if the
// clock isn't valid yet.
bool localTimeNow(struct tm& out);
// Local time of t - localtime_r() for every task: holds the lock the main
// task changes TZ under (setenv() rewrites the TZ string another task's
// localtime_r() may be reading).
void localTimeOf(time_t t, struct tm& out);
