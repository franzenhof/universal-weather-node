#pragma once

#include <stdint.h>
#include <time.h>

// Wall clock via SNTP, only available while WiFi is up. Everything else in
// the firmware runs on uptime (history.cpp, the uplink cycle) and keeps
// working without it (history.cpp only aligns its buckets to the wall clock
// once it is valid); only the direct weather-network uploads (daily rain
// total reset at local midnight, AWEKAS/Weathercloud local timestamps) and
// the status page's clock need it.

// Call on every loop() iteration (cheap). Starts SNTP once WiFi first comes
// up, using gCfg.ntpServer/gCfg.timeZone.
void timeSyncPoll();

// Re-applies gCfg.timeZone on the next main-loop pass (TZ is only changed on
// the main task, see time_sync.cpp) and restarts SNTP with
// gCfg.ntpServer on the next timeSyncPoll() — for the web settings page.
void timeSyncReconfigure();

// true while the clock shows a plausible time (2024 or later): set by SNTP,
// or kept by the RTC over a software reset.
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
