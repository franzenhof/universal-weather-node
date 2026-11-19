#include "rain_stats.h"
#include <math.h>
#include <string.h>

namespace {

// ISO 8601: a year has 53 weeks if it starts on a Thursday, or is a leap
// year starting on a Wednesday (p(y) = weekday of 31 December, 0 = Sunday).
int isoWeeksInYear(int y)
{
    auto p = [](int year) { return (year + year / 4 - year / 100 + year / 400) % 7; };
    return (p(y) == 4 || p(y - 1) == 3) ? 53 : 52;
}

} // namespace

int32_t rainIsoWeekKey(const struct tm& lt)
{
    int year = lt.tm_year + 1900;
    int isoWday = (lt.tm_wday + 6) % 7 + 1;              // Monday 1 .. Sunday 7
    int week = (lt.tm_yday + 1 - isoWday + 10) / 7;
    if (week < 1) {
        year--;
        week = isoWeeksInYear(year);
    } else if (week > isoWeeksInYear(year)) {
        year++;
        week = 1;
    }
    return year * 100 + week;
}

int32_t rainMonthKey(const struct tm& lt) { return (lt.tm_year + 1900) * 100 + lt.tm_mon + 1; }
int32_t rainYearKey(const struct tm& lt)  { return lt.tm_year + 1900; }

bool rainPeriodsRoll(RainPeriods& p, const struct tm& lt)
{
    bool changed = false;
    auto roll = [&changed](int32_t& key, float& mm, int32_t now) {
        if (key == now) return;
        // 0 = never dated (fresh start): nothing to drop, the total stays
        if (key != 0) mm = 0.0f;
        key = now;
        changed = true;
    };
    roll(p.weekKey,  p.weekMm,  rainIsoWeekKey(lt));
    roll(p.monthKey, p.monthMm, rainMonthKey(lt));
    roll(p.yearKey,  p.yearMm,  rainYearKey(lt));
    return changed;
}

void rainPeriodsAdd(RainPeriods& p, float mm)
{
    if (!(mm > 0.0f)) return;
    p.weekMm  += mm;
    p.monthMm += mm;
    p.yearMm  += mm;
}

bool rainPeriodsEqual(const RainPeriods& a, const RainPeriods& b)
{
    return a.weekKey == b.weekKey && a.monthKey == b.monthKey && a.yearKey == b.yearKey &&
           a.weekMm == b.weekMm && a.monthMm == b.monthMm && a.yearMm == b.yearMm;
}

bool rainPeriodsValid(const RainPeriods& p)
{
    auto mmOk = [](float v) { return isfinite(v) && v >= 0.0f && v < 100000.0f; };
    return p.yearKey >= 2000 && p.yearKey <= 9999 &&
           p.monthKey / 100 == p.yearKey && p.monthKey % 100 >= 1 && p.monthKey % 100 <= 12 &&
           p.weekKey / 100 >= p.yearKey - 1 && p.weekKey / 100 <= p.yearKey + 1 &&
           p.weekKey % 100 >= 1 && p.weekKey % 100 <= 53 &&
           mmOk(p.weekMm) && mmOk(p.monthMm) && mmOk(p.yearMm);
}

int32_t rainYmd(const struct tm& lt) { return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday; }

