// Rain this week / month / year (src/rain_stats.cpp): ISO week keys, period
// roll-over, the daily values (today/yesterday/last rain), the NVS blob
// (version 2, migration of the old 28-byte blob), power loss and planned
// restarts (flush) with the NVS copy, rain while the clock is unknown
// and the flash write budget (at most hourly + day roll-over, never with the
// "rainPersist" setting off).
#include "rain_stats.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// Local time = UTC here (the firmware gets struct tm from localtime_r).
static time_t at(int y, int mo, int d, int h = 12, int mi = 0, int s = 0)
{
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    return timegm(&t);
}
static struct tm tmOf(time_t t) { struct tm r; gmtime_r(&t, &r); return r; }
static int32_t ymdOf(const struct tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

// Mirrors wx_upload.cpp: the RTC RAM record (daily + periods + pending),
// the save state, the NVS blob (real encoding) and a write counter. boot()
// is a restart (RTC kept), a power loss or an update that moved the RTC
// layout (RTC lost); cycle() is wxUploadPushCycle(), restart() is
// systemRestart() (rainStatsFlush() + boot with RTC RAM kept or lost).
struct Node {
    RainRecord    rtc = {};
    bool          rtcValid = false;
    RainSaveState save;
    std::vector<uint8_t> nvsBlob;   // empty = never written
    int           writes = 0, reads = 0, flushWrites = 0;
    bool          persist = true;
    std::map<int32_t, int> writesPerDay;

    bool nvsHas() const { return !nvsBlob.empty(); }
    RainRecord nvs() const { RainRecord r = {}; assert(rainBlobDecode(nvsBlob.data(), nvsBlob.size(), r) == 2); return r; }
    void write(const RainRecord& r, int32_t day)
    {
        nvsBlob.assign(RAIN_BLOB_SIZE, 0);
        rainBlobEncode(r, nvsBlob.data());
        writes++;
        writesPerDay[day]++;
    }
    void boot(bool rtcLost, int64_t upSec)
    {
        if (rtcLost) rtcValid = false;
        RainRecord copy = {};
        bool nvsOk = false;
        if (persist) {
            reads++;
            nvsOk = nvsHas() && rainBlobDecode(nvsBlob.data(), nvsBlob.size(), copy) != 0;
        }
        if (!rtcValid) { rtc = nvsOk ? copy : RainRecord{}; rtcValid = true; }
        save = RainSaveState{};
        save.saved = nvsOk;
        if (nvsOk) save.last = copy;
        save.lastSaveSec = upSec;
    }
    // clockKnown false: before the first NTP sync after a boot.
    void cycle(time_t now, bool clockKnown, float mm, int64_t upSec, float sun = 0.0f)
    {
        struct tm lt = tmOf(now);
        if (clockKnown) rainRecordRoll(rtc, lt);
        rainRecordAddRain(rtc, mm, clockKnown, (int64_t)now);
        rainRecordAddSun(rtc, sun, clockKnown);
        if (clockKnown && rainSaveDue(save, rtc, upSec, ymdOf(lt), persist)) write(rtc, ymdOf(lt));
    }
    void flush(time_t now, bool clockKnown, int64_t upSec)
    {
        if (clockKnown) rainRecordRoll(rtc, tmOf(now));
        if (rainFlushDue(save, rtc, upSec, persist)) { write(rtc, clockKnown ? ymdOf(tmOf(now)) : 0); flushWrites++; }
    }
    void restart(time_t now, bool clockKnown, int64_t upSec, bool rtcLost = false)
    {
        flush(now, clockKnown, upSec);
        boot(rtcLost, 0);
    }
};

int main()
{
    // ---- ISO 8601 week keys (Monday start; weeks over New Year) ----
    assert(rainIsoWeekKey(tmOf(at(2026, 11, 19))) == 202647);   // Thursday
    assert(rainIsoWeekKey(tmOf(at(2026, 11, 22))) == 202647);   // Sunday: same week
    assert(rainIsoWeekKey(tmOf(at(2026, 11, 23))) == 202648);   // Monday: next week
    assert(rainIsoWeekKey(tmOf(at(2027, 1, 1)))   == 202653);   // 2026 has 53 weeks
    assert(rainIsoWeekKey(tmOf(at(2027, 1, 4)))   == 202701);
    assert(rainIsoWeekKey(tmOf(at(2024, 12, 30))) == 202501);   // Monday in week 1 of the next year
    assert(rainIsoWeekKey(tmOf(at(2021, 1, 3)))   == 202053);
    assert(rainIsoWeekKey(tmOf(at(2023, 1, 1)))   == 202252);
    assert(rainMonthKey(tmOf(at(2026, 11, 19))) == 202611 && rainYearKey(tmOf(at(2026, 11, 19))) == 2026);

    // ---- roll-over ----
    {
        RainPeriods p = {};
        assert(rainPeriodsRoll(p, tmOf(at(2026, 11, 22, 23, 59))));   // first dating: keys set, nothing dropped
        rainPeriodsAdd(p, 5.0f);
        assert(!rainPeriodsRoll(p, tmOf(at(2026, 11, 22, 23, 59, 30))));
        // week change on Monday: week starts at 0, month and year continue
        assert(rainPeriodsRoll(p, tmOf(at(2026, 11, 23, 0, 0))));
        assert(p.weekMm == 0 && near(p.monthMm, 5) && near(p.yearMm, 5) && p.weekKey == 202648);
        rainPeriodsAdd(p, 1.0f);
        rainPeriodsAdd(p, -3.0f);                                  // ignored
        assert(near(p.weekMm, 1) && near(p.monthMm, 6) && near(p.yearMm, 6));
    }
    {
        // month end: Sat 31 Oct -> Sun 1 Nov 2026 (same ISO week): month 0, week and year continue
        RainPeriods p = {};
        rainPeriodsRoll(p, tmOf(at(2026, 10, 31, 23)));
        rainPeriodsAdd(p, 2.0f);
        assert(rainPeriodsRoll(p, tmOf(at(2026, 11, 1, 0, 5))));
        assert(near(p.weekMm, 2) && p.monthMm == 0 && near(p.yearMm, 2) && p.monthKey == 202611);
    }
    {
        // year end inside an ISO week (Thu 31 Dec 2026 -> Fri 1 Jan 2027, both 2026-W53)
        RainPeriods p = {};
        rainPeriodsRoll(p, tmOf(at(2026, 12, 31, 22)));
        rainPeriodsAdd(p, 3.0f);
        rainPeriodsRoll(p, tmOf(at(2027, 1, 1, 0, 1)));
        assert(near(p.weekMm, 3) && p.monthMm == 0 && p.yearMm == 0 && p.yearKey == 2027 && p.monthKey == 202701);
        // year end on a Monday (Sun 31 Dec 2023 -> Mon 1 Jan 2024): everything starts at 0
        RainPeriods q = {};
        rainPeriodsRoll(q, tmOf(at(2023, 12, 31, 22)));
        rainPeriodsAdd(q, 4.0f);
        rainPeriodsRoll(q, tmOf(at(2024, 1, 1, 0, 1)));
        assert(q.weekMm == 0 && q.monthMm == 0 && q.yearMm == 0 && q.weekKey == 202401);
        // several periods skipped (node off for months): all stale totals dropped
        RainPeriods r = {};
        rainPeriodsRoll(r, tmOf(at(2026, 3, 10)));
        rainPeriodsAdd(r, 7.0f);
        rainPeriodsRoll(r, tmOf(at(2026, 6, 10)));
        assert(r.weekMm == 0 && r.monthMm == 0 && near(r.yearMm, 7));
    }

    // ---- validity of a copy read from NVS ----
    {
        RainPeriods p = {};
        assert(!rainPeriodsValid(p));                               // never dated
        rainPeriodsRoll(p, tmOf(at(2027, 1, 1)));
        assert(rainPeriodsValid(p));                                // week 2026-W53 in year 2027
        p.monthMm = NAN;
        assert(!rainPeriodsValid(p));
        p.monthMm = -1.0f;
        assert(!rainPeriodsValid(p));
    }

    // ---- write budget: 3 days of rain in every 5-min cycle (+ a 10-s cycle run) ----
    for (int cycleSec : { 300, 10 }) {
        Node n;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 20, 0, 0, 7);
        int64_t up = 0;
        for (int64_t s = 0; s < 3 * 86400; s += cycleSec) {
            up = s;
            n.cycle(t0 + s, true, 0.2f, up);
        }
        assert(n.writesPerDay.size() == 3);
        for (const auto& d : n.writesPerDay) {
            printf("rain stats: %d-s cycle, day %d: %d NVS writes\n", cycleSec, (int)d.first, d.second);
            assert(d.second <= 25 && d.second >= 23);
        }
        assert(n.writes <= 3 * 25);
        // the flash copy is never more than an hour behind
        assert(n.rtc.p.yearMm - n.nvs().p.yearMm <= 3600.0f / cycleSec * 0.2f + 0.01f);
    }

    // ---- dry weather: no hourly writes, one at each day roll-over (the date moves on) ----
    {
        Node n;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 18, 0, 3);   // Wednesday
        for (int64_t s = 0; s < 2 * 86400; s += 300) n.cycle(t0 + s, true, 0.0f, s);
        assert(n.writes == 2);                // the first dating + Thursday
        // Friday .. Monday (new week key on Monday: still one write that day)
        for (int64_t s = 2 * 86400; s < 6 * 86400; s += 300) n.cycle(t0 + s, true, 0.0f, s);
        assert(n.writes == 6 && n.nvs().p.weekKey == 202648 && n.nvs().d.ymd == 20261123);
    }

    // ---- setting off: no NVS write at all, and the copy is not read at boot ----
    {
        Node n;
        n.persist = false;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 20, 0, 0, 7);
        for (int64_t s = 0; s < 3 * 86400; s += 300) n.cycle(t0 + s, true, 0.2f, s);
        assert(n.writes == 0 && n.reads == 0 && !n.nvsHas());
        assert(std::fabs(n.rtc.p.monthMm - 3 * 288 * 0.2f) < 0.05f);
        n.boot(false, 3 * 86400);             // restart: RTC RAM keeps the totals
        assert(std::fabs(n.rtc.p.monthMm - 3 * 288 * 0.2f) < 0.05f);
        n.write(n.rtc, 0); n.writes = 0;     // an old copy from when it was on
        n.boot(true, 0);                      // power loss: starts at 0, copy ignored
        assert(n.rtc.p.yearMm == 0 && n.reads == 0);
    }

    // ---- power loss: restored from the NVS copy, then rolled over ----
    {
        Node n;
        n.boot(true, 0);
        time_t t0 = at(2026, 10, 30, 10, 0);   // Friday
        for (int64_t s = 0; s < 6 * 3600; s += 300) n.cycle(t0 + s, true, 0.5f, s);
        const RainPeriods lastCopy = n.nvs().p;
        assert(n.writes >= 6 && n.writes <= 7 && lastCopy.monthKey == 202610);
        assert(n.rtc.p.monthMm - lastCopy.monthMm <= 12 * 0.5f + 0.01f);   // at most an hour behind
        // restart (RTC RAM valid): totals unchanged, no extra write right after boot
        float before = n.rtc.p.monthMm;
        n.boot(false, 0);
        assert(n.rtc.p.monthMm == before);
        n.cycle(t0 + 6 * 3600, true, 0.0f, 1);
        assert(n.writes == (int)n.writesPerDay[20261030]);
        int w = n.writes;
        n.cycle(t0 + 6 * 3600 + 300, true, 0.0f, 300);
        assert(n.writes == w);   // unchanged, hourly slot not reached
        // power cut, back on 2 Nov (Monday, new week, new month)
        n.boot(true, 0);
        assert(rainRecordEqual(n.rtc, n.nvs()));
        float year = n.rtc.p.yearMm;
        n.cycle(at(2026, 11, 2, 8), true, 1.0f, 300);
        assert(near(n.rtc.p.weekMm, 1) && near(n.rtc.p.monthMm, 1) && near(n.rtc.p.yearMm, year + 1));
        assert(near(n.rtc.d.mm, 1) && n.rtc.d.prevYmd == 0 && n.rtc.d.prevMm == 0);   // 30 Oct is not "yesterday"
        assert(n.writes == w + 1);   // new periods: written once
        // power cut again on 1 Jan: year starts over
        n.boot(true, 0);
        n.cycle(at(2027, 1, 1, 9), true, 2.0f, 60);
        assert(near(n.rtc.p.yearMm, 2) && near(n.rtc.p.monthMm, 2) && n.rtc.p.yearKey == 2027);
    }

    // ---- no time sync: rain goes to the pending bucket, counted once the clock is known ----
    {
        Node n;
        n.boot(true, 0);
        n.cycle(at(2026, 10, 31, 20), true, 3.0f, 0);   // October: 3 mm
        int w = n.writes;
        n.boot(false, 0);                                // restart, clock unknown for a while
        for (int i = 0; i < 24; i++) n.cycle(0, false, 0.1f, i * 300);
        assert(n.writes == w && near(n.rtc.p.monthMm, 3) && near(n.rtc.pendingMm, 2.4f));
        // first sync on 1 Nov: October is closed, the pending rain belongs to November
        n.cycle(at(2026, 11, 1, 2), true, 0.0f, 24 * 300);
        assert(near(n.rtc.p.monthMm, 2.4f) && near(n.rtc.p.yearMm, 5.4f) && near(n.rtc.p.weekMm, 5.4f));
        assert(n.rtc.pendingMm == 0);
    }

    // ---- failed write: retried at the next hourly slot, not every cycle ----
    {
        RainSaveState st;
        RainRecord p = {};
        rainRecordRoll(p, tmOf(at(2026, 11, 19)));
        rainRecordAddRain(p, 1.0f, true, at(2026, 11, 19));
        assert(rainSaveDue(st, p, 100, 20261119, true));
        rainSaveFailed(st);
        assert(!rainSaveDue(st, p, 400, 20261119, true));
        assert(!rainSaveDue(st, p, 3699, 20261119, true));
        assert(rainSaveDue(st, p, 3700, 20261119, true));
        assert(!rainSaveDue(st, p, 7400, 20261119, true));        // written, unchanged since
    }
    // ---- daily roll-over (local days) ----
    {
        RainDaily d = {};
        assert(rainDailyRoll(d, 20261031) && d.ymd == 20261031 && d.prevYmd == 0);   // first dating
        d.mm = 4.0f; d.sunSec = 3600.0f; d.lastRainEpoch = 1000;
        assert(!rainDailyRoll(d, 20261031));
        assert(rainDailyRoll(d, 20261101));                                          // month end
        assert(d.prevYmd == 20261031 && near(d.prevMm, 4) && near(d.prevSunSec, 3600) && d.mm == 0 && d.sunSec == 0);
        assert(d.lastRainEpoch == 1000);                                             // kept
        d.mm = 2.0f;
        rainDailyRoll(d, 20261103);                                                  // 2 days later: yesterday unknown
        assert(d.prevYmd == 0 && d.prevMm == 0 && d.prevSunSec == 0 && d.mm == 0 && d.lastRainEpoch == 1000);
        RainDaily y = {};
        rainDailyRoll(y, 20261231);
        y.mm = 1.5f;
        rainDailyRoll(y, 20270101);                                                  // year end
        assert(y.prevYmd == 20261231 && near(y.prevMm, 1.5f));
        RainDaily l = {};
        rainDailyRoll(l, 20280228);
        l.mm = 1.0f;
        rainDailyRoll(l, 20280229);                                                  // leap day
        assert(l.prevYmd == 20280228 && near(l.prevMm, 1));
        assert(rainYmdDayNumber(19700101) == 0 && rainYmdDayNumber(20280301) - rainYmdDayNumber(20280228) == 2);
    }

    // ---- flash blob: round trip of every field, refused when corrupt ----
    {
        RainRecord r = {};
        r.d = { 20261119, 3.25f, 20261118, 12.5f, 7200.0f, 1800.0f, (int64_t)at(2026, 11, 19, 9, 41) };
        r.p = { 202647, 202611, 2026, 15.75f, 40.5f, 812.25f };
        r.pendingMm = 0.4f; r.pendingSunSec = 120.0f;
        uint8_t b[RAIN_BLOB_SIZE];
        rainBlobEncode(r, b);
        RainRecord back = {};
        assert(rainBlobDecode(b, sizeof(b), back) == 2 && rainRecordEqual(r, back));
        assert(back.d.lastRainEpoch == r.d.lastRainEpoch && back.d.prevYmd == 20261118 && near(back.d.prevMm, 12.5f));
        assert(b[0] == RAIN_BLOB_VERSION && b[1] == 0 && b[2] == RAIN_BLOB_SIZE && b[3] == 0);   // version, size
        RainRecord untouched = {};
        assert(rainBlobDecode(b, sizeof(b) - 1, untouched) == 0 && untouched.p.yearKey == 0);   // wrong length
        uint8_t v3[RAIN_BLOB_SIZE];
        memcpy(v3, b, sizeof(b)); v3[0] = 3;                         // future version: not taken
        assert(rainBlobDecode(v3, sizeof(v3), untouched) == 0);
        RainRecord bad = r;
        bad.d.prevMm = NAN;
        rainBlobEncode(bad, v3);
        assert(rainBlobDecode(v3, sizeof(v3), untouched) == 0);       // implausible value
        RainRecord undated = {};                                      // clock never known: not worth a copy
        assert(!rainRecordValid(undated));
    }

    // ---- migration: the old 28-byte "periods" blob (version 1) ----
    {
        struct OldBlob { uint32_t version; RainPeriods p; } old = { 1, { 202647, 202611, 2026, 5.0f, 20.0f, 300.0f } };
        static_assert(sizeof(OldBlob) == RAIN_BLOB_V1_SIZE, "old layout");
        RainRecord r = {};
        assert(rainBlobDecode((const uint8_t*)&old, sizeof(old), r) == 1);
        assert(rainPeriodsEqual(r.p, old.p) && r.d.ymd == 0 && r.d.mm == 0 && r.d.lastRainEpoch == 0 && r.pendingMm == 0);
        old.version = 7;
        assert(rainBlobDecode((const uint8_t*)&old, sizeof(old), r) == 0);
        old.version = 1;
        // a node with the old copy loses power, comes back on Friday 20 Nov
        Node n;
        n.nvsBlob.assign((const uint8_t*)&old, (const uint8_t*)&old + sizeof(old));
        n.boot(true, 0);
        assert(near(n.rtc.p.yearMm, 300) && n.rtc.d.ymd == 0);
        n.cycle(at(2026, 11, 20, 8), true, 0.5f, 60);
        assert(near(n.rtc.p.weekMm, 5.5f) && near(n.rtc.p.yearMm, 300.5f) && near(n.rtc.d.mm, 0.5f));
        assert(n.writes == 1 && n.nvsBlob.size() == RAIN_BLOB_SIZE);  // new date: rewritten as version 2, same key
        assert(rainRecordEqual(n.nvs(), n.rtc));
    }

    // ---- power loss: yesterday and the last rain come back from the copy ----
    {
        Node n;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 18, 6, 0);
        int64_t s = 0;
        for (; s < 8 * 3600; s += 300) n.cycle(t0 + s, true, s < 4 * 3600 ? 0.25f : 0.0f, s, 300.0f);
        const int64_t lastRain = (int64_t)(t0 + 4 * 3600 - 300);
        assert(n.rtc.d.lastRainEpoch == lastRain && near(n.rtc.d.mm, 12));
        // into the next day (roll-over written), dry
        for (; s < 26 * 3600; s += 300) n.cycle(t0 + s, true, 0.0f, s);
        assert(n.rtc.d.ymd == 20261119 && n.rtc.d.prevYmd == 20261118 && near(n.rtc.d.prevMm, 12));
        assert(near(n.rtc.d.prevSunSec, 8 * 12 * 300.0f));
        // power cut at 08:00 on 19 Nov, back at 08:20
        n.boot(true, 0);
        assert(n.rtc.d.prevYmd == 20261118 && near(n.rtc.d.prevMm, 12) && n.rtc.d.lastRainEpoch == lastRain);
        n.cycle(at(2026, 11, 19, 8, 20), true, 0.0f, 60);
        assert(n.rtc.d.ymd == 20261119 && near(n.rtc.d.prevMm, 12) && n.rtc.d.lastRainEpoch == lastRain);
        // power cut again, back only on 21 Nov: yesterday (20 Nov) unknown, last rain kept
        n.boot(true, 0);
        n.cycle(at(2026, 11, 21, 9), true, 0.0f, 60);
        assert(n.rtc.d.ymd == 20261121 && n.rtc.d.prevYmd == 0 && n.rtc.d.prevMm == 0 && n.rtc.d.mm == 0);
        assert(n.rtc.d.lastRainEpoch == lastRain && near(n.rtc.p.monthMm, 12));
    }

    // ---- planned restart (OTA that moves the RTC layout): flushed exactly once, nothing lost ----
    {
        Node n;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 19, 0, 30);
        int64_t s = 0;
        for (; s < 10 * 3600 + 7 * 60; s += 60) n.cycle(t0 + s, true, s < 9 * 3600 ? 0.0f : 0.1f, s);
        const RainRecord before = n.rtc;
        assert(!rainRecordEqual(before, n.nvs()));                   // the hourly copy is behind
        int w = n.writes;
        n.flush(t0 + s, true, s);
        assert(n.writes == w + 1 && n.flushWrites == 1);
        n.flush(t0 + s, true, s + 1);                                // second call (e.g. two paths): no write
        assert(n.writes == w + 1);
        n.boot(true, 0);                                             // new firmware, RTC copy invalid
        assert(rainRecordEqual(n.rtc, before) && near(n.rtc.d.mm, before.d.mm));
        // nothing changed since: the restart writes nothing
        w = n.writes;
        n.restart(t0 + s + 30, true, 30, true);
        assert(n.writes == w && rainRecordEqual(n.rtc, before));
        // a restart before the clock sync: pending rain survives in the copy, then goes into today
        n.cycle(0, false, 0.6f, 10);
        n.restart(0, false, 20, true);
        assert(near(n.rtc.pendingMm, 0.6f) && n.writes == w + 1);
        n.cycle(t0 + s + 600, true, 0.0f, 30);
        assert(n.rtc.pendingMm == 0 && near(n.rtc.d.mm, before.d.mm + 0.6f) && near(n.rtc.p.yearMm, before.p.yearMm + 0.6f));
    }

    // ---- write budget with planned restarts: continuous rain, one restart a day (OTA/settings) ----
    for (int restartsPerDay : { 1, 4 }) {
        Node n;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 20, 0, 0, 7);
        int64_t up = 0, next = 37 * 60 + 600;                          // off the hourly slots
        const int64_t every = 86400 / restartsPerDay;
        int restarts = 0;
        for (int64_t s = 0; s < 3 * 86400; s += 300) {
            if (s >= next) {                                            // every other one moves the RTC layout
                n.restart(t0 + s, true, up, ++restarts % 2 == 0);
                up = 0;
                next += every;
            }
            n.cycle(t0 + s, true, 0.2f, up);
            up += 300;
        }
        assert(n.flushWrites == restarts);
        for (const auto& d : n.writesPerDay) {
            printf("rain stats: %d restart(s)/day, day %d: %d NVS writes\n", restartsPerDay, (int)d.first, d.second);
            assert(d.second <= 24 + restartsPerDay);
            if (restartsPerDay == 1) assert(d.second <= 25);
        }
        assert(std::fabs(n.rtc.p.monthMm - 3 * 288 * 0.2f) < 0.05f);  // nothing lost over the restarts
    }

    // ---- setting off: restarts write nothing and never read the copy ----
    {
        Node n;
        n.persist = false;
        n.boot(true, 0);
        time_t t0 = at(2026, 11, 20, 8);
        for (int64_t s = 0; s < 3600; s += 300) n.cycle(t0 + s, true, 0.2f, s);
        n.restart(t0 + 3600, true, 3600);
        n.restart(t0 + 3700, true, 100, true);                        // RTC lost: starts at 0
        assert(n.writes == 0 && n.reads == 0 && !n.nvsHas() && n.rtc.p.yearMm == 0 && n.rtc.d.mm == 0);
        RainSaveState st;
        RainRecord r = {};
        rainRecordRoll(r, tmOf(t0));
        assert(!rainFlushDue(st, r, 0, false));
    }
    return 0;
}
