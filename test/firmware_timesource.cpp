// Time sources (src/time_source.cpp): priority, freshness, no override by a
// worse source, step / confirm, DCF77 conversion and plausibility, the
// LoRaWAN DeviceTimeReq plan, the "stepped back over midnight" day check.
#include "time_source.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>

static const int64_t T0 = 1781049600LL * 1000;   // 2026-06-10 00:00 UTC
static const uint64_t H = 3600ULL * 1000;

static void testPriority()
{
    TimeSources ts;
    int64_t step = 0;
    // no time yet: LoRaWAN sets it (rx 2 s ago: the time is projected to now)
    assert(ts.offer(TSRC_LORAWAN, T0, 8000, 1000, 10000, 5000, false, step) == TimeSources::TS_SET);
    assert(step == T0 + 2000 - 5000);
    assert(ts.current() == TSRC_LORAWAN && ts.bestFresh(10000) == TSRC_LORAWAN);
    // NTP is better: overrides a fresh LoRaWAN
    uint64_t now = 20000;
    int64_t sys = T0 + 12000;
    assert(ts.offer(TSRC_NTP, sys + 1500, now, 50, now, sys, true, step) == TimeSources::TS_SET && step == 1500);
    assert(ts.current() == TSRC_NTP);
    // LoRaWAN while NTP is fresh: blocked, even 10 s off
    now += H;
    sys += H;
    assert(ts.offer(TSRC_LORAWAN, sys + 10000, now, 1000, now, sys, true, step) == TimeSources::TS_BLOCKED && step == 0);
    assert(ts.current() == TSRC_NTP);
    assert(ts.betterFresh(TSRC_LORAWAN, now) && ts.betterFresh(TSRC_DCF77, now) && !ts.betterFresh(TSRC_NTP, now));
    // DCF77 and the phone are blocked by NTP as well; GNSS overrides it
    assert(ts.offer(TSRC_DCF77, sys, now, 2000, now, sys, true, step) == TimeSources::TS_BLOCKED);
    assert(ts.offer(TSRC_PHONE, sys, now, 2000, now, sys, true, step) == TimeSources::TS_BLOCKED);
    assert(ts.offer(TSRC_GNSS, sys + 300, now, 10, now, sys, true, step) == TimeSources::TS_SET && step == 300);
    assert(ts.current() == TSRC_GNSS);
    // NTP while GNSS is fresh: blocked
    assert(ts.offer(TSRC_NTP, sys - 5000, now, 50, now, sys, true, step) == TimeSources::TS_BLOCKED);
    // GNSS stale after 1 h: NTP (still fresh from its earlier report) takes over again
    now += H + 1;
    sys += H + 1;
    assert(!ts.fresh(TSRC_GNSS, now) && ts.fresh(TSRC_NTP, now) && ts.bestFresh(now) == TSRC_NTP);
    assert(ts.offer(TSRC_NTP, sys, now, 50, now, sys, true, step) == TimeSources::TS_CONFIRMED);
    assert(ts.current() == TSRC_NTP);
    // NTP 24 h without an answer: stale, LoRaWAN takes over (a blocked report never counted as fresh)
    now += 24 * H;
    sys += 24 * H;
    assert(!ts.fresh(TSRC_NTP, now) && ts.bestFresh(now) == TSRC_NONE);
    assert(ts.offer(TSRC_LORAWAN, sys - 4000, now, 1000, now, sys, true, step) == TimeSources::TS_SET && step == -4000);
    assert(ts.current() == TSRC_LORAWAN && ts.fresh(TSRC_LORAWAN, now));
    uint64_t rx = 0;
    assert(ts.lastReport(TSRC_LORAWAN, rx) && rx == now);
    assert(!ts.lastReport(TSRC_PHONE, rx));
}

