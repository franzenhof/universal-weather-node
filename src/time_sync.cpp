#include "time_sync.h"
#include "globals.h"
#include "wifi_mqtt.h"
#include "wx_upload.h"
#include "serial_log.h"
#include "rf_stats.h"
#include "rf_sensors.h"
#include "low_power.h"
#include "lora_transport.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <mutex>

namespace {

// Anything before this (2024-01-01) is the ESP32's unsynced 1970 epoch.
constexpr time_t MIN_VALID_EPOCH = 1704067200;

// volatile: cleared by timeSyncReconfigure() from the web server task.
volatile bool sntpStarted = false;
// new gCfg.timeZone, applied by timeSyncPoll(); set at the start too, so a
// node without WiFi (no configTzTime()) has its time zone
volatile bool tzPending = true;

// newlib's TZ state (setenv/tzset vs. localtime_r) is not safe across
// tasks: TZ is only changed on the main task (timeSyncPoll) under this lock.
// Other tasks convert through localTimeNow() / localTimeOf(), which take it;
// main-task code (history.cpp, astro_store.cpp, loxone_lines.cpp) may call
// localtime_r / mktime directly.
std::mutex tzLock;

// Accuracy classes (ms): a report closer than this to the clock confirms it
// without a step.
constexpr uint32_t NTP_ACC_MS = 50;
constexpr uint32_t DCF77_ACC_MS = 2000;     // whole seconds, the sensor's own clock between syncs
constexpr uint32_t LORAWAN_ACC_MS = 1000;   // RadioLib: < 100 ms intended, up to 1 s
constexpr uint32_t PHONE_ACC_MS = 2000;     // browser clock + request latency; phones agree within this, no stepping between them

// The sources and the LoRaWAN plan: offers come from the lwIP task (NTP),
// the main loop (DCF77, LoRaWAN) and the web task (phone).
std::mutex srcLock;
TimeSources sources;
LoraTimePlan loraPlan;
// for the log line in timeSyncPoll() (main task)
TimeSource loggedSource = TSRC_NONE;
volatile bool stepPending = false;
int64_t lastStepMs = 0;
TimeSource lastStepSource = TSRC_NONE;

// DCF77 frames of a WH31 radio-clock sensor (rf_sensors.h)
unsigned long dcfLastRx = 0;
bool dcfSeen = false;
Dcf77Plausibility dcfCheck;

int64_t systemMs(bool& valid)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    valid = tv.tv_sec >= MIN_VALID_EPOCH;
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void pollRadioClock()
{
    RfRadioClock rc;
    if (!rfRadioClock(rc) || (dcfSeen && rc.rxMs == dcfLastRx)) return;
    dcfSeen = true;
    dcfLastRx = rc.rxMs;
    const uint64_t rx = uptimeMs64() - (uint64_t)(millis() - rc.rxMs);
    int64_t u;
    bool ok;
    {
        std::lock_guard<std::mutex> g(tzLock);
        ok = dcf77LocalToUnix(rc.year, rc.month, rc.day, rc.hour, rc.minute, rc.second, u);
    }
    if (!ok) {
        dbgSerial.println("Time: DCF77 sensor time not used (invalid, or in the repeated / skipped hour of a DST change).");
        dcfCheck = Dcf77Plausibility();
        return;
    }
    if (!dcfCheck.check(u, rx)) {
        dbgSerial.println("Time: DCF77 sensor time noted; used once a second frame agrees with it.");
        return;
    }
    timeSourceOffer(TSRC_DCF77, u * 1000, rx, DCF77_ACC_MS);
}

} // namespace

uint64_t uptimeMs64()
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

TimeSources::Result timeSourceOffer(TimeSource s, int64_t unixMs, uint64_t rxMs, uint32_t accMs)
{
    std::lock_guard<std::mutex> g(srcLock);
    const uint64_t now = uptimeMs64();
    bool valid;
    const int64_t sys = systemMs(valid);
    int64_t step = 0;
    const TimeSources::Result r = sources.offer(s, unixMs, rxMs, accMs, now, sys, valid, step);
    if (r == TimeSources::TS_SET) {
        // the clock now, re-read: settimeofday() moves it by step
        const int64_t target = systemMs(valid) + step;
        struct timeval tv;
        tv.tv_sec = (time_t)(target / 1000);
        tv.tv_usec = (suseconds_t)((target % 1000) * 1000);
        settimeofday(&tv, nullptr);
        lastStepMs = step;
        lastStepSource = s;
        stepPending = true;
    }
    return r;
}

