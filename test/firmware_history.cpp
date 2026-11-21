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

// Empty history, clock not synced, uptime phaseMs past a 5-min boundary.
static void resetHistory(unsigned long phaseMs)
{
    fine.head = fine.count = 0;     fine.open = OpenBucket{};
    coarse.head = coarse.count = 0; coarse.open = OpenBucket{};
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

int main()
{
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
    checkRange(true, HISTORY_COARSE_CAPACITY, 3600, [](size_t) { return 1.2f; });
    resetHistory(7000);
    runFor(9 * 86400, 7200, 1.0f);
    {
        JsonDocument doc;
        buildHistoryJson(doc, true);
        bool firstIsGap = doc["rainMm"][0].isNull();
        checkRange(true, HISTORY_COARSE_CAPACITY, 3600, [&](size_t i) { return ((i % 2 == 0) == firstIsGap) ? -1.0f : 1.0f; });
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
        assert(historyExport(false, nullptr, 0) == 0);   // too small
        static uint8_t fineBlob[HISTORY_BLOB_HEADER + HISTORY_FINE_CAPACITY * sizeof(HistorySample)];
        static uint8_t coarseBlob[HISTORY_BLOB_HEADER + HISTORY_COARSE_CAPACITY * sizeof(HistorySample)];
        size_t nf = historyExport(false, fineBlob, sizeof(fineBlob));
        size_t nc = historyExport(true, coarseBlob, sizeof(coarseBlob));
        assert(nf == sizeof(fineBlob) && nc > HISTORY_BLOB_HEADER);
        int64_t savedEpoch;
        assert(epochNow(savedEpoch));

        resetHistory(1000);                                   // restart
        assert(historyImport(false, fineBlob, nf) == -1);     // no clock yet
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;               // 10 min later
        fineBlob[3] = 99; assert(historyImport(false, fineBlob, nf) == -1); fineBlob[3] = 1;   // version
        assert(historyImport(false, fineBlob, nf - 1) == -1);                                  // size
        int rf = historyImport(false, fineBlob, nf);
        int rc = historyImport(true, coarseBlob, nc);
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
        assert(historyImport(false, fineBlob, nf) == 0);
    }
    return 0;
}
