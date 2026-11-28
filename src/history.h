#pragma once

#include <ArduinoJson.h>
#include "series_def.h"
#include "storage_layout.h"

// Three-tier in-RAM ring buffers of bucketed measurement values, feeding the
// dashboard and /api/history. Kept across restarts by history_store.cpp (own
// NVS partition, wall-clock timestamps; restored once the clock is synced).
//
// Classic RRDtool-style tiering rather than one long flat buffer:
//   fine    5-min buckets, 288 = 24 h       (detailed recent view, trends)
//   hourly  1-h buckets,   840 = 35 days    (7d / 30d views, "this week vs last week")
//   daily   local days,    731 = 2 years    (1y view, "this year vs last year")
// Each pushHistorySample() call merges the just-finished cycle's values into
// the fine and the hourly tier's bucket whose window contains the cycle's end
// time (running average, except wind gust = running max, rain amount =
// running sum; temperature, humidity and pressure also min / max - see
// history.cpp). Windows are
// fixed, back to back: (k x bucketSec, (k+1) x bucketSec] on the wall clock
// (epoch) once it is synced, on the uptime before. One ring entry per window,
// also for a window without any cycle or value (a gap: null in the JSON,
// never 0), so 288 entries are exactly 24 h whatever the cycle length. The
// open (current) bucket is served as the newest point (ageSec 0).
//
// The daily tier is fed from the hourly tier (each finished hour, weight 1),
// not from the cycles: a day is the mean of its hours, gust and the maxima
// the maximum of the hourly ones, the minima the minimum, rain the sum. Days are LOCAL calendar
// days (gCfg.timeZone; 23 / 25 h at the DST changes), so a day's rain and
// min / max match "rain today" and the records; a day closes as soon as its
// last hour has closed. After a restart the open day is rebuilt from the
// restored hourly entries and the open hour from the restored 5-min entries
// (each weighted by its number of cycles), so a restart loses neither.
// Without a synced clock there are no days (the daily tier stays empty).
// Half-hour time zones: the day boundary falls within an hour - that hour
// counts for the day in which it ends.
//
// Memory (all bigAlloc(): PSRAM on the V4, also the three open buckets),
// HistorySample = 44 bytes, no padding: per bucket mean, min and max of
// temperature (int16 0.1 °C), humidity (uint8 0.5 %, like the mean: 0..127.5)
// and pressure (QNH, uint16 0.1 hPa, like the mean: 0..6553.5 - no offset
// needed), plus the number of cycles (uint16, weights the rebuilt open hour):
//   fine 12.4 KB + hourly 36.1 KB + daily 31.4 KB = 79.9 KB (was 65.4 KB at 36 B);
//   per custom series 3.6 KB (int16 per entry in all three tiers), 32 = 116 KB,
//   so at most 196 KB of the 2 MB PSRAM (+ 1.1 KB for the definitions' copy).
// Flash (history_store.cpp): blobs 12.7 / 37.0 / 32.2 kB (+ 18.6 / 53.9 / 46.9 kB
// with 32 series); each tier saved only when it has changed - see there.
// The aggregation of a custom series (series_def.h: mean, min, max, sum) is
// the same in every step: cycles into a 5-min / 1-h bucket, hours into a day,
// buckets into a downsampled point.
enum HistoryTier : uint8_t { HT_FINE = 0, HT_HOURLY, HT_DAILY, HT_COUNT };
constexpr size_t HISTORY_FINE_CAPACITY          = 288;   // 288 x 5 min = 24 h
constexpr unsigned long HISTORY_FINE_BUCKET_SEC   = 300;
constexpr size_t HISTORY_HOURLY_CAPACITY        = 840;   // 840 x 1 h   = 35 days
constexpr unsigned long HISTORY_HOURLY_BUCKET_SEC = 3600;
constexpr size_t HISTORY_DAILY_CAPACITY         = 731;   // 731 days    = 2 years
constexpr unsigned long HISTORY_DAILY_BUCKET_SEC  = 86400;   // nominal: local days
// /api/history returns at most this many finished points per series; longer
// ranges are downsampled on the node (consecutive buckets merged).
constexpr size_t HISTORY_MAX_POINTS = 400;

