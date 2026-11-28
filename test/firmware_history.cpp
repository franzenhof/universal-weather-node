// History (src/history.cpp): buckets on fixed windows (one ring entry per
// window, gaps for windows without a cycle), rain windows never longer than
// nominal, trend samples not younger than nominal.
// history.cpp is compiled into this file: the host build has no FreeRTOS
// mutex functions, and runtime.cpp's "no history" stubs are renamed away.
#define historyPressureAgo hostStubPressureAgo
#define historyTempAgo     hostStubTempAgo
#define historyRainSum     hostStubRainSum
#include "host/runtime.cpp"
#undef historyPressureAgo
#undef historyTempAgo
#undef historyRainSum

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#ifndef portMAX_DELAY
#define portMAX_DELAY 0
#endif
#define xSemaphoreTake(m, t) ((void)(m), (void)(t), true)
#define xSemaphoreGive(m)    ((void)(m), true)
#include "../src/history.cpp"

#include "sources.h"
#include <cassert>
#include <cmath>

// Wall clock for the bucket alignment: off until a test "syncs" it.
static bool hostSynced = false;
static int64_t hostEpochAtZero = 0;
bool epochNow(int64_t& out)
{
    if (!hostSynced) return false;
    out = hostEpochAtZero + (int64_t)(millis() / 1000);
    return true;
}

// The ranges of before (24h / 7d) by name
static void buildHistoryJson(JsonDocument& doc, bool coarseRange)
{
    HistoryQuery q;
    assert(historyParseQuery(coarseRange ? "7d" : "24h", nullptr, nullptr, q));
    buildHistoryJson(doc, q);
}

