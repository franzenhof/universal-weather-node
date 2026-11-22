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
        static uint8_t fineBlob[HISTORY_BLOB_HEADER + HISTORY_FINE_CAPACITY * sizeof(HistorySample) + 1];   // + series count (none)
        static uint8_t coarseBlob[HISTORY_BLOB_HEADER + HISTORY_COARSE_CAPACITY * sizeof(HistorySample) + 1];
        assert(historyBlobMaxSize(false) >= sizeof(fineBlob));
        size_t nf = historyExport(false, fineBlob, sizeof(fineBlob));
        size_t nc = historyExport(true, coarseBlob, sizeof(coarseBlob));
        assert(nf == sizeof(fineBlob) && nc > HISTORY_BLOB_HEADER && fineBlob[3] == 2 && fineBlob[nf - 1] == 0);
        int64_t savedEpoch;
        assert(epochNow(savedEpoch));

        resetHistory(1000);                                   // restart
        assert(historyImport(false, fineBlob, nf) == -1);     // no clock yet
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;               // 10 min later
        fineBlob[3] = 99; assert(historyImport(false, fineBlob, nf) == -1); fineBlob[3] = 2;   // version
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
        // a version-1 blob (rc.8 and older: no series section) is still read
        resetHistory(1000);
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;
        fineBlob[3] = 1;
        assert(historyImport(false, fineBlob, nf - 1) > 250);
        assert(historyImport(false, fineBlob, nf) == -1);   // v1 with a trailing byte: size wrong
        fineBlob[3] = 2;
    }

    // Custom series: one value per bucket next to the main values (mean,
    // lightning strikes summed, minimum), gaps without a value, a slot only
    // while defined, restored only into a slot with the same definition.
    {
        SeriesDef defs[SERIES_MAX];
        strcpy(defs[0].name, "Soil"); defs[0].rfId = 0x510F651B; defs[0].field = SF_MOIST;
        strcpy(defs[1].name, "Strikes"); defs[1].rfId = 0x57013CFF; defs[1].field = SF_STRIKES;
        strcpy(defs[2].name, "Dist"); defs[2].rfId = 0x57013CFF; defs[2].field = SF_DIST;
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
        assert(!strcmp(ser[0]["name"], "Soil") && !strcmp(ser[0]["value"], "moist") && !strcmp(ser[0]["unit"], "%"));
        assert(ser[1]["perBucket"] == true && ser[0]["perBucket"].isNull());
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
        size_t n = historyExport(false, blob, sizeof(blob));
        assert(n > 0 && blob[HISTORY_BLOB_HEADER + fine.count * sizeof(HistorySample)] == 2);   // two series in it
        int64_t savedEpoch;
        assert(epochNow(savedEpoch));
        // same definitions after a restart: restored
        resetHistory(1000);
        hostSynced = true;
        hostEpochAtZero = savedEpoch + 600 - 1;
        assert(historyImport(false, blob, n) > 20);
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
        assert(historyImport(false, blob, n) > 20);
        buildHistoryJson(doc, false);
        JsonArray r1 = doc["series"][0]["values"];
        for (size_t i = 0; i < r1.size(); i++) assert(r1[i].isNull());
        // slots removed: rings freed, no "series" in the JSON
        SeriesDef none[SERIES_MAX];
        historySeriesConfigure(none, SERIES_MAX);
        assert(!fine.ser[0] && !coarse.ser[1]);
        buildHistoryJson(doc, false);
        assert(doc["series"].isNull());
    }
    return 0;
}
