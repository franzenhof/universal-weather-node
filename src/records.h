#pragma once
// Records (extremes) of the main values: minimum / maximum with time for
// today, yesterday, this month, this year and all time - temperature,
// humidity, pressure (QNH), gust and rain rate (maximum only) - plus the
// largest daily rain of the month, year and all time. Day / month / year
// roll over at local midnight like the rain statistics (needs the clock).
//
// Pure part (Records, roll-over, update, blob, JSON) host-tested in
// test/firmware_records.cpp; the device part (records_store.cpp) updates
// them every base cycle, keeps them in one NVS blob (namespace "records",
// written at most hourly, at the day change and before planned restarts)
// and serves GET /api/records.
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <ArduinoJson.h>
#include "config.h"

enum RecQty    : uint8_t { RQ_TEMP = 0, RQ_HUM, RQ_PRESS, RQ_GUST, RQ_RAINRATE, RQ_COUNT };
enum RecPeriod : uint8_t { RP_TODAY = 0, RP_YESTERDAY, RP_MONTH, RP_YEAR, RP_ALL, RP_COUNT };
constexpr bool recHasMin(uint8_t q) { return q <= RQ_PRESS; }   // gust / rain rate: maximum only

struct RecVal { float v; uint32_t t; };   // t: UTC seconds, 0 = none

struct Records {
    uint32_t ymd = 0;                     // local date "today" belongs to (yyyymmdd), 0 = never set
    RecVal   mn[RQ_COUNT][RP_COUNT] = {};
    RecVal   mx[RQ_COUNT][RP_COUNT] = {};
    RecVal   rainDay[3] = {};             // largest daily rain: month, year, all time (t = local date yyyymmdd)
};

// New local date: yesterday = today, today cleared; a new month / year clears
// those periods. Returns true if anything changed.
bool recordsRoll(Records& r, const struct tm& local);
// One value at UTC time t into today, month, year and all time.
void recordsUpdate(Records& r, RecQty q, float v, uint32_t t);
// Today's rain so far (mm, monotonic within the day): the daily-rain maxima
// (their t = the local date yyyymmdd).
void recordsDailyRain(Records& r, float mm);
// allTime = false: today ... this year; true: everything.
void recordsReset(Records& r, bool allTime);

constexpr size_t RECORDS_BLOB_SIZE = sizeof(Records) + 8;
void recordsEncode(const Records& r, uint8_t* out);                // RECORDS_BLOB_SIZE bytes
bool recordsDecode(const uint8_t* in, size_t len, Records& out);   // false = not a valid blob
// A blob of a newer firmware ("REC2" and up, any length): kept untouched by
// this one until the user resets all records (storage_layout.h).
bool recordsBlobIsNewer(const uint8_t* in, size_t len);

// GET /api/records (docs/api/schemas/records.schema.json), units as configured.
void recordsJson(JsonDocument& doc, const Records& r, const Config& c, bool clockValid);

// ---- device part (records_store.cpp) ----
void recordsSetup();                // load the NVS copy
void recordsCycle();                // every base cycle (main loop)
void recordsFlush();                // before a planned restart (systemRestart())
void recordsResetRequest(bool allTime);
void recordsSnapshot(Records& out);
// The records while the clock is valid and today is known (else false); for /api/data.
bool recordsToday(Records& out);
