#pragma once

#include <ArduinoJson.h>

// Two-tier in-RAM ring buffer of bucketed measurement values, feeding the
// /chart web page and /api/history endpoint. Kept across restarts by
// history_store.cpp (NVS, with wall-clock timestamps; restored once the
// clock is synced). Long-term history remains Grafana's job via
// MQTT/ChirpStack; this is a short local window for the device's own web UI.
//
// Classic RRDtool-style tiering rather than one long flat buffer: a "fine"
// tier keeps short buckets for a detailed recent view, a "coarse" tier keeps
// long buckets for a full week at a fraction of the RAM a flat buffer of
// the same duration/resolution would need. Each pushHistorySample() call
// merges the just-finished cycle's values into *both* tiers' bucket whose
// window contains the cycle's end time (running average, except wind gust =
// running max and rain amount = running sum — see history.cpp). Windows are
// fixed, back to back: (k x bucketSec, (k+1) x bucketSec] on the wall clock
// (epoch) once it is synced, on the uptime before. One ring entry per
// window, also for a window without any cycle or value (a gap: null in the
// JSON, never 0), so 288 entries are exactly 24 h and 168 exactly 7 days,
// whatever the cycle length: 60-s cycles put 5 cycles into a 5-min bucket,
// 600-s cycles leave every other 5-min bucket a gap. The open (current)
// bucket is served as the newest point (ageSec 0).
constexpr size_t HISTORY_FINE_CAPACITY        = 288;  // 288 x 5 min   = 24h
constexpr unsigned long HISTORY_FINE_BUCKET_SEC   = 300;
constexpr size_t HISTORY_COARSE_CAPACITY      = 168;  // 168 x 1h      = 7 days
constexpr unsigned long HISTORY_COARSE_BUCKET_SEC = 3600;

// Call once per completed measurement cycle (same point mqttPublishTelemetry()
// consumes the finished cycle's values from) — reads already-computed
// globals, no new sensor access.
void pushHistorySample();

// Serializes one tier's ring buffer (oldest first) as parallel arrays plus
// one set of sibling *Unit fields, converting from stored base units via
// units.h at serialize time — same convention as buildJsonPayload().
// coarse=false -> fine/24h tier, coarse=true -> coarse/7-day tier.
void buildHistoryJson(JsonDocument& doc, bool coarse);

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
size_t historyBlobMaxSize(bool coarse);
// Bytes written to out; 0 = clock not synced yet, tier empty or cap too small.
size_t historyExport(bool coarse, uint8_t* out, size_t cap);
// Puts saved windows in front of this run's entries (gaps in between); needs
// the synced clock. Returns the number of restored entries, -1 = invalid blob
// or no clock.
int historyImport(bool coarse, const uint8_t* in, size_t n);

// history_store.cpp (device): historyStoreCycle() once per cycle before
// pushHistorySample() - restores after the clock sync, saves the 7-day tier
// every 24 h; historyStoreFlush() before a planned restart (both tiers; the
// 24-h tier only as a hand-over, removed after the restore).
void historyStoreCycle();
void historyStoreFlush();
// Settings -> System: delete the chart history (both ranges, RAM and flash).
// Only a request (web task); done by the next historyStoreCycle() in the loop.
void historyClearRequest();
// Empties both rings (history.cpp; called from the loop).
void historyClear();
