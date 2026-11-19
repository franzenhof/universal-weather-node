#pragma once
// Rain this week (ISO 8601 week, Monday start), this month and this year in
// local time - pure bookkeeping without Arduino/NVS, so the host tests can
// drive it (wx_upload.cpp keeps the totals in RTC RAM and does the NVS I/O).
//
// Also the daily bookkeeping (rain/sunshine today and yesterday, last rain)
// and the rain collected while the clock is unknown: all of it forms one
// RainRecord, which is what the flash copy holds (RAIN_BLOB_*).
//
// Flash budget: rainSaveDue() allows a write only when the record changed
// since the last write AND either an hour has passed since it, the local day
// changed or the copy belongs to an earlier week/month/year - at most 24
// hourly writes plus one at the day roll-over per day, even with rain in
// every measurement cycle. rainFlushDue() adds one write per planned restart
// (OTA, restart command, settings) and restarts the hourly interval. Each
// write is a single small NVS blob in the cfgdata partition.
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Plain aggregate without member initialisers: it also lives in RTC RAM
// (RTC_NOINIT_ATTR), where a constructor would zero it at every boot.
struct RainPeriods {
    int32_t weekKey;    // ISO week-numbering year * 100 + ISO week (e.g. 202647); 0 = not known yet
    int32_t monthKey;   // yyyymm
    int32_t yearKey;    // yyyy
    float   weekMm, monthMm, yearMm;
};

int32_t rainIsoWeekKey(const struct tm& lt);   // from tm_year, tm_yday, tm_wday
int32_t rainMonthKey(const struct tm& lt);
int32_t rainYearKey(const struct tm& lt);

// Moves the totals onto the periods of lt: a total whose period has passed
// (stored key differs) starts again at 0, the others continue - e.g. a new
// month keeps the year; a new year resets month and year, the week only if
// 1 January starts a new ISO week (a week running over New Year continues).
// Returns true if anything changed.
bool rainPeriodsRoll(RainPeriods& p, const struct tm& lt);

// Adds rain (mm, >= 0) to all three totals.
void rainPeriodsAdd(RainPeriods& p, float mm);

bool rainPeriodsEqual(const RainPeriods& a, const RainPeriods& b);

// A plausible copy (from NVS): known keys, finite non-negative totals.
bool rainPeriodsValid(const RainPeriods& p);

// Daily bookkeeping in local days. Plain aggregate (RTC RAM, see above).
struct RainDaily {
    int32_t ymd;           // local date (yyyymmdd) the totals belong to; 0 = not known yet
    float   mm;            // rain today
    int32_t prevYmd;       // date of the "yesterday" values; 0 = unknown
    float   prevMm;        // rain yesterday
    float   sunSec;        // sunshine today, s
    float   prevSunSec;    // sunshine yesterday, s
    int64_t lastRainEpoch; // UTC time of the last rain increment, 0 = unknown
};

// Everything the flash copy holds. pendingMm/pendingSunSec: collected while
// the clock was unknown (date of "today" not known yet), added to today at
// the next roll-over with a known clock.
struct RainRecord {
    RainDaily   d;
    RainPeriods p;
    float       pendingMm, pendingSunSec;
};

int32_t rainYmd(const struct tm& lt);           // yyyymmdd
int32_t rainYmdDayNumber(int32_t ymd);          // days since 1970-01-01 (proleptic Gregorian)

// Moves "today" to "yesterday" when the local date has moved on by exactly
// one day; after a longer gap (node off) "yesterday" is cleared (unknown).
// A never dated record (ymd 0) just gets the date. Returns true if changed.
bool rainDailyRoll(RainDaily& d, int32_t ymd);

// Roll-over of the whole record onto the local date lt: daily, week/month/
// year, then the pending rain/sunshine goes into today (and the periods).
// Returns true if anything changed.
bool rainRecordRoll(RainRecord& r, const struct tm& lt);

// Rain of one measurement cycle (mm, > 0 counts). clockKnown false: into
// the pending bucket (the date of today is not known yet). epoch: UTC now.
void rainRecordAddRain(RainRecord& r, float mm, bool clockKnown, int64_t epoch);
void rainRecordAddSun(RainRecord& r, float sec, bool clockKnown);

bool rainDailyValid(const RainDaily& d);
bool rainRecordEqual(const RainRecord& a, const RainRecord& b);
// Worth a flash copy: dated periods (clock was known once) and a plausible rest.
bool rainRecordValid(const RainRecord& r);

// Flash copy (NVS blob, little-endian, explicit field offsets - independent
// of struct padding):
//   version 2 (RAIN_BLOB_SIZE = 68 bytes): u16 version, u16 size (total
//     blob length), RainDaily (i32 ymd, f32 mm, i32 prevYmd, f32 prevMm,
//     f32 sunSec, f32 prevSunSec, i64 lastRainEpoch), RainPeriods (i32 week,
//     month, year keys, f32 week/month/year mm), f32 pendingMm, f32 pendingSunSec.
//   version 1 (28 bytes, older firmware, key "periods"): u32 version = 1 +
//     RainPeriods only - read and migrated (daily values empty), overwritten
//     by a version 2 blob under the same key at the next write.
// A different version or size (future layout) is not taken: it must bump
// the version and migrate here.
constexpr uint16_t RAIN_BLOB_VERSION = 2;
constexpr size_t   RAIN_BLOB_SIZE    = 68;
constexpr size_t   RAIN_BLOB_V1_SIZE = 28;
void rainBlobEncode(const RainRecord& r, uint8_t out[RAIN_BLOB_SIZE]);
// Returns the version read (2, or 1 = migrated from the old periods-only
// blob) or 0 if the blob is not usable (out unchanged then).
int  rainBlobDecode(const uint8_t* b, size_t len, RainRecord& out);

constexpr int64_t RAIN_SAVE_MIN_INTERVAL_SEC = 3600;

struct RainSaveState {
    bool        saved = false;      // NVS holds `last` (written or loaded at boot)
    RainRecord  last = {};          // what NVS holds
    int64_t     lastSaveSec = 0;    // uptime (s) of the last write / of the boot load
    int32_t     lastYmd = 0;        // local date (yyyymmdd) of the previous check; 0 = none yet
};

// Call once per measurement cycle with the current record (clock known),
// the uptime in seconds and the local date. Returns true if the record must
// be written to NVS now and updates st as written. persistEn false (setting
// "rainPersist" off): never true - the totals then live in RTC RAM only.
bool rainSaveDue(RainSaveState& st, const RainRecord& cur, int64_t nowSec, int32_t ymd, bool persistEn);

// Right before a planned restart (OTA, restart command, settings): true if
// the record differs from the copy in NVS (and is worth saving) - written
// once, a second call without changes returns false. Restarts the hourly
// interval like any write. Never true with persistEn false.
bool rainFlushDue(RainSaveState& st, const RainRecord& cur, int64_t nowSec, bool persistEn);

// The write failed (NVS full / error): retried at the next hourly slot.
void rainSaveFailed(RainSaveState& st);
