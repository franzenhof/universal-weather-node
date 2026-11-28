#pragma once

// Time sources (pure logic, host-tested in test/firmware_timesource.cpp).
// The glue that reads the clock and calls settimeofday() is time_sync.cpp.
//
// Every source reports a time (Unix ms), when it was received (uptime ms,
// 64-bit: never wraps) and its accuracy. The clock follows the best source
// that is fresh: a source is fresh for its freshness window after its last
// report, and a report from a worse source is not applied while a better
// one is fresh. Freshness is counted on the uptime, so a clock step never
// changes it.

#include <stdint.h>

// Days since 1970-01-01 of a YYYYMMDD date (days_from_civil, H. Hinnant).
inline int32_t ymdDayNumber(int32_t ymd)
{
    int32_t y = ymd / 10000;
    const int32_t m = ymd / 100 % 100, d = ymd % 100;
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;
    const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// true if ymd is the day before the stored day: the clock stepped back over
// midnight (a better source corrected it). "Today" (records, rain, sensor
// statistics) then stays on the stored day instead of rolling back - which
// would make today "yesterday" and lose the real yesterday. Only a step of
// one day is caught: a larger one counts as a new day.
inline bool ymdSteppedBack(int32_t ymd, int32_t stored)
{
    return ymd > 0 && stored > 0 && ymdDayNumber(stored) - ymdDayNumber(ymd) == 1;
}

// Priority: a lower value is better. All five report: GNSS (gnss.cpp), NTP,
// the DCF77 radio-clock sensor, LoRaWAN (time_sync.cpp) and the phone
// (POST /api/time).
enum TimeSource : uint8_t {
    TSRC_NONE = 0,
    TSRC_GNSS,
    TSRC_NTP,
    TSRC_DCF77,
    TSRC_PHONE,
    TSRC_LORAWAN,
    TSRC_COUNT
};

// "none", "gnss", "ntp", "dcf77", "phone", "lorawan" (API / log)
const char* timeSourceName(TimeSource s);
// Freshness window in seconds: GNSS 1 h (offered once a minute), NTP 24 h
// (lwIP re-syncs every 3 h), DCF77 sensor 25 h (a WH31 sends its time about
// once a day), phone 24 h, LoRaWAN 26 h (requested daily, every 6 h in
// low-power mode; the margin covers the wait for the next uplink and its
// answer, so the source does not go stale every day).
uint32_t timeSourceFreshSec(TimeSource s);

class TimeSources {
public:
    enum Result : uint8_t {
        TS_SET,        // apply: the clock moves by stepMs
        TS_CONFIRMED,  // accepted, the clock is already within the accuracy
        TS_BLOCKED,    // a better source is fresh: not applied
        TS_INVALID     // unknown source / time before 2024
    };

    // A report: unixMs at uptime rxMs, accurate to accMs. nowMs = uptime now,
    // sysMs / sysValid = the system clock now. stepMs = how far to move it
    // (TS_SET only).
    Result offer(TimeSource s, int64_t unixMs, uint64_t rxMs, uint32_t accMs,
                 uint64_t nowMs, int64_t sysMs, bool sysValid, int64_t& stepMs);

    bool fresh(TimeSource s, uint64_t nowMs) const;
    // The best fresh source, TSRC_NONE if none.
    TimeSource bestFresh(uint64_t nowMs) const;
    // true while a source better than s is fresh (s would be blocked).
    bool betterFresh(TimeSource s, uint64_t nowMs) const;

    // The source the clock follows (the last one applied or confirmed) and
    // when that was (uptime ms). TSRC_NONE until the first report.
    TimeSource current() const { return cur; }
    uint64_t currentMs() const { return curMs; }
    // Last applied or confirmed report of a source; false = none yet.
    bool lastReport(TimeSource s, uint64_t& rxMs) const;
    // The source is stale from now on (until its next report): GNSS time
    // switched off. Its last report stays for the status.
    void expire(TimeSource s);

    void reset() { *this = TimeSources(); }

private:
    struct Slot { bool have = false, expired = false; uint64_t rxMs = 0; };
    Slot slot[TSRC_COUNT];
    TimeSource cur = TSRC_NONE;
    uint64_t curMs = 0;
};

// DCF77 radio-clock sensor (WH31 / WN31 DNT, family 0x52). Its frames carry
// the local time of the time signal (CET / CEST). Converted with the node's
// time zone (TZ must be set by the caller); a time in the repeated hour at
// the end of summer time or in the skipped hour is refused (ambiguous).
bool dcf77LocalToUnix(int year, int month, int day, int hour, int minute, int second, int64_t& unixSec);

// Two consecutive frames must agree: the time between them by the frames and
// by the uptime may differ by at most 3 s + 1 ms per second between them
// (crystal / light-sleep drift). check() stores the frame as the previous one
// and returns true if it agrees with the one before.
struct Dcf77Plausibility {
    bool have = false;
    int64_t unixSec = 0;
    uint64_t rxMs = 0;
    bool check(int64_t frameUnixSec, uint64_t frameRxMs);
};

// LoRaWAN DeviceTimeReq planning: after every join, then once a day (every
// 6 h in low-power mode: the RTC drifts in light sleep), unanswered requests
// again after 1 h; never while a better source (NTP ...) is fresh, so a
// Wi-Fi node with NTP sends none.
struct LoraTimePlan {
    static constexpr uint64_t INTERVAL_MS    = 24ULL * 3600 * 1000;
    static constexpr uint64_t INTERVAL_LP_MS = 6ULL * 3600 * 1000;
    static constexpr uint64_t RETRY_MS       = 3600ULL * 1000;

    bool askAfterJoin = false;
    bool haveRequest = false, haveAnswer = false;
    uint64_t requestMs = 0, answerMs = 0;

    void joined() { askAfterJoin = true; }
    void requested(uint64_t nowMs) { haveRequest = true; requestMs = nowMs; askAfterJoin = false; }
    void answered(uint64_t nowMs) { haveAnswer = true; answerMs = nowMs; }
    // ms until the next request (0 = with the next uplink), -1 = none planned
    // (a better source is fresh).
    int64_t dueInMs(uint64_t nowMs, bool betterFresh, bool lowPower) const;
};