void timeSourceExpire(TimeSource s)
{
    std::lock_guard<std::mutex> g(srcLock);
    sources.expire(s);
}

TimeSource timeBestFresh()
{
    std::lock_guard<std::mutex> g(srcLock);
    return sources.bestFresh(uptimeMs64());
}

TimeSources::Result timePhoneOffer(int64_t unixMs, uint64_t rxMs)
{
    return timeSourceOffer(TSRC_PHONE, unixMs, rxMs, PHONE_ACC_MS);
}

// lwIP's SNTP client calls this (weak in ESP-IDF) for every NTP answer:
// the time goes through the sources instead of straight to settimeofday().
extern "C" void sntp_sync_time(struct timeval* tv)
{
    timeSourceOffer(TSRC_NTP, (int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000, uptimeMs64(), NTP_ACC_MS);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}

void timeSyncStatus(TimeSyncStatus& out)
{
    const bool lp = lowPowerActive();
    const bool lora = gCfg.loraEn && loraReady();
    std::lock_guard<std::mutex> g(srcLock);
    const uint64_t now = uptimeMs64();
    out = TimeSyncStatus();
    out.source = sources.current();
    if (out.source != TSRC_NONE) {
        uint64_t rx = 0;
        sources.lastReport(out.source, rx);
        out.ageSec = (uint32_t)((now - rx) / 1000);
        out.fresh = sources.fresh(out.source, now);
    }
    if (lora) {
        const int64_t due = loraPlan.dueInMs(now, sources.betterFresh(TSRC_LORAWAN, now), lp);
        out.loraNextSec = due < 0 ? -1 : due / 1000;
    }
    out.loraAnswered = loraPlan.haveAnswer;
    if (loraPlan.haveAnswer) out.loraAnswerAgeSec = (uint32_t)((now - loraPlan.answerMs) / 1000);
}

bool loraTimeRequestDue()
{
    const bool lp = lowPowerActive();
    std::lock_guard<std::mutex> g(srcLock);
    const uint64_t now = uptimeMs64();
    if (loraPlan.dueInMs(now, sources.betterFresh(TSRC_LORAWAN, now), lp) != 0) return false;
    loraPlan.requested(now);
    return true;
}

void loraTimeJoined()
{
    std::lock_guard<std::mutex> g(srcLock);
    loraPlan.joined();
}

void loraTimeAnswer(uint32_t unixSec, uint16_t ms)
{
    const uint64_t now = uptimeMs64();
    {
        std::lock_guard<std::mutex> g(srcLock);
        loraPlan.answered(now);
    }
    const TimeSources::Result r = timeSourceOffer(TSRC_LORAWAN, (int64_t)unixSec * 1000 + ms, now, LORAWAN_ACC_MS);
    dbgSerial.printf("LoRaWAN: DeviceTimeAns %lu.%03u (Unix)%s\n", (unsigned long)unixSec, ms,
                     r == TimeSources::TS_BLOCKED ? " - not used, a better time source is fresh" :
                     r == TimeSources::TS_INVALID ? " - implausible, not used" : "");
}

namespace {
// Source changes and clock steps into the log (main task).
void logTimeChanges()
{
    TimeSource cur;
    int64_t step = 0;
    TimeSource stepSrc = TSRC_NONE;
    bool stepped = false;
    {
        std::lock_guard<std::mutex> g(srcLock);
        cur = sources.current();
        if (stepPending) { stepped = true; step = lastStepMs; stepSrc = lastStepSource; stepPending = false; }
    }
    if (stepped) {
        struct tm lt;
        if (localTimeNow(lt))
            dbgSerial.printf("Time: clock set from %s (%+lld ms), local time %04d-%02d-%02d %02d:%02d:%02d\n",
                             timeSourceName(stepSrc), (long long)step, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                             lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
    if (cur != loggedSource) {
        dbgSerial.printf("Time: source %s -> %s\n", timeSourceName(loggedSource), timeSourceName(cur));
        loggedSource = cur;
    }
}
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
    if (!sntpStarted && gCfg.wifiEn && wifiIsConnected()) {
        // configTzTime() sets TZ (setenv+tzset) and starts SNTP in one go;
        // lwIP's SNTP client then re-syncs on its own (every 3 h,
        // CONFIG_LWIP_SNTP_UPDATE_DELAY; each answer goes through
        // sntp_sync_time() below) for
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
    }
    pollRadioClock();
    logTimeChanges();
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
    t = (uint32_t)(uptimeMs64() / 1000);   // 64-bit uptime: millis() / 1000 wraps after 49.7 days
    ymd = 0;
    return false;
}
