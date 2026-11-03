#pragma once

#include <time.h>

// Wall clock via SNTP, only available while WiFi is up. Everything else in
// the firmware runs on uptime (history.cpp, the uplink cycle) and keeps
// working without it; only the direct weather-network uploads (daily rain
// total reset at local midnight, AWEKAS/Weathercloud local timestamps) and
// the status page's clock need it.

// Call on every loop() iteration (cheap). Starts SNTP once WiFi first comes
// up, using gCfg.ntpServer/gCfg.timeZone.
void timeSyncPoll();

// Re-applies gCfg.timeZone immediately and restarts SNTP with
// gCfg.ntpServer on the next timeSyncPoll() — for the web settings page.
void timeSyncReconfigure();

// true once SNTP has delivered a plausible time at least once since boot.
bool timeIsValid();

// Current local time (gCfg.timeZone). Returns false (out untouched) if the
// clock isn't valid yet.
bool localTimeNow(struct tm& out);
