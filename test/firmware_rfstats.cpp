// Per-sensor rain today / last hour and lightning strikes (src/rf_stats.cpp).
#include "Arduino.h"
#include "rf_stats.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

static uint32_t clockT = 0, clockYmd = 0;
bool rfStatsClock(uint32_t& t, uint32_t& ymd) { t = clockT; ymd = clockYmd; return ymd != 0; }

int main()
{
    const uint32_t D1 = 20261120, D2 = 20261121;
    uint32_t t = 1795000000;   // UTC seconds
    RfStat s;
    s.id = 1;
    // rain: increments add up; last hour = six 10-minute buckets
    rfStatRain(s, 0.2f, t, D1);
    rfStatRain(s, 0.4f, t + 300, D1);
    assert(near(s.rainTodayMm, 0.6f) && near(rfStatRain1h(s, t + 600), 0.6f));
    rfStatRain(s, 0.0f, t + 1800, D1);                       // nothing
    rfStatRain(s, -1.0f, t + 1800, D1);                      // never negative
    assert(near(s.rainTodayMm, 0.6f));
    assert(near(rfStatRain1h(s, t + 3700), 0.4f) || near(rfStatRain1h(s, t + 3700), 0.0f));   // the first bucket expired
    assert(near(rfStatRain1h(s, t + 7200), 0.0f) && near(s.rainTodayMm, 0.6f));
    // a new local day resets today
    rfStatRain(s, 1.0f, t + 86400, D2);
    assert(near(s.rainTodayMm, 1.0f) && s.ymd == D2);
    // before the clock is known (ymd 0): counted, the day starts when the date appears
    RfStat u;
    rfStatRain(u, 0.5f, 100, 0);
    assert(near(u.rainTodayMm, 0.5f) && u.ymd == 0);
    rfStatRain(u, 0.5f, t, D1);
    assert(near(u.rainTodayMm, 1.0f) && u.ymd == D1);

    // lightning: first packet = baseline, 8-bit wrap, a big jump = counter reset
    RfStat l;
    assert(rfStatLightning(l, 89, t, D1) == 0 && l.strikesToday == 0);
    assert(rfStatLightning(l, 92, t + 79, D1) == 3 && l.lastStrike == t + 79);
    assert(rfStatLightning(l, 92, t + 158, D1) == 0 && l.lastStrike == t + 79);
    assert(rfStatLightning(l, 254, t + 237, D1) == 0);       // +162: battery change / reset, not counted
    assert(rfStatLightning(l, 2, t + 316, D1) == 4);         // 254 -> 255 -> 0 -> 1 -> 2
    assert(l.strikesToday == 7 && rfStatStrikes1h(l, t + 400) == 7);
    assert(rfStatStrikes1h(l, t + 400 + 3600) == 0 && l.strikesToday == 7);
    assert(rfStatLightning(l, 5, t + 86400, D2) == 3 && l.strikesToday == 3);   // next day
    // the clock going back (set): the hour starts over, nothing negative
    assert(rfStatStrikes1h(l, t) == 0);

    // table: create, find, full, prune
    rfStatsClear();
    for (uint32_t id = 1; id <= RF_STATS_MAX; id++) assert(rfStatFor(id, true));
    assert(!rfStatFor(999, true) && rfStatFor(3, false) && rfStatFor(3, false)->id == 3);
    rfStatsPrune([](uint32_t id) { return id != 3; });
    assert(!rfStatFor(3, false) && rfStatFor(999, true));

    // readers (outputs, web pages) get a rolled copy; the table itself is
    // changed only by the receiving task (no day / bucket roll from a reader)
    rfStatsClear();
    clockT = t; clockYmd = D1;
    rfStatsLock();
    RfStat* w = rfStatFor(7, true);
    rfStatRain(*w, 0.6f, t, D1);
    rfStatsUnlock();
    RfStat view;
    uint32_t viewT = 0;
    assert(!rfStatSnapshot(8, view, viewT));
    assert(rfStatSnapshot(7, view, viewT) && viewT == t && near(view.rainTodayMm, 0.6f) && near(rfStatRain1h(view, viewT), 0.6f));
    clockT = t + 86400; clockYmd = D2;                       // next day, no new rain yet
    assert(rfStatSnapshot(7, view, viewT) && view.ymd == D2 && near(view.rainTodayMm, 0.0f) && near(rfStatRain1h(view, viewT), 0.0f));
    const RfStat* table = rfStatFor(7, false);
    assert(table->ymd == D1 && near(table->rainTodayMm, 0.6f) && table->bucket == t / 600);   // untouched
    puts("rfstats ok");
    return 0;
}