int32_t rainYmdDayNumber(int32_t ymd)
{
    // days_from_civil (H. Hinnant)
    int32_t y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;
    const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool rainDailyRoll(RainDaily& d, int32_t ymd)
{
    if (d.ymd == ymd) return false;
    if (d.ymd != 0) {
        if (rainYmdDayNumber(ymd) - rainYmdDayNumber(d.ymd) == 1) {
            d.prevYmd    = d.ymd;
            d.prevMm     = d.mm;
            d.prevSunSec = d.sunSec;
        } else {   // two or more days (or backwards): yesterday is not known
            d.prevYmd    = 0;
            d.prevMm     = 0.0f;
            d.prevSunSec = 0.0f;
        }
        d.mm     = 0.0f;
        d.sunSec = 0.0f;
    }
    d.ymd = ymd;
    return true;
}

bool rainRecordRoll(RainRecord& r, const struct tm& lt)
{
    bool changed = rainPeriodsRoll(r.p, lt);
    changed |= rainDailyRoll(r.d, rainYmd(lt));
    if (r.pendingMm != 0.0f || r.pendingSunSec != 0.0f) {
        if (r.pendingMm > 0.0f) r.d.mm += r.pendingMm;
        if (r.pendingSunSec > 0.0f) r.d.sunSec += r.pendingSunSec;
        rainPeriodsAdd(r.p, r.pendingMm);
        r.pendingMm = r.pendingSunSec = 0.0f;
        changed = true;
    }
    return changed;
}

void rainRecordAddRain(RainRecord& r, float mm, bool clockKnown, int64_t epoch)
{
    if (!(mm > 0.0f)) return;
    if (!clockKnown) { r.pendingMm += mm; return; }
    r.d.mm += mm;
    r.d.lastRainEpoch = epoch;
    rainPeriodsAdd(r.p, mm);
}

void rainRecordAddSun(RainRecord& r, float sec, bool clockKnown)
{
    if (!(sec > 0.0f)) return;
    if (clockKnown) r.d.sunSec += sec;
    else            r.pendingSunSec += sec;
}

namespace {

bool mmOk(float v)  { return isfinite(v) && v >= 0.0f && v < 100000.0f; }
bool ymdOk(int32_t ymd)
{
    int32_t y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    return y >= 2000 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

void put32(uint8_t* b, size_t& o, uint32_t v) { for (int i = 0; i < 4; i++) b[o++] = (uint8_t)(v >> (8 * i)); }
void putI(uint8_t* b, size_t& o, int32_t v)   { put32(b, o, (uint32_t)v); }
void putF(uint8_t* b, size_t& o, float v)     { uint32_t u; memcpy(&u, &v, 4); put32(b, o, u); }
uint32_t get32(const uint8_t* b, size_t& o)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)b[o++] << (8 * i);
    return v;
}
int32_t getI(const uint8_t* b, size_t& o) { return (int32_t)get32(b, o); }
float   getF(const uint8_t* b, size_t& o) { uint32_t u = get32(b, o); float v; memcpy(&v, &u, 4); return v; }

void putPeriods(uint8_t* b, size_t& o, const RainPeriods& p)
{
    putI(b, o, p.weekKey); putI(b, o, p.monthKey); putI(b, o, p.yearKey);
    putF(b, o, p.weekMm);  putF(b, o, p.monthMm);  putF(b, o, p.yearMm);
}
RainPeriods getPeriods(const uint8_t* b, size_t& o)
{
    RainPeriods p;
    p.weekKey = getI(b, o); p.monthKey = getI(b, o); p.yearKey = getI(b, o);
    p.weekMm  = getF(b, o); p.monthMm  = getF(b, o); p.yearMm  = getF(b, o);
    return p;
}

} // namespace

bool rainDailyValid(const RainDaily& d)
{
    return (d.ymd == 0 || ymdOk(d.ymd)) && (d.prevYmd == 0 || ymdOk(d.prevYmd)) &&
           mmOk(d.mm) && mmOk(d.prevMm) && mmOk(d.sunSec) && mmOk(d.prevSunSec) && d.lastRainEpoch >= 0;
}

bool rainRecordEqual(const RainRecord& a, const RainRecord& b)
{
    return a.d.ymd == b.d.ymd && a.d.mm == b.d.mm && a.d.prevYmd == b.d.prevYmd && a.d.prevMm == b.d.prevMm &&
           a.d.sunSec == b.d.sunSec && a.d.prevSunSec == b.d.prevSunSec && a.d.lastRainEpoch == b.d.lastRainEpoch &&
           rainPeriodsEqual(a.p, b.p) && a.pendingMm == b.pendingMm && a.pendingSunSec == b.pendingSunSec;
}