static void testFreshness()
{
    assert(timeSourceFreshSec(TSRC_GNSS) == 3600 && timeSourceFreshSec(TSRC_NTP) == 86400 &&
           timeSourceFreshSec(TSRC_DCF77) == 90000 && timeSourceFreshSec(TSRC_PHONE) == 86400 &&
           timeSourceFreshSec(TSRC_LORAWAN) == 26 * 3600 && timeSourceFreshSec(TSRC_NONE) == 0);
    // LoRaWAN stays fresh past its daily request (it goes out with the next uplink, the answer comes after it)
    assert(timeSourceFreshSec(TSRC_LORAWAN) * 1000ULL > LoraTimePlan::INTERVAL_MS + 3600ULL * 1000);
    TimeSources ts;
    int64_t step;
    assert(ts.offer(TSRC_DCF77, T0, 1000, 2000, 1000, 0, false, step) == TimeSources::TS_SET);
    assert(ts.fresh(TSRC_DCF77, 1000 + 25 * H - 1) && !ts.fresh(TSRC_DCF77, 1000 + 25 * H));
    // the phone is blocked during the DCF77 window, then accepted
    assert(ts.offer(TSRC_PHONE, T0, 1000 + 25 * H - 1, 2000, 1000 + 25 * H - 1, T0, true, step) == TimeSources::TS_BLOCKED);
    assert(ts.offer(TSRC_PHONE, T0, 1000 + 25 * H, 2000, 1000 + 25 * H, T0, true, step) == TimeSources::TS_CONFIRMED);
    assert(ts.current() == TSRC_PHONE);
}

static void testExpire()
{
    // GNSS time switched off: the slot expires at once, NTP is taken again
    TimeSources ts;
    int64_t step;
    assert(ts.offer(TSRC_GNSS, T0, 1000, 20, 1000, T0, true, step) == TimeSources::TS_CONFIRMED);
    assert(ts.offer(TSRC_NTP, T0 + 500, 2000, 50, 2000, T0 + 1000, true, step) == TimeSources::TS_BLOCKED);
    ts.expire(TSRC_GNSS);
    assert(!ts.fresh(TSRC_GNSS, 2000) && ts.bestFresh(2000) == TSRC_NONE);
    uint64_t rx = 0;
    assert(ts.current() == TSRC_GNSS && ts.lastReport(TSRC_GNSS, rx) && rx == 1000);   // kept for the status
    assert(ts.offer(TSRC_NTP, T0 + 1500, 3000, 50, 3000, T0 + 1000, true, step) == TimeSources::TS_SET && step == 500);
    assert(ts.current() == TSRC_NTP);
    // a new GNSS report makes it fresh again
    assert(ts.offer(TSRC_GNSS, T0 + 4000, 4000, 20, 4000, T0 + 4000, true, step) == TimeSources::TS_CONFIRMED);
    assert(ts.fresh(TSRC_GNSS, 4000) && ts.betterFresh(TSRC_NTP, 4000));
    ts.expire(TSRC_NONE);   // ignored
    ts.expire(TSRC_COUNT);
}

static void testConfirmAndInvalid()
{
    TimeSources ts;
    int64_t step;
    // within the accuracy: confirmed, no step
    assert(ts.offer(TSRC_LORAWAN, T0 + 900, 5000, 1000, 5000, T0, true, step) == TimeSources::TS_CONFIRMED && step == 0);
    assert(ts.offer(TSRC_LORAWAN, T0 - 1001, 5000, 1000, 5000, T0, true, step) == TimeSources::TS_SET && step == -1001);
    // NTP: steps of more than 50 ms
    assert(ts.offer(TSRC_NTP, T0 + 40, 6000, 50, 6000, T0, true, step) == TimeSources::TS_CONFIRMED);
    assert(ts.offer(TSRC_NTP, T0 + 60, 6000, 50, 6000, T0, true, step) == TimeSources::TS_SET && step == 60);
    // before 2024, unknown source, received in the future
    assert(ts.offer(TSRC_NTP, 1000, 6000, 50, 6000, T0, true, step) == TimeSources::TS_INVALID);
    assert(ts.offer(TSRC_NONE, T0, 6000, 50, 6000, T0, true, step) == TimeSources::TS_INVALID);
    assert(ts.offer(TSRC_COUNT, T0, 6000, 50, 6000, T0, true, step) == TimeSources::TS_INVALID);
    assert(ts.offer(TSRC_NTP, T0, 7000, 50, 6000, T0, true, step) == TimeSources::TS_INVALID);
    assert(std::string(timeSourceName(TSRC_LORAWAN)) == "lorawan" && std::string(timeSourceName(TSRC_NONE)) == "none" &&
           std::string(timeSourceName(TSRC_DCF77)) == "dcf77" && std::string(timeSourceName(TSRC_COUNT)) == "none");
}