// /api/history?range=24h|7d|30d|1y[&end=<unix s>][&res=5m|1h|1d]
struct HistoryRangeDef { const char* name; uint32_t sec; };
constexpr int HISTORY_RANGE_COUNT = 4;
extern const HistoryRangeDef HISTORY_RANGES[HISTORY_RANGE_COUNT];   // 24h, 7d, 30d, 1y
extern const char* const HISTORY_TIER_NAMES[HT_COUNT];              // "5m", "1h", "1d"
struct HistoryQuery {
    const char* range = "24h";   // one of HISTORY_RANGES (static string)
    uint32_t rangeSec = 86400;
    int64_t  endEpoch = 0;       // window end (unix s); 0 = now
    int8_t   res = -1;           // HistoryTier to read; -1 = auto
};
// Parses the query parameters (nullptr or "" = absent). An unknown range is
// 24h (as before); false = a malformed end or an unknown res (HTTP 400).
bool historyParseQuery(const char* range, const char* end, const char* res, HistoryQuery& q);
// Which tier serves a range and how many of its buckets make one point:
// res, else the finest tier whose capacity reaches back depthSec (= now -
// end + range); group = ceil(range / bucket / HISTORY_MAX_POINTS).
void historyPlan(uint32_t rangeSec, uint32_t depthSec, int res, int& tier, uint32_t& group);
uint32_t historyTierBucketSec(int tier);
size_t   historyTierCapacity(int tier);
// Largest number of points (finished, without the open one) a range returns at end = now.
size_t   historyRangeMaxPoints(uint32_t rangeSec);

// Call once per completed measurement cycle (same point mqttPublishTelemetry()
// consumes the finished cycle's values from) — reads already-computed
// globals, no new sensor access.
//
// Custom series (series_def.h): seriesV / seriesOk = the cycle's value per
// slot (series.h seriesNowValues() / seriesNowOk()), nullptr = none.
void pushHistorySample(const float* seriesV = nullptr, const bool* seriesOk = nullptr);

// Custom series: which slots exist (defs, first `limit` usable) and how
// they aggregate. A slot whose definition changed (seriesKey) starts empty; its rings (int16 per
// bucket, 3.6 KB for the three tiers, bigAlloc) exist only while it is defined.
// In setup() after the config, and after the settings were saved.
void historySeriesConfigure(const SeriesDef defs[SERIES_MAX], int limit);

// Allocates the rings (PSRAM if active, psram.h) - once in setup(), after
// the core has started the PSRAM; every other call also does it on first use.
void historyAllocate();

// /api/history: the points of q's window, oldest first, as parallel arrays
// plus one set of sibling *Unit fields, converting from stored base units via
// units.h at serialize time - same convention as buildJsonPayload(). Points
// are the buckets whose end lies in (end - range, end]; with more than
// HISTORY_MAX_POINTS of them, `group` consecutive buckets (aligned on the
// wall clock) are merged into one point. The open bucket only at end = now.
// end needs the synced clock (ignored without).
void buildHistoryJson(JsonDocument& doc, const HistoryQuery& q);

// Lookups in the fine tier (5-min buckets, 24 h) for trends and rolling sums.
// Value about ageSec ago (closest non-gap bucket ending between ageSec - 5 min
// and ageSec + 10 min ago); false = no data that old yet (e.g. less than
// about 3 h since start for the pressure tendency, 1 h for the temperature trend).
bool historyPressureAgo(uint32_t ageSec, float& qnhHpa);   // QNH, hPa
bool historyTempAgo(uint32_t ageSec, float& tempC);        // selected temperature source, °C
// Rain of the selected source over the last windowSec at most: finished
// buckets lying completely within the window plus the open one, so it may
// miss up to one 5-min bucket at the old end, but never covers more. mm;
// since start if the node runs shorter.
float historyRainSum(uint32_t windowSec);

// Persistence: one tier as a blob with wall-clock (epoch) bucket ends.
constexpr size_t HISTORY_BLOB_HEADER = 12;
size_t historyBlobMaxSize(HistoryTier tier);
// Bytes written to out; 0 = clock not synced yet, tier empty or cap too small.
size_t historyExport(HistoryTier tier, uint8_t* out, size_t cap);
// Puts saved windows in front of this run's entries (gaps in between); needs
// the synced clock. Returns the number of restored entries, -1 = invalid blob
// (also one of an older format: dropped) or no clock.
int historyImport(HistoryTier tier, const uint8_t* in, size_t n);
// What a stored tier's "UWH" + version header says (storage_layout.h):
// STORED_NEWER = a newer firmware's tier (kept, never overwritten by this
// one), STORED_MIGRATE = an older one (the hook), STORED_TAKE = this version
// or no history header at all (historyImport() decides).
StoredVersionAction historyBlobVersionAction(const uint8_t* in, size_t n);
// Changes with every new entry of the tier (and an import / clear): the
// store saves a tier only when this differs from its last save.
uint32_t historyGeneration(HistoryTier tier);

// history_store.cpp (device): historyStoreCycle() once per cycle before
// pushHistorySample() - restores after the clock sync, then saves every
// 30 min the tiers that changed; historyStoreFlush() before a planned restart.
void historyStoreCycle();
void historyStoreFlush();
// Settings -> System: delete the chart history (all tiers, RAM and flash).
// Only a request (web task); done by the next historyStoreCycle() in the loop.
void historyClearRequest();
// Empties all rings (history.cpp; called from the loop).
void historyClear();
