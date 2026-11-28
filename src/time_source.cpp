#include "time_source.h"
#include <time.h>
#include <string.h>

namespace {
// Anything before this (2024-01-01) is not a real time (time_sync.cpp).
constexpr int64_t MIN_VALID_UNIX_MS = 1704067200LL * 1000;
// Steps smaller than this are not worth a settimeofday().
constexpr uint32_t MIN_STEP_MS = 20;
}

const char* timeSourceName(TimeSource s)
{
    static const char* const names[TSRC_COUNT] = { "none", "gnss", "ntp", "dcf77", "phone", "lorawan" };
    return s < TSRC_COUNT ? names[s] : "none";
}

uint32_t timeSourceFreshSec(TimeSource s)
{
    switch (s) {
        case TSRC_GNSS:    return 3600;
        case TSRC_NTP:     return 24 * 3600;
        case TSRC_DCF77:   return 25 * 3600;
        case TSRC_PHONE:   return 24 * 3600;
        case TSRC_LORAWAN: return 26 * 3600;
        default:           return 0;
    }
}

bool TimeSources::fresh(TimeSource s, uint64_t nowMs) const
{
    if (s == TSRC_NONE || s >= TSRC_COUNT || !slot[s].have || slot[s].expired) return false;
    return nowMs - slot[s].rxMs < (uint64_t)timeSourceFreshSec(s) * 1000;
}

TimeSource TimeSources::bestFresh(uint64_t nowMs) const
{
    for (uint8_t s = TSRC_NONE + 1; s < TSRC_COUNT; s++)
        if (fresh((TimeSource)s, nowMs)) return (TimeSource)s;
    return TSRC_NONE;
}

bool TimeSources::betterFresh(TimeSource s, uint64_t nowMs) const
{
    const TimeSource b = bestFresh(nowMs);
    return b != TSRC_NONE && b < s;
}

void TimeSources::expire(TimeSource s)
{
    if (s != TSRC_NONE && s < TSRC_COUNT) slot[s].expired = true;
}

bool TimeSources::lastReport(TimeSource s, uint64_t& rxMs) const
{
    if (s == TSRC_NONE || s >= TSRC_COUNT || !slot[s].have) return false;
    rxMs = slot[s].rxMs;
    return true;
}

TimeSources::Result TimeSources::offer(TimeSource s, int64_t unixMs, uint64_t rxMs, uint32_t accMs,
                                       uint64_t nowMs, int64_t sysMs, bool sysValid, int64_t& stepMs)
{
    stepMs = 0;
    if (s == TSRC_NONE || s >= TSRC_COUNT || unixMs < MIN_VALID_UNIX_MS || rxMs > nowMs) return TS_INVALID;
    // A blocked report does not count as fresh either: freshness says that
    // the clock follows this source.
    if (betterFresh(s, nowMs)) return TS_BLOCKED;
    slot[s].have = true;
    slot[s].expired = false;
    slot[s].rxMs = rxMs;
    cur = s;
    curMs = rxMs;
    const int64_t delta = unixMs + (int64_t)(nowMs - rxMs) - sysMs;
    const uint64_t mag = delta < 0 ? (uint64_t)-delta : (uint64_t)delta;
    if (sysValid && mag <= (accMs > MIN_STEP_MS ? accMs : MIN_STEP_MS)) return TS_CONFIRMED;
    stepMs = delta;
    return TS_SET;
}

bool dcf77LocalToUnix(int year, int month, int day, int hour, int minute, int second, int64_t& unixSec)
{
    if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 60 || hour < 0 || minute < 0 || second < 0)
        return false;
    // Try both the standard and the summer time reading: exactly one must
    // give back the same local fields (two = repeated hour, none = skipped).
    int found = 0;
    time_t t = 0;
    for (int dst = 0; dst <= 1; dst++) {
        struct tm in;
        memset(&in, 0, sizeof(in));
        in.tm_year = year - 1900; in.tm_mon = month - 1; in.tm_mday = day;
        in.tm_hour = hour; in.tm_min = minute; in.tm_sec = second > 59 ? 59 : second;
        in.tm_isdst = dst;
        const time_t c = mktime(&in);
        if (c == (time_t)-1) continue;
        struct tm back;
        localtime_r(&c, &back);
        if (back.tm_isdst != dst || back.tm_year != year - 1900 || back.tm_mon != month - 1 || back.tm_mday != day ||
            back.tm_hour != hour || back.tm_min != minute)
            continue;
        if (found && c != t) return false;   // two readings: ambiguous
        t = c;
        found = 1;
    }
    if (!found) return false;
    unixSec = (int64_t)t;
    return true;
}

bool Dcf77Plausibility::check(int64_t frameUnixSec, uint64_t frameRxMs)
{
    bool ok = false;
    if (have && frameRxMs > rxMs) {
        const uint64_t upMs = frameRxMs - rxMs;
        const int64_t diff = (frameUnixSec - unixSec) * 1000 - (int64_t)upMs;
        const uint64_t mag = diff < 0 ? (uint64_t)-diff : (uint64_t)diff;
        ok = mag <= 3000 + upMs / 1000;
    }
    have = true;
    unixSec = frameUnixSec;
    rxMs = frameRxMs;
    return ok;
}

int64_t LoraTimePlan::dueInMs(uint64_t nowMs, bool betterFresh, bool lowPower) const
{
    if (betterFresh) return -1;
    if (askAfterJoin || (!haveRequest && !haveAnswer)) return 0;
    uint64_t at = 0;
    if (haveAnswer) at = answerMs + (lowPower ? INTERVAL_LP_MS : INTERVAL_MS);
    if (haveRequest && (!haveAnswer || requestMs > answerMs)) {   // unanswered: again after RETRY_MS
        const uint64_t retry = requestMs + RETRY_MS;
        if (retry > at) at = retry;
    }
    return at > nowMs ? (int64_t)(at - nowMs) : 0;
}