static void testDcf77()
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    int64_t u;
    // summer (CEST, UTC+2) and winter (CET, UTC+1)
    assert(dcf77LocalToUnix(2026, 6, 10, 2, 0, 0, u) && u == 1781049600LL);
    assert(dcf77LocalToUnix(2026, 1, 15, 2, 0, 5, u) && u == 1768438805LL);
    // 2026-10-25: 02:00-03:00 twice (refused), 2026-03-29: 02:00-03:00 skipped (refused)
    assert(!dcf77LocalToUnix(2026, 10, 25, 2, 30, 0, u));
    assert(dcf77LocalToUnix(2026, 10, 25, 1, 59, 0, u) && dcf77LocalToUnix(2026, 10, 25, 3, 0, 0, u));
    assert(!dcf77LocalToUnix(2026, 3, 29, 2, 30, 0, u));
    assert(dcf77LocalToUnix(2026, 3, 29, 3, 0, 0, u) && u == 1774746000LL);
    // implausible fields
    assert(!dcf77LocalToUnix(2023, 6, 10, 2, 0, 0, u) && !dcf77LocalToUnix(2026, 13, 1, 0, 0, 0, u) &&
           !dcf77LocalToUnix(2026, 6, 10, 24, 0, 0, u) && !dcf77LocalToUnix(2026, 6, 0, 0, 0, 0, u));
    setenv("TZ", "UTC0", 1);
    tzset();
    assert(dcf77LocalToUnix(2026, 10, 25, 2, 30, 0, u) && u == 1792895400LL);   // no DST: never ambiguous

    // plausibility: the first frame alone is never used
    Dcf77Plausibility p;
    assert(!p.check(1781049600LL, 1000000));
    // 60 s later by the uptime and by the frames (1 s off): used
    assert(p.check(1781049661LL, 1060000));
    // a corrupt frame (one hour off) is refused, and the next good one is compared with it: refused too
    assert(!p.check(1781053261LL, 1120000));
    assert(!p.check(1781049781LL, 1180000));
    // ... the one after agrees again
    assert(p.check(1781049841LL, 1240000));
    // a day apart: 3 s + 86 s drift allowed
    assert(p.check(1781049841LL + 86400 + 80, 1240000 + 86400000ULL));
    Dcf77Plausibility q;
    q.check(1781049600LL, 1000000);
    assert(!q.check(1781049600LL + 86400 + 100, 1000000 + 86400000ULL));
    // the same reception twice (no new frame) does not count
    Dcf77Plausibility r;
    r.check(1781049600LL, 1000000);
    assert(!r.check(1781049600LL, 1000000));
}

static void testLoraPlan()
{
    LoraTimePlan p;
    // before any request: with the next uplink; never while a better source is fresh
    assert(p.dueInMs(1000, false, false) == 0);
    assert(p.dueInMs(1000, true, false) == -1);
    p.requested(1000);
    // unanswered: again after 1 h
    assert(p.dueInMs(1000 + 1800000, false, false) == 1800000);
    assert(p.dueInMs(1000 + H, false, false) == 0);
    p.requested(1000 + H);
    p.answered(1000 + H + 5000);
    // answered: once a day, every 6 h in low-power mode
    assert(p.dueInMs(1000 + H + 5000, false, false) == (int64_t)(24 * H));
    assert(p.dueInMs(1000 + H + 5000, false, true) == (int64_t)(6 * H));
    assert(p.dueInMs(1000 + H + 5000 + 6 * H, false, true) == 0);
    // a (re)join asks again at once, unless a better source is fresh
    p.joined();
    assert(p.dueInMs(2 * H, false, false) == 0 && p.dueInMs(2 * H, true, false) == -1);
    p.requested(2 * H);
    assert(!p.askAfterJoin);
    // that request unanswered: the next one after max(answer + 24 h, request + 1 h)
    assert(p.dueInMs(2 * H, false, false) == (int64_t)(1000 + H + 5000 + 24 * H - 2 * H));
    assert(p.dueInMs(2 * H, false, true) == (int64_t)(1000 + H + 5000 + 6 * H - 2 * H));
}

static void testDayStep()
{
    assert(ymdDayNumber(19700101) == 0 && ymdDayNumber(20260610) == 20614 && ymdDayNumber(20240301) - ymdDayNumber(20240228) == 2);
    assert(ymdSteppedBack(20261125, 20261126));
    assert(ymdSteppedBack(20251231, 20260101));
    assert(!ymdSteppedBack(20261126, 20261126) && !ymdSteppedBack(20261127, 20261126));   // same day, forward
    assert(!ymdSteppedBack(20261124, 20261126));   // two days back: a real date change
    assert(!ymdSteppedBack(20261125, 0) && !ymdSteppedBack(0, 20261126));
}

int main()
{
    testPriority();
    testFreshness();
    testExpire();
    testConfirmAndInvalid();
    testDcf77();
    testLoraPlan();
    testDayStep();
    return 0;
}