// Empty history, clock not synced, uptime phaseMs past a 5-min boundary.
static void resetHistory(unsigned long phaseMs)
{
    historyAllocate();
    fine.head = fine.count = 0;     *fine.open = OpenBucket{};
    coarse.head = coarse.count = 0; *coarse.open = OpenBucket{};
    daily.head = daily.count = 0;   *daily.open = OpenBucket{};
    dailyPrimed = false;
    hourPrimePending = false;
    clockOffset = 0; clockOffsetFromEpoch = false;
    hostSynced = false;
    hostNow = phaseMs;
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Checks one range's JSON: `buckets` finished entries plus the open one,
// evenly spaced by bucketSec, the oldest ending about buckets x bucketSec ago
// (+- one bucket); finished entry i has rain rainOf(i) (< 0: a gap, null).
template <typename F>
static void checkRange(bool coarseRange, size_t buckets, unsigned long bucketSec, F rainOf)
{
    JsonDocument doc;
    buildHistoryJson(doc, coarseRange);
    JsonArray age = doc["ageSec"], rain = doc["rainMm"], temp = doc["temp"];
    assert(age.size() == buckets + 1 && rain.size() == age.size() && temp.size() == age.size());
    long span = age[0].as<long>();
    long nominal = (long)(buckets * bucketSec);
    assert(span >= nominal - (long)bucketSec && span <= nominal + (long)bucketSec);
    assert(age[buckets].as<long>() == 0);   // the open bucket, newest
    for (size_t i = 0; i < buckets; i++) {
        if (i + 1 < buckets) assert(age[i].as<long>() - age[i + 1].as<long>() == (long)bucketSec);
        float r = rainOf(i);
        if (r < 0) { assert(rain[i].isNull() && temp[i].isNull()); }
        else       { assert(!rain[i].isNull() && near(rain[i].as<float>(), r) && !temp[i].isNull()); }
    }
}

static void runFor(unsigned long sec, unsigned long stepSec, float rainPerStep)
{
    for (unsigned long t = 0; t < sec; t += stepSec) {
        delay(stepSec * 1000);
        rainMM = rainPerStep;
        pushHistorySample();
    }
}

// ---- Long history (hourly 35 days, local days 2 years) ----

static void useTz(const char* tz) { setenv("TZ", tz, 1); tzset(); }

// 5-min cycles until the uptime reaches untilSec: 0.1 mm rain each, the
// temperature 10 x (day of month % 10) + 0.03 x (5-min slot of the local
// day the cycle ends in), so each day runs from 10 x d (its first slot) up.
static void runCycles(unsigned long untilSec)
{
    while (millis() / 1000 + 300 <= untilSec) {
        delay(300000);
        int64_t e;
        assert(epochNow(e));
        time_t t = (time_t)(e - 1);
        struct tm lt;
        localtime_r(&t, &lt);
        const int slot = (lt.tm_hour * 60 + lt.tm_min) / 5;
        gSrc.tempC = (lt.tm_mday % 10) * 10.0f + slot * 0.03f;
        gSrc.humidity = 40.0f + slot * 0.1f;          // 40 .. 68.7 % over the day
        gSrc.pressSeaHpa = 1000.0f + slot * 0.1f;     // 1000 .. 1028.7 hPa
        rainMM = 0.1f;
        pushHistorySample();
    }
}

static void query(JsonDocument& doc, const char* range, const char* end = nullptr, const char* res = nullptr)
{
    HistoryQuery q;
    assert(historyParseQuery(range, end, res, q));
    buildHistoryJson(doc, q);
    size_t n = doc["ageSec"].size();
    for (const char* k : { "rainMm", "temp", "tempMin", "tempMax", "humMin", "humMax", "pressMin", "pressMax", "windGust" }) assert(doc[k].size() == n);
}

static void checkLongHistory()
{
    gCfg.windEn = 0;
    gSrc.tempValid = true;
    gSrc.humValid = true;
    gSrc.pressValid = true;

    // API parameters and the plan
    {
        HistoryQuery q;
        assert(historyParseQuery("bogus", nullptr, nullptr, q) && q.rangeSec == 86400 && !strcmp(q.range, "24h"));
        assert(historyParseQuery("30d", "", "", q) && q.rangeSec == 30 * 86400 && q.endEpoch == 0 && q.res == -1);
        assert(historyParseQuery("1y", "1781000000", "1d", q) && q.endEpoch == 1781000000 && q.res == HT_DAILY);
        assert(!historyParseQuery("7d", "17810x", nullptr, q));
        assert(!historyParseQuery("7d", "-5", nullptr, q));
        assert(!historyParseQuery("7d", nullptr, "2h", q));
        int tier;
        uint32_t group;
        historyPlan(86400, 86400, -1, tier, group);            assert(tier == HT_FINE && group == 1);
        historyPlan(86400, 2 * 86400, -1, tier, group);        assert(tier == HT_HOURLY && group == 1);   // yesterday
        historyPlan(7 * 86400, 14 * 86400, -1, tier, group);   assert(tier == HT_HOURLY && group == 1);   // last week
        historyPlan(30 * 86400, 30 * 86400, -1, tier, group);  assert(tier == HT_HOURLY && group == 2);
        historyPlan(30 * 86400, 60 * 86400, -1, tier, group);  assert(tier == HT_DAILY && group == 1);    // last month
        historyPlan(365 * 86400, 730 * 86400, -1, tier, group); assert(tier == HT_DAILY && group == 1);   // last year
        historyPlan(7 * 86400, 7 * 86400, HT_FINE, tier, group); assert(tier == HT_FINE && group == 6);
        assert(historyRangeMaxPoints(86400) == 288 && historyRangeMaxPoints(7 * 86400) == 168);
        assert(historyRangeMaxPoints(30 * 86400) == 360 && historyRangeMaxPoints(365 * 86400) == 365);
    }

    // UTC days: rollover as soon as a day's last hour is in, daily rain sum,
    // min / max of the cycle values, mean of the hourly means.
    useTz("UTC0");
    resetHistory(0);
    hostSynced = true;
    hostEpochAtZero = 1781049600;   // 2026-06-10 00:00 UTC at uptime 0
    runCycles(3 * 86400 + 300);     // 3 days + the first cycle of the 4th
    assert(daily.count == 3 && !daily.open->started);
    JsonDocument doc;
    query(doc, "1y");
    assert(!strcmp(doc["res"], "1d") && doc["bucketSec"] == 86400 && !strcmp(doc["range"], "1y"));
    {
        JsonArray age = doc["ageSec"], rain = doc["rainMm"], t = doc["temp"], lo = doc["tempMin"], hi = doc["tempMax"];
        assert(age.size() == 4 && age[3] == 0);
        for (int d = 0; d < 3; d++) {
            const float base = ((10 + d) % 10) * 10.0f;   // June 10, 11, 12
            assert(age[d].as<long>() == (2 - d) * 86400L + 300);   // ends at midnight
            assert(near(rain[d].as<float>(), 28.8f));
            assert(near(lo[d].as<float>(), base) && std::fabs(hi[d].as<float>() - (base + 8.61f)) < 0.051f);
            assert(std::fabs(t[d].as<float>() - (base + 4.305f)) < 0.06f);
            // humidity / pressure: the day's extremes from the hourly ones (humidity in 0.5 % steps)
            assert(near(doc["humMin"][d].as<float>(), 40.0f) && std::fabs(doc["humMax"][d].as<float>() - 68.7f) <= 0.25f);
            assert(near(doc["pressMin"][d].as<float>(), 1000.0f) && std::fabs(doc["pressMax"][d].as<float>() - 1028.7f) < 0.051f);
            assert(doc["humMin"][d].as<float>() <= doc["humidityPct"][d].as<float>() && doc["humidityPct"][d].as<float>() <= doc["humMax"][d].as<float>());
            assert(doc["pressMin"][d].as<float>() <= doc["pressureQnh"][d].as<float>() && doc["pressureQnh"][d].as<float>() <= doc["pressMax"][d].as<float>());
        }
        assert(near(rain[3].as<float>(), 0.1f));    // the open day: its open hour
    }

    // Node off for two days: the missing day is a gap (null), not 0 and not skipped.
    delay(2UL * 86400 * 1000 - 300000);       // no cycles until day 5 (June 15)
    runCycles(6 * 86400 + 300);
    query(doc, "1y");
    {
        JsonArray age = doc["ageSec"], rain = doc["rainMm"];
        assert(age.size() == 7);                  // June 10 - 15 + the open June 16
        assert(near(rain[3].as<float>(), 0.1f));  // June 13: one cycle
        assert(rain[4].isNull() && doc["temp"][4].isNull());   // June 14: off
        assert(near(rain[5].as<float>(), 28.8f)); // June 15
        for (int i = 0; i + 2 < 7; i++) assert(age[i].as<long>() - age[i + 1].as<long>() == 86400);
    }

    // Ranges and end on the same data (6 days of 5-min cycles)
    query(doc, "30d");                        // hourly tier, 2-h points
    assert(!strcmp(doc["res"], "1h") && doc["bucketSec"] == 7200);
    {
        JsonArray age = doc["ageSec"], rain = doc["rainMm"];
        size_t n = age.size();
        assert(n >= 72 && n <= 74);           // 6 days + 5 min in 2-h points (+ the open hour)
        assert(age[n - 1] == 0);
        size_t full = 0;
        for (size_t i = 0; i + 1 < n; i++) {
            if (i > 0) assert(age[i - 1].as<long>() - age[i].as<long>() >= 3600);
            if (!rain[i].isNull() && near(rain[i].as<float>(), 2.4f)) full++;
        }
        assert(full >= 45);                   // two hours of 1.2 mm each, merged
    }
    int64_t now;
    assert(epochNow(now));
    char end[24];
    snprintf(end, sizeof(end), "%lld", (long long)(now - 12 * 3600));
    query(doc, "24h", end);                   // ending 12 h ago: from the hourly tier
    assert(!strcmp(doc["res"], "1h") && doc["bucketSec"] == 3600);
    {
        JsonArray age = doc["ageSec"];
        assert(age.size() == 24);
        for (JsonVariant a : age) assert(a.as<long>() >= 12 * 3600 && a.as<long>() < 36 * 3600);   // no open point
        for (size_t i = 0; i < 24; i++) {   // June 14 (off) null, June 15 1.2 mm an hour
            const bool off = now - age[i].as<long>() <= 1781049600 + 5 * 86400;
            assert(off ? doc["rainMm"][i].isNull() : near(doc["rainMm"][i].as<float>(), 1.2f));
        }
    }
    query(doc, "24h", end, "5m");             // forced: the 5-min tier only reaches back 24 h, i.e. 12 h of it
    assert(!strcmp(doc["res"], "5m") && doc["bucketSec"] == 300 && doc["ageSec"].size() == 145);   // ages 12 h .. 24 h
    snprintf(end, sizeof(end), "%lld", (long long)(now - 3 * 86400));
    query(doc, "7d", end);                    // "last week" ending 3 days ago
    assert(!strcmp(doc["res"], "1h") && doc["ageSec"].size() >= 70 && doc["ageSec"].size() <= 73);
    for (JsonVariant a : doc["ageSec"].as<JsonArray>()) assert(a.as<long>() >= 3 * 86400);
    snprintf(end, sizeof(end), "%lld", (long long)(now + 999));
    query(doc, "24h", end);                   // an end in the future = now
    assert(doc["ageSec"].size() == HISTORY_FINE_CAPACITY && doc["ageSec"][HISTORY_FINE_CAPACITY - 1] == 0);   // 287 + open: the 288th ended exactly 24 h ago
    snprintf(end, sizeof(end), "%lld", (long long)(now - 365 * 86400));
    query(doc, "1y", end);                    // last year: nothing yet
    assert(!strcmp(doc["res"], "1d") && doc["ageSec"].size() == 0);

    // Save / restore: restored days, and the open day rebuilt from the
    // restored hours (a restart at noon does not lose the morning).
    useTz("UTC0");
    resetHistory(0);
    hostSynced = true;
    hostEpochAtZero = 1781049600;
    runCycles(2 * 86400 + 12 * 3600 + 300);   // June 12, 12:05
    static uint8_t dayBlob[HISTORY_BLOB_HEADER + HISTORY_DAILY_CAPACITY * sizeof(HistorySample) + 1];
    static uint8_t hourBlob[HISTORY_BLOB_HEADER + HISTORY_HOURLY_CAPACITY * sizeof(HistorySample) + 1];
    const uint32_t genDay = historyGeneration(HT_DAILY), genHour = historyGeneration(HT_HOURLY);
    size_t nd = historyExport(HT_DAILY, dayBlob, sizeof(dayBlob));
    size_t nh = historyExport(HT_HOURLY, hourBlob, sizeof(hourBlob));
    assert(nd == HISTORY_BLOB_HEADER + 2 * sizeof(HistorySample) + 1 && dayBlob[3] == 4 && dayBlob[6] == (1440 & 0xFF) && dayBlob[7] == (1440 >> 8));
    assert(nh == HISTORY_BLOB_HEADER + 60 * sizeof(HistorySample) + 1);
    runCycles(2 * 86400 + 12 * 3600 + 600);
    assert(historyGeneration(HT_DAILY) == genDay && historyGeneration(HT_HOURLY) == genHour);   // nothing new: not saved again
    int64_t savedEpoch;
    assert(epochNow(savedEpoch));
    resetHistory(1000);                       // restart, 10 min later
    hostSynced = true;
    hostEpochAtZero = savedEpoch + 600 - 1;
    assert(historyImport(HT_DAILY, dayBlob, nd) == 2);
    assert(historyImport(HT_HOURLY, hourBlob, nh) == 60);
    assert(historyImport(HT_DAILY, hourBlob, nh) == -1);   // wrong tier
    // the header's version: this one, a newer firmware's (kept by the store), an older one (the hook)
    assert(historyBlobVersionAction(dayBlob, nd) == STORED_TAKE);
    {
        static uint8_t other[sizeof(dayBlob)];
        memcpy(other, dayBlob, nd);
        other[3] = STORED_HISTORY_TIER_VERSION + 1;
        assert(historyBlobVersionAction(other, nd) == STORED_NEWER && historyImport(HT_DAILY, other, nd) == -1);
        other[3] = STORED_HISTORY_TIER_VERSION - 1;
        assert(historyBlobVersionAction(other, nd) == STORED_MIGRATE);
        other[0] = 'X';   // no history header: not a version question, historyImport() refuses it
        assert(historyBlobVersionAction(other, nd) == STORED_TAKE && historyImport(HT_DAILY, other, nd) == -1);
        assert(historyBlobVersionAction(other, 3) == STORED_TAKE && historyBlobVersionAction(nullptr, 0) == STORED_TAKE);
    }
    runCycles(12 * 3600);                     // past the end of June 12
    query(doc, "1y");
    {
        JsonArray rain = doc["rainMm"];
        assert(daily.count == 3);
        assert(near(rain[0].as<float>(), 28.8f) && near(rain[1].as<float>(), 28.8f));
        // June 12: everything but the cycles of the lost open hour and the 10 min off
        assert(rain[2].as<float>() >= 28.3f && rain[2].as<float>() <= 28.65f);
        assert(near(doc["tempMin"][2].as<float>(), 20.0f));   // the restored morning's minimum
    }

    // DST (Europe/Berlin): local days of 23 h at the change in March; a day
    // still is one entry, its rain the sum of its 23 hours.
    useTz("CET-1CEST,M3.5.0,M10.5.0/3");
    resetHistory(0);
    hostSynced = true;
    hostEpochAtZero = 1774566000;   // 2026-03-27 00:00 CET at uptime 0
    runCycles(4 * 86400 - 3600 + 300);   // Mar 27, 28, 29 (23 h), 30 + one cycle
    query(doc, "1y");
    {
        JsonArray age = doc["ageSec"], rain = doc["rainMm"];
        assert(age.size() == 5);
        assert(near(rain[0].as<float>(), 28.8f) && near(rain[1].as<float>(), 28.8f));
        assert(near(rain[2].as<float>(), 27.6f));   // March 29: 23 h
        assert(near(rain[3].as<float>(), 28.8f));
        assert(age[0].as<long>() - age[1].as<long>() == 86400);
        assert(age[1].as<long>() - age[2].as<long>() == 82800);
        assert(age[2].as<long>() - age[3].as<long>() == 86400);
    }
    useTz("UTC0");
}

// ---- The open hour after a restart (rebuilt from the restored 5-min values) ----

// The values of the cycle ending at epoch e: a function of the time only, so
// a run with and one without a restart see the same cycles.
static void setCycleValues(int64_t e)
{
    const long n = (long)(e / 60);
    gSrc.tempValid = gSrc.humValid = gSrc.pressValid = true;
    gSrc.tempC = 5.0f + (n % 17) * 0.7f;
    gSrc.humidity = 40.0f + (n % 23) * 1.5f;
    gSrc.pressSeaHpa = 1000.0f + (n % 13) * 0.9f;
    rainMM = (n % 3) * 0.1f;
}
static void pushCycle()
{
    int64_t e;
    assert(epochNow(e));
    setCycleValues(e);
    pushHistorySample();
}
// 1-min cycles while the next one ends at or before untilEpoch
static void cyclesUntil(int64_t untilEpoch)
{
    int64_t e;
    while (epochNow(e) && e + 60 <= untilEpoch) { delay(60000); pushCycle(); }
}
static const HistorySample& newestHour() { return coarse.ring[(coarse.head + coarse.capacity - 1) % coarse.capacity]; }

// Custom series aggregation (series_def.h): mean / sum / min / max overridden
// per slot act the same in every tier and in the downsampled points. One
// cycle per 5-min bucket, values 0..11 in each hour: an hour is mean 6 (5.5
// rounded to the 1-% step), sum 66, min 0, max 11; a day (24 such hours):
// mean 6, sum 1584, min 0, max 11; two hours merged (30d range): 6, 132, 0, 11.
static void checkSeriesAggregation()
{
    useTz("UTC0");
    SeriesDef defs[SERIES_MAX];
    const uint8_t aggs[4] = { SA_MEAN, SA_SUM, SA_MIN, SA_MAX };
    for (int i = 0; i < 4; i++) {
        snprintf(defs[i].name, sizeof(defs[i].name), "Soil %d", i);
        strcpy(defs[i].sensorInternalId, "L510F65");
        defs[i].field = SF_MOIST;
        seriesSetAgg(defs[i], aggs[i]);
    }
    assert(defs[0].agg == SA_DEFAULT && defs[1].agg == SA_SUM);   // the value's default is not stored
    resetHistory(0);
    hostSynced = true;
    hostEpochAtZero = 1781049600;   // 2026-06-10 00:00 UTC, a day boundary
    historySeriesConfigure(defs, SERIES_MAX);
    assert(serAgg[0] == SA_MEAN && serAgg[1] == SA_SUM && serAgg[2] == SA_MIN && serAgg[3] == SA_MAX);
    float v[SERIES_MAX] = {};
    bool ok[SERIES_MAX] = {};
    for (int n = 0; n < 26 * 12; n++) {
        delay(300000);
        for (int i = 0; i < 4; i++) { v[i] = (float)(n % 12); ok[i] = true; }
        pushHistorySample(v, ok);
    }
    // the values of each slot at JSON point k
    auto at = [](JsonDocument& doc, int slot, size_t k) -> JsonVariant { return doc["series"][slot]["values"][k]; };
    auto expect = [&](JsonDocument& doc, size_t skipEnd, float mean, float sum, float mn, float mx) {
        JsonArray s0 = doc["series"][0]["values"];
        size_t checked = 0;
        for (size_t k = 0; k + skipEnd < s0.size(); k++) {
            if (at(doc, 0, k).isNull()) continue;
            assert(near(at(doc, 0, k).as<float>(), mean) && near(at(doc, 1, k).as<float>(), sum) &&
                   near(at(doc, 2, k).as<float>(), mn) && near(at(doc, 3, k).as<float>(), mx));
            checked++;
        }
        return checked;
    };
    JsonDocument doc;
    query(doc, "24h");   // 5-min tier: the cycle values themselves
    {
        JsonArray s = doc["series"];
        assert(s.size() == 4 && !strcmp(s[0]["agg"], "mean") && !strcmp(s[1]["agg"], "sum") && !strcmp(s[2]["agg"], "min") &&
               !strcmp(s[3]["agg"], "max"));
        assert(s[1]["perBucket"] == true && s[0]["perBucket"].isNull() && s[3]["perBucket"].isNull());
        const size_t last = s[0]["values"].size() - 1;   // the open bucket: the last cycle (value 11), before it 10
        for (int i = 0; i < 4; i++) assert(near(at(doc, i, last).as<float>(), 11.0f) && near(at(doc, i, last - 1).as<float>(), 10.0f));
    }
    query(doc, "24h", nullptr, "1h");            assert(expect(doc, 1, 6, 66, 0, 11) >= 20);    // hourly tier
    query(doc, "30d");                           assert(expect(doc, 2, 6, 132, 0, 11) >= 10);   // hourly, 2 merged per point
    assert(doc["series"][0]["values"].size() < 400);
    query(doc, "1y");                            assert(expect(doc, 1, 6, 1584, 0, 11) == 1);   // daily tier: the one finished day

    // an aggregation change is another definition: the slot starts empty
    const uint32_t before = seriesKey(defs[3]);
    seriesSetAgg(defs[3], SA_MIN);
    assert(seriesKey(defs[3]) != before);
    historySeriesConfigure(defs, SERIES_MAX);
    query(doc, "24h");
    for (JsonVariant x : doc["series"][3]["values"].as<JsonArray>()) assert(x.isNull());
    SeriesDef none[SERIES_MAX];
    historySeriesConfigure(none, SERIES_MAX);
}

static void checkHourRebuild()
{
    useTz("UTC0");
    gCfg.windEn = 0;
    const int64_t T0 = 1781049600;              // 2026-06-10 00:00 UTC; cycles at T0 + 1 + k x 60
    const int64_t R = T0 + 2 * 3600 + 1800;     // restart at 02:30
    const int64_t END = T0 + 3 * 3600 + 1;      // the first cycle after 03:00 closes 02:00 - 03:00

    // without a restart
    resetHistory(1000);
    hostSynced = true;
    hostEpochAtZero = T0;
    cyclesUntil(END);
    const HistorySample ref = newestHour();
    assert(alignedOf(ref.bucketEndSec) == T0 + 3 * 3600 && ref.cycles == 60);

    // the same cycles, saved after the one at R + 1 (it closed the 5-min bucket ending at R)
    resetHistory(1000);
    hostSynced = true;
    hostEpochAtZero = T0;
    cyclesUntil(R + 1);
    static uint8_t fb[HISTORY_BLOB_HEADER + HISTORY_FINE_CAPACITY * sizeof(HistorySample) + 1];
    static uint8_t hb[HISTORY_BLOB_HEADER + HISTORY_HOURLY_CAPACITY * sizeof(HistorySample) + 1];
    const size_t nf = historyExport(HT_FINE, fb, sizeof(fb)), nh = historyExport(HT_HOURLY, hb, sizeof(hb));
    assert(nf && nh);
    // restart: back at R + 1 (the cycle of the 5-min bucket open at the save comes again)
    resetHistory(1000);
    hostSynced = true;
    hostEpochAtZero = R;
    assert(historyImport(HT_HOURLY, hb, nh) == 2);    // 00:00 - 01:00, 01:00 - 02:00
    assert(historyImport(HT_FINE, fb, nf) == 30);     // 00:00 - 02:30
    pushCycle();
    assert(coarse.open->cycles == 1 + 6 * 5);           // this cycle + 02:00 - 02:30 (6 buckets of 5)
    {
        JsonDocument doc;
        query(doc, "7d");                               // the open hour (live point) already holds the restored half hour
        JsonArray age = doc["ageSec"];
        assert(age[age.size() - 1] == 0 && !doc["humMin"][age.size() - 1].isNull());
    }
    cyclesUntil(END);
    const HistorySample got = newestHour();
    assert(alignedOf(got.bucketEndSec) == T0 + 3 * 3600 && got.cycles == 60 && got.validMask == ref.validMask);
    auto within = [](long a, long b) { return labs(a - b) <= 1; };   // one step of the stored scale
    assert(within(got.tempCx10, ref.tempCx10) && within(got.tempMinCx10, ref.tempMinCx10) && within(got.tempMaxCx10, ref.tempMaxCx10));
    assert(within(got.humidityX2, ref.humidityX2) && within(got.humMinX2, ref.humMinX2) && within(got.humMaxX2, ref.humMaxX2));
    assert(within(got.pressureQnhX10, ref.pressureQnhX10) && within(got.pressMinX10, ref.pressMinX10) && within(got.pressMaxX10, ref.pressMaxX10));
    assert(within(got.rainAmountX100, ref.rainAmountX100) && within(got.rainRateX100, ref.rainRateX100));
    assert(ref.tempMinCx10 < ref.tempCx10 && ref.tempCx10 < ref.tempMaxCx10 && ref.humMinX2 < ref.humMaxX2 && ref.pressMinX10 < ref.pressMaxX10);

    // without the 5-min values (only the hourly blob) the hour has only this run's cycles, as before
    resetHistory(1000);
    hostSynced = true;
    hostEpochAtZero = R;
    assert(historyImport(HT_HOURLY, hb, nh) == 2);
    pushCycle();
    cyclesUntil(END);
    assert(newestHour().cycles == 30);
}

// ---- The clock stepped back (time_sync.h: a better time source corrected it) ----

// Every tier: bucket ends strictly increasing (uptime); after a step back
// at most one entry per local day (by the current clock; a step forward
// moves the last day's entry into the next day, as before).
static void checkTiersAfterStep(const char* what, bool distinctDays)
{
    for (Tier* t : TIERS) {
        const size_t start = t->count < t->capacity ? 0 : t->head;
        for (size_t i = 1; i < t->count; i++) {
            const HistorySample& a = t->ring[(start + i - 1) % t->capacity];
            const HistorySample& b = t->ring[(start + i) % t->capacity];
            if (!((int32_t)(b.bucketEndSec - a.bucketEndSec) > 0) ||
                !(windowIndex(*t, alignedOf(b.bucketEndSec)) > windowIndex(*t, alignedOf(a.bucketEndSec)) || t != &daily || !distinctDays)) {
                fprintf(stderr, "%s: tier %u entry %zu: end %u after %u\n", what, (unsigned)t->bucketSec, i, b.bucketEndSec, a.bucketEndSec);
                assert(false);
            }
        }
    }
}

static void checkBackwardStep(int64_t stepBackSec, int64_t atSec)
{
    useTz("UTC0");
    gCfg.windEn = 0;
    const int64_t T0 = 1781049600;   // 2026-06-10 00:00 UTC
    resetHistory(1000);
    hostSynced = true;
    hostEpochAtZero = T0;
    cyclesUntil(T0 + 2 * 86400 + atSec);
    hostEpochAtZero -= stepBackSec;   // the clock goes back
    cyclesUntil(T0 + 4 * 86400 + 600);
    char what[48];
    snprintf(what, sizeof(what), "step %llds at %llds", (long long)stepBackSec, (long long)atSec);
    checkTiersAfterStep(what, stepBackSec > 0);
    // 10 .. 13 June closed once each (by the clock now; a step of more than
    // a day relabels the days before it, their entries stay)
    if (stepBackSec < 86400) assert(daily.count == 4);
    else assert(daily.count == 5);   // + the day before, closed at the step
    // the newest entries sit on window ends again
    assert(alignedOf(coarse.ring[(coarse.head + coarse.capacity - 1) % coarse.capacity].bucketEndSec) % 3600 == 0);
    assert(alignedOf(fine.ring[(fine.head + fine.capacity - 1) % fine.capacity].bucketEndSec) % 300 == 0);
    assert(alignedOf(daily.ring[(daily.head + daily.capacity - 1) % daily.capacity].bucketEndSec) % 86400 == 0);
}

int main()
{
    // the clock steps back: within a window, over an hour, over midnight, by a day
    checkBackwardStep(90, 10 * 3600 + 150);
    checkBackwardStep(200, 10 * 3600 + 3540);
    checkBackwardStep(3600, 1800);       // 00:30 -> 23:30 the day before
    checkBackwardStep(2 * 3600, 1800);   // 00:30 -> 22:30
    checkBackwardStep(600, 60);          // 00:01 -> 23:51
    checkBackwardStep(26 * 3600, 1800);
    checkBackwardStep(-3 * 3600, 1800);  // forward (as at the first sync): unchanged behaviour

    resetHistory(0);
    gCfg.rainEn = 1; gCfg.windEn = 0; gCfg.rfEn = 0;   // rain from the board gauge
    gSrc.tempValid = true; gSrc.tempC = 10.0f;
    bme280Present = true; bme280PressSea = 1013.0f;
    gSrc.pressValid = true; gSrc.pressSeaHpa = 1013.0f;   // the selected pressure source (srcUpdateCycle())
    assert(srcActive(SG_RAIN));

    // 1-min cycles, 0.1 mm each: 1 h = 60 cycles = 6.0 mm at most
    runFor(58 * 60, 60, 0.1f);
    float t, p;
    assert(!historyTempAgo(3600, t));             // only 58 min of data: no "1 h" trend yet
    runFor(9 * 60, 60, 0.1f);                     // 67 min
    assert(historyTempAgo(3600, t) && t == 10.0f);
    assert(!historyPressureAgo(3 * 3600, p));
    runFor(170 * 60, 60, 0.1f);                   // ~3 h 57 min
    assert(historyPressureAgo(3 * 3600, p));
    float h1 = historyRainSum(3600), h24 = historyRainSum(24 * 3600);
    assert(h1 <= 6.0f + 1e-3f && h1 >= 5.4f - 1e-3f);   // at most 1 h, at most one 5-min bucket less
    float total = 0.1f * (58 + 9 + 170);
    assert(std::fabs(h24 - total) < 0.01f);       // shorter than 24 h running: everything since start

    // 10-min cycles: a bucket then spans two cycles (20 min), still not more than 1 h
    runFor(5 * 3600, 600, 1.0f);
    h1 = historyRainSum(3600);
    assert(h1 <= 6.0f + 1e-3f && h1 >= 3.0f);

    // Default 300-s cycle: one cycle per 5-min bucket, 288 buckets = 24 h
    // (before the fix a bucket held two cycles and the ring ~48 h).
    resetHistory(7000);
    runFor(30 * 3600, 300, 0.1f);
    checkRange(false, HISTORY_FINE_CAPACITY, 300, [](size_t) { return 0.1f; });
    // 1-min cycles: 5 per bucket
    resetHistory(7000);
    runFor(25 * 3600, 60, 0.1f);
    checkRange(false, HISTORY_FINE_CAPACITY, 300, [](size_t) { return 0.5f; });
    // 10-min cycles: every other 5-min bucket is a gap, still 24 h
    resetHistory(7000);
    runFor(30 * 3600, 600, 1.0f);
    {
        JsonDocument doc;
        buildHistoryJson(doc, false);
        bool firstIsGap = doc["rainMm"][0].isNull();
        checkRange(false, HISTORY_FINE_CAPACITY, 300, [&](size_t i) { return ((i % 2 == 0) == firstIsGap) ? -1.0f : 1.0f; });
    }
    h1 = historyRainSum(3600);
    assert(h1 >= 5.0f - 1e-3f && h1 <= 6.0f + 1e-3f);   // 1 h = 6 cycles, at most one bucket less
    h24 = historyRainSum(24 * 3600);
    assert(h24 >= 143.0f - 1e-3f && h24 <= 144.0f + 1e-3f);   // 24 h = 144 cycles, not 30 h

    // Hourly ring: 168 buckets = 7 days; 5-min cycles -> 12 per bucket,
    // 2-h cycles -> every other hour a gap.
    resetHistory(7000);
    runFor(9 * 86400, 300, 0.1f);
    checkRange(true, 168, 3600, [](size_t) { return 1.2f; });
    resetHistory(7000);
    runFor(9 * 86400, 7200, 1.0f);
    {
        JsonDocument doc;
        buildHistoryJson(doc, true);
        bool firstIsGap = doc["rainMm"][0].isNull();
        checkRange(true, 168, 3600, [&](size_t i) { return ((i % 2 == 0) == firstIsGap) ? -1.0f : 1.0f; });
    }

    // Time sync: buckets switch from uptime to wall-clock windows without a
    // run of fake gaps; later buckets end on epoch 5-min boundaries.
    resetHistory(7000);
    runFor(2 * 3600, 300, 0.1f);
    hostSynced = true;
    hostEpochAtZero = 1781000123;   // epoch - uptime: not a multiple of 300
    runFor(3 * 3600, 300, 0.1f);
    {
        JsonDocument doc;
        buildHistoryJson(doc, false);
        JsonArray age = doc["ageSec"], rain = doc["rainMm"];
        int64_t epochResp;
        assert(epochNow(epochResp));
        size_t n = age.size(), nulls = 0;
        assert(n >= 59 && n <= 61);   // 5 h of 5-min buckets + the open one
        for (size_t i = 0; i + 1 < n; i++) {
            long d = i + 1 < n - 1 ? age[i].as<long>() - age[i + 1].as<long>() : 1;
            assert(d > 0 && d <= 300);
            if (rain[i].isNull()) nulls++;
            if (age[i].as<long>() < 3 * 3600 - 300) assert((epochResp - age[i].as<long>()) % 300 == 0);
        }
        assert(nulls <= 1);
    }
    // Persistence: export with wall-clock ends, "restart" (uptime from 0,
    // 10 min off), import after the clock sync -> saved windows, gaps for
    // the downtime, then this run's entries; evenly spaced, rain kept.
    resetHistory(7000);
    hostSynced = true;
    hostEpochAtZero = 1781000000;
    runFor(30 * 3600, 300, 0.1f);
    {
        assert(historyExport(HT_FINE, nullptr, 0) == 0);   // too small
        static uint8_t fineBlob[HISTORY_BLOB_HEADER + HISTORY_FINE_CAPACITY * sizeof(HistorySample) + 1];   // + series count (none)
        static uint8_t coarseBlob[HISTORY_BLOB_HEADER + HISTORY_HOURLY_CAPACITY * sizeof(HistorySample) + 1];
        assert(historyBlobMaxSize(HT_FINE) >= sizeof(fineBlob));
        size_t nf = historyExport(HT_FINE, fineBlob, sizeof(fineBlob));
        size_t nc = historyExport(HT_HOURLY, coarseBlob, sizeof(coarseBlob));
        assert(nf == sizeof(fineBlob) && nc > HISTORY_BLOB_HEADER && fineBlob[3] == 4 && fineBlob[nf - 1] == 0);
        int64_t savedEpoch;
        assert(epochNow(savedEpoch));

        resetHistory(1000);                                   // restart
        assert(historyImport(HT_FINE, fineBlob, nf) == -1);   // no clock yet
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;               // 10 min later
        fineBlob[3] = 99; assert(historyImport(HT_FINE, fineBlob, nf) == -1); fineBlob[3] = 4;   // version
        fineBlob[3] = 3; assert(historyImport(HT_FINE, fineBlob, nf) == -1); fineBlob[3] = 4;    // 36-byte samples: dropped
        assert(historyImport(HT_FINE, fineBlob, nf - 1) == -1);                                  // size
        assert(historyImport(HT_HOURLY, fineBlob, nf) == -1);                                    // other tier
        int rf = historyImport(HT_FINE, fineBlob, nf);
        int rc = historyImport(HT_HOURLY, coarseBlob, nc);
        assert(rf > 250 && rc > 20);
        runFor(3600, 300, 0.1f);
        JsonDocument doc;
        buildHistoryJson(doc, false);
        JsonArray age = doc["ageSec"], rain = doc["rainMm"];
        size_t n = age.size(), nulls = 0;
        assert(n == HISTORY_FINE_CAPACITY + 1);
        for (size_t i = 0; i + 2 < n; i++) {
            assert(age[i].as<long>() - age[i + 1].as<long>() == 300);
            if (rain[i].isNull()) nulls++;
            else assert(near(rain[i].as<float>(), 0.1f));
        }
        assert(nulls >= 1 && nulls <= 3);                     // the 10 min off
        assert(age[0].as<long>() >= 24 * 3600 - 300 && age[0].as<long>() <= 24 * 3600 + 300);
        // a blob older than the ring restores nothing
        resetHistory(1000);
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 3 * 86400;
        assert(historyImport(HT_FINE, fineBlob, nf) == 0);
    }

    // Custom series: one value per bucket next to the main values (mean,
    // lightning strikes summed, minimum), gaps without a value, a slot only
    // while defined, restored only into a slot with the same definition.
    {
        SeriesDef defs[SERIES_MAX];
        strcpy(defs[0].name, "Soil"); strcpy(defs[0].sensorInternalId, "L510F65"); defs[0].field = SF_MOIST;
        strcpy(defs[1].name, "Strikes"); strcpy(defs[1].sensorInternalId, "L57013C"); defs[1].field = SF_STRIKES;
        strcpy(defs[2].name, "Dist"); strcpy(defs[2].sensorInternalId, "L57013C"); defs[2].field = SF_DIST;
        resetHistory(1000);
        historySeriesConfigure(defs, 2);          // limit 2: the third slot stays off
        assert(serKey[0] && serKey[1] && !serKey[2] && fine.ser[0] && !fine.ser[2]);
        float v[SERIES_MAX] = {};
        bool ok[SERIES_MAX] = {};
        // 10 cycles of 60 s in two 5-min buckets: moisture 40 / 50 alternating, 1 strike each, the 5th cycle no value
        for (int c = 0; c < 10; c++) {
            delay(60000);
            rainMM = 0.0f;
            v[0] = (c % 2) ? 50.0f : 40.0f; ok[0] = c != 4;
            v[1] = 1.0f; ok[1] = c != 4;
            pushHistorySample(v, ok);
        }
        JsonDocument doc;
        buildHistoryJson(doc, false);
        JsonArray ser = doc["series"];
        assert(ser.size() == 2);
        assert(!strcmp(ser[0]["name"], "Soil") && !strcmp(ser[0]["field"], "moist") && !strcmp(ser[0]["unit"], "%"));
        assert(ser[1]["perBucket"] == true && ser[0]["perBucket"].isNull());
        assert(!strcmp(ser[0]["sensor"], "L510F65") && !strcmp(ser[1]["sensor"], "L57013C"));
        {   // serialized exactly: no padding of the name / ID arrays
            std::string js; serializeJson(doc, js);
            assert(js.find("\"name\":\"Soil\",") != std::string::npos && js.find("\"sensor\":\"L510F65\",") != std::string::npos);
        }
        JsonArray s0 = ser[0]["values"], s1 = ser[1]["values"], age = doc["ageSec"];
        assert(s0.size() == age.size() && s1.size() == age.size());
        // finished buckets: means of 40/50 (whole %), strike sums; the newest (open) bucket too
        float strikes = 0;
        for (size_t i = 0; i < s1.size(); i++) if (!s1[i].isNull()) strikes += s1[i].as<float>();
        assert(near(strikes, 9.0f));                        // 10 cycles, one without a value
        for (size_t i = 0; i < s0.size(); i++) if (!s0[i].isNull()) assert(s0[i].as<float>() >= 40 && s0[i].as<float>() <= 50);

        // export / import with the series (clock synced)
        hostSynced = true;
        hostEpochAtZero = 1781000000;
        for (int c = 0; c < 30; c++) { delay(300000); v[0] = 33.0f; ok[0] = true; ok[1] = false; pushHistorySample(v, ok); }
        static uint8_t blob[HISTORY_BLOB_HEADER + HISTORY_FINE_CAPACITY * sizeof(HistorySample) + 1 + SERIES_MAX * (4 + HISTORY_FINE_CAPACITY * 2)];
        size_t n = historyExport(HT_FINE, blob, sizeof(blob));
        assert(n > 0 && blob[HISTORY_BLOB_HEADER + fine.count * sizeof(HistorySample)] == 2);   // two series in it
        int64_t savedEpoch;
        assert(epochNow(savedEpoch));
        // same definitions after a restart: restored
        resetHistory(1000);
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;
        assert(historyImport(HT_FINE, blob, n) > 20);
        buildHistoryJson(doc, false);
        JsonArray r0 = doc["series"][0]["values"];
        size_t have = 0;
        for (size_t i = 0; i < r0.size(); i++) if (!r0[i].isNull()) { have++; }
        assert(have >= 25 && near(r0[r0.size() - 3].as<float>(), 33.0f));
        // the soil series renamed: a new slot key - its old values are not taken
        strcpy(defs[0].name, "Bed");
        historySeriesConfigure(defs, 2);
        resetHistory(1000);
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;
        assert(historyImport(HT_FINE, blob, n) > 20);
        buildHistoryJson(doc, false);
        JsonArray r1 = doc["series"][0]["values"];
        for (size_t i = 0; i < r1.size(); i++) assert(r1[i].isNull());
        // slots removed: rings freed, no "series" in the JSON
        SeriesDef none[SERIES_MAX];
        historySeriesConfigure(none, SERIES_MAX);
        assert(!fine.ser[0] && !coarse.ser[1] && !daily.ser[0]);
        buildHistoryJson(doc, false);
        assert(doc["series"].isNull());
    }
    checkLongHistory();
    checkHourRebuild();
    checkSeriesAggregation();
    return 0;
}
