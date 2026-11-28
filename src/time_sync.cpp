#include "time_sync.h"
#include "globals.h"
#include "wifi_mqtt.h"
#include "wx_upload.h"
#include "serial_log.h"
#include "rf_stats.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <mutex>

namespace {

// Anything before this (2024-01-01) is the ESP32's unsynced 1970 epoch.
constexpr time_t MIN_VALID_EPOCH = 1704067200;

// volatile: cleared by timeSyncReconfigure() from the web server task.
volatile bool sntpStarted = false;
volatile bool tzPending = false;   // new gCfg.timeZone, applied by timeSyncPoll()
bool loggedValid = false;

// newlib's TZ state (setenv/tzset vs. localtime_r) is not safe across
// tasks: TZ is only changed on the main task (timeSyncPoll) under this lock.
// Other tasks convert through localTimeNow() / localTimeOf(), which take it;
// main-task code (history.cpp, astro_store.cpp, loxone_lines.cpp) may call
// localtime_r / mktime directly.
std::mutex tzLock;

} // namespace

void timeSyncPoll()
{
    if (tzPending) {
        tzPending = false;
        wxUploadLock();   // gCfg.timeZone is written by the web task
        {
            std::lock_guard<std::mutex> g(tzLock);
            setenv("TZ", gCfg.timeZone, 1);
            tzset();
        }
        wxUploadUnlock();
    }
    if (!sntpStarted) {
        if (!gCfg.wifiEn || !wifiIsConnected()) return;
        // configTzTime() sets TZ (setenv+tzset) and starts SNTP in one go;
        // lwIP's SNTP client then re-syncs on its own (default: hourly) for
        // as long as the node runs, so this only ever needs calling once.
        // lwIP keeps the server name *pointer*, not a copy — gCfg's own
        // buffer stays valid for the device's lifetime. wxUploadLock: the
        // web settings page writes these fields from another task.
        wxUploadLock();
        const char* server = gCfg.ntpServer[0] ? gCfg.ntpServer : "pool.ntp.org";
        {
            std::lock_guard<std::mutex> g(tzLock);
            configTzTime(gCfg.timeZone, server, "time.google.com");
        }
        sntpStarted = true;
        dbgSerial.printf("NTP: started (server %s, TZ %s)\n", server, gCfg.timeZone);
        wxUploadUnlock();
        return;
    }
    if (!loggedValid && timeIsValid()) {
        loggedValid = true;
        struct tm lt;
        localTimeNow(lt);
        dbgSerial.printf("NTP: time synced, local time %04d-%02d-%02d %02d:%02d:%02d\n",
                         lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                         lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
}

void timeSyncReconfigure()
{
    // Called from the web task: only flag it, the main task applies the TZ
    // (next loop() iteration). configTzTime() stops a running SNTP client
    // itself before restarting it.
    tzPending = true;
    sntpStarted = false;
}

bool timeIsValid()
{
    return time(nullptr) >= MIN_VALID_EPOCH;
}

bool epochNow(int64_t& out)
{
    time_t now = time(nullptr);
    if (now < MIN_VALID_EPOCH) return false;
    out = (int64_t)now;
    return true;
}

bool localTimeNow(struct tm& out)
{
    time_t now = time(nullptr);
    if (now < MIN_VALID_EPOCH) return false;
    localTimeOf(now, out);
    return true;
}

void localTimeOf(time_t t, struct tm& out)
{
    std::lock_guard<std::mutex> g(tzLock);
    localtime_r(&t, &out);
}

// Clock of the per-sensor rain / lightning statistics (rf_stats.h): UTC
// seconds and the local date once synced, else the uptime and no date.
bool rfStatsClock(uint32_t& t, uint32_t& ymd)
{
    int64_t e;
    struct tm lt;
    if (epochNow(e) && localTimeNow(lt)) {
        t = (uint32_t)e;
        ymd = (uint32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday);
        return true;
    }
    t = (uint32_t)(esp_timer_get_time() / 1000000LL);   // 64-bit uptime: millis() / 1000 wraps after 49.7 days
    ymd = 0;
    return false;
}