bool rainRecordValid(const RainRecord& r)
{
    return rainPeriodsValid(r.p) && rainDailyValid(r.d) && mmOk(r.pendingMm) && mmOk(r.pendingSunSec);
}

void rainBlobEncode(const RainRecord& r, uint8_t out[RAIN_BLOB_SIZE])
{
    size_t o = 0;
    put32(out, o, (uint32_t)RAIN_BLOB_VERSION | (uint32_t)RAIN_BLOB_SIZE << 16);
    putI(out, o, r.d.ymd);     putF(out, o, r.d.mm);
    putI(out, o, r.d.prevYmd); putF(out, o, r.d.prevMm);
    putF(out, o, r.d.sunSec);  putF(out, o, r.d.prevSunSec);
    put32(out, o, (uint32_t)((uint64_t)r.d.lastRainEpoch & 0xFFFFFFFFu));
    put32(out, o, (uint32_t)((uint64_t)r.d.lastRainEpoch >> 32));
    putPeriods(out, o, r.p);
    putF(out, o, r.pendingMm); putF(out, o, r.pendingSunSec);
}

int rainBlobDecode(const uint8_t* b, size_t len, RainRecord& out)
{
    if (!b || len < 4) return 0;
    size_t o = 0;
    const uint32_t head = get32(b, o);
    RainRecord r = {};
    int version;
    if (len == RAIN_BLOB_V1_SIZE && head == 1) {           // old periods-only blob
        r.p = getPeriods(b, o);
        version = 1;
    } else if (len == RAIN_BLOB_SIZE && (head & 0xFFFF) == RAIN_BLOB_VERSION && (head >> 16) == RAIN_BLOB_SIZE) {
        r.d.ymd = getI(b, o);     r.d.mm = getF(b, o);
        r.d.prevYmd = getI(b, o); r.d.prevMm = getF(b, o);
        r.d.sunSec = getF(b, o);  r.d.prevSunSec = getF(b, o);
        uint64_t lo = get32(b, o), hi = get32(b, o);
        r.d.lastRainEpoch = (int64_t)(lo | hi << 32);
        r.p = getPeriods(b, o);
        r.pendingMm = getF(b, o); r.pendingSunSec = getF(b, o);
        version = 2;
    } else {
        return 0;
    }
    if (!rainRecordValid(r)) return 0;
    out = r;
    return version;
}

bool rainSaveDue(RainSaveState& st, const RainRecord& cur, int64_t nowSec, int32_t ymd, bool persistEn)
{
    bool newDay = (st.lastYmd != 0 && ymd != st.lastYmd) || (st.saved && cur.d.ymd != st.last.d.ymd);
    st.lastYmd = ymd;
    if (!persistEn) return false;
    // A new week / month / year since the copy (e.g. back on after a power
    // cut in a new month) counts like a day roll-over: at most once a day.
    bool newPeriod = st.saved && (cur.p.weekKey != st.last.p.weekKey || cur.p.monthKey != st.last.p.monthKey ||
                                  cur.p.yearKey != st.last.p.yearKey);
    bool due = !st.saved || newDay || newPeriod || nowSec - st.lastSaveSec >= RAIN_SAVE_MIN_INTERVAL_SEC;
    if (!due || (st.saved && rainRecordEqual(st.last, cur))) return false;
    st.saved = true;
    st.last = cur;
    st.lastSaveSec = nowSec;
    return true;
}

bool rainFlushDue(RainSaveState& st, const RainRecord& cur, int64_t nowSec, bool persistEn)
{
    if (!persistEn || !rainRecordValid(cur)) return false;
    if (st.saved && rainRecordEqual(st.last, cur)) return false;
    st.saved = true;
    st.last = cur;
    st.lastSaveSec = nowSec;
    return true;
}

void rainSaveFailed(RainSaveState& st)
{
    st.last.p.weekMm = -1.0f;   // differs from any real copy: written again at the next hourly slot
}
