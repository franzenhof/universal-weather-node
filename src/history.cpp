#include "history.h"
#include "globals.h"
#include "units.h"
#include "board_io.h"
#include "sources.h"
#include "uv_calibration.h"
#include "api_info.h"
#include "time_sync.h"
#include "psram.h"
#include "storage_layout.h"
#include <Arduino.h>
#include <new>
#include <DallasTemperature.h>
#include <esp_timer.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

namespace {

// Seconds since boot from the 64-bit esp_timer. millis()/1000 wraps from
// 4294967 back to 0 after 49.7 days while bucket ages are computed modulo
// 2^32, which would turn every pre-wrap bucket's age into ~4.29e9 s.
uint32_t uptimeSec()
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

// Compact scaled storage, reusing the exact scaling conventions already
// established in the compact binary LoRaWAN format (lora_payload.cpp): x100
// for wind/rain rate, x10 for temp/pressure, x2 for humidity. validMask marks
// which groups were actually enabled/detected during the bucket, so a
// sensor that's off leaves a clean "no data" gap instead of a misleading 0.
// DS18B20 is the one exception — it's per-*probe* hot-plug rather than a
// single on/off group, so each of the 3 slots uses its own sentinel value
// (DS18B20_INVALID_X10) instead of a validMask bit; same idea as
// DEVICE_DISCONNECTED_C, which is exactly where that sentinel is checked.
struct HistorySample {   // ordered by size: 44 bytes without padding
    uint32_t bucketEndSec;      // end of the bucket's time window, in uptimeSec()
    int16_t  windSpeedX100;     // average over the bucket
    int16_t  windGustX100;      // max over the bucket
    uint16_t windDirDeg;        // last valid reading in the bucket (not averaged — bearings wrap)
    int16_t  rainRateX100;      // average over the bucket
    uint16_t rainAmountX100;    // *sum* over the bucket (mm accumulated, not a rate)
    int16_t  tempCx10;          // average over the bucket
    int16_t  tempMinCx10;       // lowest cycle value in the bucket (daily tier: of the hourly minima)
    int16_t  tempMaxCx10;       // highest
    uint16_t pressureQnhX10;    // average over the bucket
    uint16_t pressMinX10;       // lowest cycle value (QNH x10, like the mean; daily: of the hourly minima)
    uint16_t pressMaxX10;       // highest
    uint16_t batteryVoltX100;   // average over the bucket
    uint16_t uvVoltsX1000;      // average over the bucket
    uint16_t uvIndexX100;       // average over the bucket; 0xFFFF if uncalibrated
    int16_t  ds18b20X10[3];     // average over the bucket, per probe slot
    uint16_t cycles;            // measurement cycles merged (daily: those of its hours, saturated);
                                // the weight of a 5-min bucket when the open hour is rebuilt (primeHour())
    uint8_t  validMask;         // 0 = a gap (no cycle, or no value at all, in that window)
    uint8_t  humidityX2;        // average over the bucket
    uint8_t  humMinX2;          // lowest cycle value (x2, like the mean; daily: of the hourly minima)
    uint8_t  humMaxX2;          // highest
};
static_assert(sizeof(HistorySample) == 44, "no padding in the history rings");

constexpr uint8_t VALID_WIND   = 1 << 0;
constexpr uint8_t VALID_RAIN   = 1 << 1;
constexpr uint8_t VALID_BME280 = 1 << 2;
constexpr uint8_t VALID_LIPO   = 1 << 3;
constexpr uint8_t VALID_UV     = 1 << 4;   // UV index and/or voltage (see the two below)
constexpr uint8_t VALID_HUM    = 1 << 5;   // humidity (VALID_BME280 = temperature; may come from a wireless sensor)
constexpr uint8_t VALID_PRESS  = 1 << 6;   // pressure (BME280 only)
constexpr uint8_t VALID_UVV    = 1 << 7;   // UV terminal voltage (board UV input only)

// Defensive clamps before scaling to their fixed-width int fields, same
// rationale as clampLppAnalog() in lora_payload.cpp — cheap insurance
// against a silent wraparound rather than an actual expected case.
constexpr float RAIN_RATE_LIMIT_MMH = 327.67f;
constexpr float RAIN_AMOUNT_LIMIT_MM = 655.35f;   // per bucket (x100 in a uint16)

// -127.0°C x10 — DEVICE_DISCONNECTED_C itself, already well outside a
// DS18B20's real -55..125°C range, reused as the "no reading" sentinel for
// an individual probe slot's stored/serialized value.
constexpr int16_t DS18B20_INVALID_X10 = (int16_t)(DEVICE_DISCONNECTED_C * 10.0f);

// Accumulates the current, not-yet-finalized bucket for one tier. Wind
// speed/rain rate/temp/humidity/pressure/battery/DS18B20 are running
// averages (sum + per-field count, so a sensor toggled mid-bucket doesn't
// dilute the average with zeros); wind gust is a running *max* (a gust
// averaged away would defeat the point of tracking it); rain amount is a
// running *sum* (it's an accumulated quantity, not an instantaneous one).
struct OpenBucket {
    bool     started = false;
    int64_t  index = 0;          // bucket number on the aligned clock (see alignedNow())
    uint32_t endSec = 0;         // end of its window, in uptimeSec()
    bool     haveClosed = false; // not started: the window closed last (backward clock steps, openFor())
    int64_t  closedIndex = 0;
    uint16_t windCount = 0, rainCount = 0, bmeCount = 0, lipoCount = 0;
    float    windSpeedSum = 0, windGustMax = 0;
    uint16_t windDirLast = 0;
    float    rainRateSum = 0, rainAmountSum = 0;
    float    tempSum = 0, humiditySum = 0, pressureSum = 0, batterySum = 0;
    float    tempMin = 0, tempMax = 0;   // valid with bmeCount
    uint16_t humCount = 0, pressCount = 0;
    float    humMin = 0, humMax = 0;     // valid with humCount
    float    pressMin = 0, pressMax = 0; // valid with pressCount
    uint32_t cycles = 0;                 // cycles merged (HistorySample::cycles)
    float    uvVoltsSum = 0, uvIndexSum = 0;
    uint16_t uvCount = 0, uvIndexCount = 0;   // uvCount: board voltage samples
    float    ds18b20Sum[3] = {0, 0, 0};
    uint16_t ds18b20Count[3] = {0, 0, 0};
    // custom series (series_def.h): SA_MEAN = sum / count, SA_SUM = sum, SA_MIN / SA_MAX = extreme
    float    sAcc[SERIES_MAX] = {};
    uint16_t sCount[SERIES_MAX] = {};
};

// One tier: a ring of finished buckets, one entry per bucket window - also
// for a window without data (a gap entry, validMask 0), so `capacity`
// entries always span capacity x bucketSec and the entries are evenly
// spaced in time - plus the open bucket.
struct Tier {
    HistorySample* ring;
    size_t   capacity;
    uint32_t bucketSec;         // nominal (daily: local days of 23 - 25 h)
    bool     localDays;         // windows are local calendar days, not k x bucketSec
    size_t   head;
    size_t   count;
    OpenBucket* open;           // the open bucket (bigAlloc: PSRAM, like the rings)
    int16_t* ser[SERIES_MAX];   // custom series: one value per ring entry (same index), nullptr = slot unused
    uint32_t gen;               // historyGeneration()
};

// The configured custom series (historySeriesConfigure()): a copy, so the
// rings and their meaning change together under historyMutex (bigAlloc,
// with the rings: 32 x 34 B of PSRAM instead of internal RAM).
SeriesDef* serDef = nullptr;
uint8_t   serAgg[SERIES_MAX] = {};   // seriesAgg(serDef[i])
uint32_t  serKey[SERIES_MAX] = {};   // 0 = slot unused

// The rings (80 KB) and the open buckets come from bigAlloc() (psram.h): PSRAM on a board that
// has it, internal RAM otherwise - allocated in setup() (historyAllocate(),
// after the core has started the PSRAM), at the latest on first use.
Tier fine   = { nullptr, HISTORY_FINE_CAPACITY,   HISTORY_FINE_BUCKET_SEC,   false, 0, 0, nullptr, {}, 0 };
Tier coarse = { nullptr, HISTORY_HOURLY_CAPACITY, HISTORY_HOURLY_BUCKET_SEC, false, 0, 0, nullptr, {}, 0 };   // hourly
Tier daily  = { nullptr, HISTORY_DAILY_CAPACITY,  HISTORY_DAILY_BUCKET_SEC,  true,  0, 0, nullptr, {}, 0 };
Tier* const TIERS[HT_COUNT] = { &fine, &coarse, &daily };

// The daily tier has taken over the hourly entries up to the newest one
// (primeDaily()); false after an import / clear and before the clock sync.
bool dailyPrimed = false;

// historyImport() of the 5-min tier restored windows ending at or before
// this epoch: primeHour() takes those of the open hour over (next cycle).
bool    hourPrimePending = false;
int64_t hourPrimeUntil = 0;

void ensureRings()
{
    for (Tier* t : TIERS) {
        if (t->ring) continue;
        t->ring = (HistorySample*)bigAlloc(t->capacity * sizeof(HistorySample));
        t->open = (OpenBucket*)bigAlloc(sizeof(OpenBucket));
        if (!t->ring || !t->open) abort();   // 81 KB right after the start: cannot fail on a working board
        memset(t->ring, 0, t->capacity * sizeof(HistorySample));
        *t->open = OpenBucket{};
    }
    if (!serDef) {
        void* p = bigAlloc(SERIES_MAX * sizeof(SeriesDef));
        if (!p) abort();
        serDef = (SeriesDef*)p;
        for (int i = 0; i < SERIES_MAX; i++) new (&serDef[i]) SeriesDef();
    }
}

// One value into a slot's accumulator: mean / sum add (the mean with weight
// w), min / max keep the extreme; count = the weight for a mean, else 1 per value.
void accSeries(OpenBucket& b, int i, float v, uint16_t w)
{
    const uint8_t agg = serAgg[i];
    const bool first = !b.sCount[i];
    if (agg == SA_MIN)       b.sAcc[i] = first ? v : fminf(b.sAcc[i], v);
    else if (agg == SA_MAX)  b.sAcc[i] = first ? v : fmaxf(b.sAcc[i], v);
    else if (agg == SA_MEAN) b.sAcc[i] += v * w;
    else                     b.sAcc[i] += v;   // SA_SUM: a count per bucket
    b.sCount[i] += agg == SA_MEAN ? w : 1;
}

// Bucket alignment. Buckets are fixed windows of the "aligned clock" =
// uptime + clockOffset: (k x bucketSec, (k + 1) x bucketSec]. Before the
// first time sync the offset is 0 (windows aligned to uptime); once the
// clock is synced it is epoch - uptime, so the windows sit on wall-clock
// boundaries (:00, :05, ... and full hours, UTC = local for whole-hour time
// zones). The offset is kept unless it moves by more than 2 s: the two
// clocks' seconds tick independently, so their difference jitters by 1 s.
int64_t clockOffset = 0;
bool    clockOffsetFromEpoch = false;

// Guards both tiers' ring[]/head/count and their OpenBucket accumulators.
// pushHistorySample() (main loop task) and buildHistoryJson() (servable from
// the dedicated web-server task, see web_api.cpp) can genuinely run
// concurrently on the two ESP32-S3 cores — unlike a plain scalar (see the
// `volatile` note on Config's int fields), an in-progress head/count update
// read mid-write could hand back an out-of-bounds ring[] index, not just a
// stale value, so this needs a real mutex rather than just visibility
// guarantees.
SemaphoreHandle_t historyMutex = xSemaphoreCreateMutex();

void mergeSeries(OpenBucket& b, const float* v, const bool* ok)
{
    if (!v || !ok) return;
    for (int i = 0; i < SERIES_MAX; i++)
        if (serKey[i] && ok[i]) accSeries(b, i, v[i], 1);
}

// The bucket's value per slot as ring entries (SERIES_GAP without one)
void seriesFromBucket(const OpenBucket& b, int16_t out[SERIES_MAX])
{
    for (int i = 0; i < SERIES_MAX; i++) {
        out[i] = SERIES_GAP;
        if (!serKey[i] || !b.sCount[i]) continue;
        const uint8_t f = serDef[i].field;
        out[i] = seriesEncode(f, serAgg[i] == SA_MEAN ? b.sAcc[i] / b.sCount[i] : b.sAcc[i]);
    }
}

bool anySeries(const OpenBucket& b)
{
    for (int i = 0; i < SERIES_MAX; i++) if (serKey[i] && b.sCount[i]) return true;
    return false;
}

void mergeIntoBucket(OpenBucket& b)
{
    if (srcActive(SG_WIND)) {
        b.windGustMax = (b.windCount == 0) ? windGustMs : max(b.windGustMax, windGustMs);
        b.windSpeedSum += windSpeedMs;
        b.windDirLast   = windDirection;
        b.windCount++;
    }
    if (srcActive(SG_RAIN)) {
        b.rainRateSum   += constrain(rainRateMmH, -RAIN_RATE_LIMIT_MMH, RAIN_RATE_LIMIT_MMH);
        b.rainAmountSum += constrain(rainMM, 0.0f, RAIN_AMOUNT_LIMIT_MM);
        b.rainCount++;
    }
    if (gSrc.tempValid) {
        b.tempMin = b.bmeCount ? fminf(b.tempMin, gSrc.tempC) : gSrc.tempC;
        b.tempMax = b.bmeCount ? fmaxf(b.tempMax, gSrc.tempC) : gSrc.tempC;
        b.tempSum     += gSrc.tempC;
        b.bmeCount++;
    }
    if (gSrc.tempValid && gSrc.humValid) {
        b.humMin = b.humCount ? fminf(b.humMin, gSrc.humidity) : gSrc.humidity;
        b.humMax = b.humCount ? fmaxf(b.humMax, gSrc.humidity) : gSrc.humidity;
        b.humiditySum += gSrc.humidity;
        b.humCount++;
    }
    if (gSrc.pressValid) {
        b.pressMin = b.pressCount ? fminf(b.pressMin, gSrc.pressSeaHpa) : gSrc.pressSeaHpa;
        b.pressMax = b.pressCount ? fmaxf(b.pressMax, gSrc.pressSeaHpa) : gSrc.pressSeaHpa;
        b.pressureSum += gSrc.pressSeaHpa;
        b.pressCount++;
    }
    if (gCfg.lipoEn) {
        b.batterySum += batteryVolt;
        b.lipoCount++;
    }
    if (gSrc.uvRadio) {
        b.uvIndexSum += gSrc.uvIndexRadio;
        b.uvIndexCount++;
    } else if (gSrc.uvBoard && ioFull() && isfinite(gIo.uvSensorVolts)) {
        b.uvVoltsSum += gIo.uvSensorVolts;
        b.uvCount++;
        if (gIo.uvIndexCalibrated && isfinite(gIo.uvIndex)) {
            b.uvIndexSum += gIo.uvIndex;
            b.uvIndexCount++;
        }
    }
    for (int i = 0; i < 3 && i < bus1Count; i++) {
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C) {
            b.ds18b20Sum[i] += bus1Temps[i];
            b.ds18b20Count[i]++;
        }
    }
    b.cycles++;
}

// Converts an OpenBucket's running sums into a HistorySample without
// touching the bucket itself — shared by pushTier() (which then
// appends the result and resets the bucket) and serializeTier() (which uses
// it to preview the *current*, not-yet-finalized bucket as a live point, see
// below). Returns false for a bucket without any value (every sensor off)
// rather than producing a misleadingly "valid" all-zero sample; pushTier()
// stores a gap entry then.
bool sampleFromBucket(const OpenBucket& b, unsigned long endSec, HistorySample& out)
{
    bool anyDs18b20 = b.ds18b20Count[0] || b.ds18b20Count[1] || b.ds18b20Count[2];
    if (!(b.windCount || b.rainCount || b.bmeCount || b.pressCount || b.lipoCount || b.uvCount || b.uvIndexCount || anyDs18b20)) return false;

    out = HistorySample{};
    out.bucketEndSec = endSec;
    out.cycles = (uint16_t)(b.cycles > 0xFFFF ? 0xFFFF : b.cycles);
    for (int i = 0; i < 3; i++) {
        out.ds18b20X10[i] = b.ds18b20Count[i]
            ? (int16_t)roundf(b.ds18b20Sum[i] / b.ds18b20Count[i] * 10.0f)
            : DS18B20_INVALID_X10;
    }
    if (b.windCount) {
        out.validMask     |= VALID_WIND;
        out.windSpeedX100  = (int16_t)roundf(b.windSpeedSum / b.windCount * 100.0f);
        out.windGustX100   = (int16_t)roundf(b.windGustMax * 100.0f);
        out.windDirDeg     = b.windDirLast;
    }
    if (b.rainCount) {
        out.validMask     |= VALID_RAIN;
        out.rainRateX100   = (int16_t)roundf(b.rainRateSum / b.rainCount * 100.0f);
        out.rainAmountX100 = (uint16_t)roundf(fminf(b.rainAmountSum, RAIN_AMOUNT_LIMIT_MM) * 100.0f);
    }
    if (b.bmeCount) {
        out.validMask      |= VALID_BME280;
        out.tempCx10        = (int16_t)roundf(b.tempSum / b.bmeCount * 10.0f);
        out.tempMinCx10     = (int16_t)roundf(b.tempMin * 10.0f);
        out.tempMaxCx10     = (int16_t)roundf(b.tempMax * 10.0f);
    }
    if (b.humCount) {
        out.validMask      |= VALID_HUM;
        out.humidityX2      = (uint8_t)roundf(b.humiditySum / b.humCount * 2.0f);
        out.humMinX2        = (uint8_t)roundf(b.humMin * 2.0f);
        out.humMaxX2        = (uint8_t)roundf(b.humMax * 2.0f);
    }
    if (b.pressCount) {
        out.validMask      |= VALID_PRESS;
        out.pressureQnhX10  = (uint16_t)roundf(b.pressureSum / b.pressCount * 10.0f);
        out.pressMinX10     = (uint16_t)roundf(b.pressMin * 10.0f);
        out.pressMaxX10     = (uint16_t)roundf(b.pressMax * 10.0f);
    }
    if (b.lipoCount) {
        out.validMask       |= VALID_LIPO;
        out.batteryVoltX100  = (uint16_t)roundf(b.batterySum / b.lipoCount * 100.0f);
    }
    if (b.uvCount || b.uvIndexCount) {
        out.validMask |= VALID_UV;
        if (b.uvCount) out.validMask |= VALID_UVV;
        out.uvVoltsX1000 = b.uvCount ? (uint16_t)constrain(lroundf(b.uvVoltsSum / b.uvCount * 1000.0f), 0L, 5500L) : 0;
        out.uvIndexX100 = b.uvIndexCount
            ? (uint16_t)constrain(lroundf(fminf(b.uvIndexSum / b.uvIndexCount, UV_INDEX_REPORT_MAX) * 100.0f), 0L, 32767L)
            : 0xFFFF;
    }
    return true;
}

// Adds a finished bucket (s, with its custom series entries sv) to an open
// one with weight w: 1 (like one cycle) for an hour into its day (daily
// tier) or a bucket into a downsampled point (buildHistoryJson()); its
// number of cycles for a 5-min bucket into the rebuilt open hour
// (primeHour()), so that hour's means come out as without the restart.
// Means average the means, gust / maxima take the maximum, minima the
// minimum, rain and lightning strikes add up, the direction is the last one.
void mergeSample(OpenBucket& b, const HistorySample& s, const int16_t* sv, uint16_t w = 1)
{
    if (s.validMask & VALID_WIND) {
        const float g = s.windGustX100 / 100.0f;
        b.windGustMax = b.windCount ? fmaxf(b.windGustMax, g) : g;
        b.windSpeedSum += s.windSpeedX100 / 100.0f * w;
        b.windDirLast = s.windDirDeg;
        b.windCount += w;
    }
    if (s.validMask & VALID_RAIN) {
        b.rainRateSum += s.rainRateX100 / 100.0f * w;
        b.rainAmountSum += s.rainAmountX100 / 100.0f;
        b.rainCount += w;
    }
    if (s.validMask & VALID_BME280) {
        const float lo = s.tempMinCx10 / 10.0f, hi = s.tempMaxCx10 / 10.0f;
        b.tempMin = b.bmeCount ? fminf(b.tempMin, lo) : lo;
        b.tempMax = b.bmeCount ? fmaxf(b.tempMax, hi) : hi;
        b.tempSum += s.tempCx10 / 10.0f * w;
        b.bmeCount += w;
    }
    if (s.validMask & VALID_HUM) {
        const float lo = s.humMinX2 / 2.0f, hi = s.humMaxX2 / 2.0f;
        b.humMin = b.humCount ? fminf(b.humMin, lo) : lo;
        b.humMax = b.humCount ? fmaxf(b.humMax, hi) : hi;
        b.humiditySum += s.humidityX2 / 2.0f * w;
        b.humCount += w;
    }
    if (s.validMask & VALID_PRESS) {
        const float lo = s.pressMinX10 / 10.0f, hi = s.pressMaxX10 / 10.0f;
        b.pressMin = b.pressCount ? fminf(b.pressMin, lo) : lo;
        b.pressMax = b.pressCount ? fmaxf(b.pressMax, hi) : hi;
        b.pressureSum += s.pressureQnhX10 / 10.0f * w;
        b.pressCount += w;
    }
    if (s.validMask & VALID_LIPO)  { b.batterySum += s.batteryVoltX100 / 100.0f * w; b.lipoCount += w; }
    if (s.validMask & VALID_UV) {
        if (s.validMask & VALID_UVV) { b.uvVoltsSum += s.uvVoltsX1000 / 1000.0f * w; b.uvCount += w; }
        if (s.uvIndexX100 != 0xFFFF) { b.uvIndexSum += s.uvIndexX100 / 100.0f * w; b.uvIndexCount += w; }
    }
    for (int i = 0; i < 3; i++) {
        if (s.ds18b20X10[i] == DS18B20_INVALID_X10) continue;
        b.ds18b20Sum[i] += s.ds18b20X10[i] / 10.0f * w;
        b.ds18b20Count[i] += w;
    }
    b.cycles += s.cycles;
    if (!sv) return;
    for (int i = 0; i < SERIES_MAX; i++)
        if (serKey[i] && sv[i] != SERIES_GAP) accSeries(b, i, seriesDecode(serDef[i].field, sv[i]), w);
}

// sv: the custom series' entries for it, nullptr = all gaps
void appendSample(Tier& t, const HistorySample& s, const int16_t* sv = nullptr)
{
    t.ring[t.head] = s;
    for (int i = 0; i < SERIES_MAX; i++) if (t.ser[i]) t.ser[i][t.head] = sv ? sv[i] : SERIES_GAP;
    t.head = (t.head + 1) % t.capacity;
    if (t.count < t.capacity) t.count++;
    t.gen++;
}

HistorySample gapSample(uint32_t endSec)
{
    HistorySample s{};
    s.bucketEndSec = endSec;
    for (int i = 0; i < 3; i++) s.ds18b20X10[i] = DS18B20_INVALID_X10;
    return s;
}

// A window without any main value: those fields null in the JSON (the
// custom series may still have one: sv).
void appendGap(Tier& t, uint32_t endSec, const int16_t* sv = nullptr)
{
    appendSample(t, gapSample(endSec), sv);
}

int64_t floorDiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int64_t alignedNow(uint32_t nowSec)
{
    int64_t epoch;
    if (epochNow(epoch)) {
        int64_t off = epoch - (int64_t)nowSec;
        if (!clockOffsetFromEpoch || off - clockOffset > 2 || clockOffset - off > 2) {
            clockOffset = off;
            clockOffsetFromEpoch = true;
        }
    }
    return (int64_t)nowSec + clockOffset;
}

// An uptime bucket end on the aligned clock. Restored ends that lie before
// this start are "negative" uptimes (modulo 2^32): read as int32 - real
// uptimes stay far below 2^31 s.
int64_t alignedOf(uint32_t uptimeEnd) { return (int64_t)(int32_t)uptimeEnd + clockOffset; }

// ---- Local calendar days (daily tier). Only on the main task: TZ is only
// changed there (time_sync.cpp), so localtime_r / mktime need no lock. ----
// Days since 1970-01-01 of a civil date and back (H. Hinnant's algorithms).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int)((int64_t)yoe + era * 400 + (m <= 2));
}
int64_t localDayOf(int64_t epoch)
{
    const time_t t = (time_t)epoch;
    struct tm lt;
    localtime_r(&t, &lt);
    return daysFromCivil(lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday);
}
int64_t localMidnight(int64_t day)   // epoch of local 00:00 of that day
{
    int y;
    unsigned m, d;
    civilFromDays(day, y, m, d);
    struct tm lt;
    memset(&lt, 0, sizeof(lt));
    lt.tm_year = y - 1900;
    lt.tm_mon = (int)m - 1;
    lt.tm_mday = (int)d;
    lt.tm_isdst = -1;
    return (int64_t)mktime(&lt);
}

// Window k of a tier on the aligned clock: (end(k - 1), end(k)]. Fixed
// tiers: end(k) = (k + 1) x bucketSec; daily: local midnight after day k.
int64_t windowIndex(const Tier& t, int64_t aligned)
{
    return t.localDays ? localDayOf(aligned - 1) : floorDiv(aligned - 1, t.bucketSec);
}
int64_t windowEnd(const Tier& t, int64_t idx)
{
    return t.localDays ? localMidnight(idx + 1) : (idx + 1) * (int64_t)t.bucketSec;
}
// A day may be an hour shorter or longer than bucketSec (DST)
uint32_t windowSlack(const Tier& t) { return t.localDays ? 3600 : 0; }

void feedDaily(const HistorySample& s, const int16_t* sv);

// Finishes the open bucket (a gap entry if it has no value at all) - nextIdx
// = the window of the next value - and records the windows skipped in
// between (cycles longer than the bucket) as gaps; never more of them than
// fit between the closed bucket's end and now in uptime, so a clock step
// (first sync, NTP correction) cannot produce a run of fake gaps. After a
// step backwards a window can come again: it is a new entry then, and the
// closed bucket's end is clamped to now so the ring stays in time order.
void closeOpen(Tier& t, uint32_t nowSec, int64_t nextIdx)
{
    OpenBucket& b = *t.open;
    uint32_t endSec = (int32_t)(nowSec - b.endSec) >= 0 ? b.endSec : nowSec;
    HistorySample s;
    int16_t ser[SERIES_MAX];
    seriesFromBucket(b, ser);
    if (!sampleFromBucket(b, endSec, s)) s = gapSample(endSec);
    appendSample(t, s, ser);
    if (&t == &coarse) feedDaily(s, ser);
    if (nextIdx > b.index + 1) {
        uint64_t skipped = (uint64_t)(nextIdx - b.index - 1);
        uint64_t fit = (nowSec - endSec + windowSlack(t)) / t.bucketSec;
        if (skipped > fit) skipped = fit;
        if (skipped > t.capacity) skipped = t.capacity;
        uint32_t e = endSec;
        for (uint64_t k = 1; k <= skipped; k++) {
            e += (uint32_t)(windowEnd(t, b.index + (int64_t)k) - windowEnd(t, b.index + (int64_t)k - 1));
            appendGap(t, e);
        }
    }
    const int64_t closed = b.index;
    b = OpenBucket{};
    b.haveClosed = true;
    b.closedIndex = closed;
}

// The open bucket of window idx (aligned clock), started if needed. After
// historyImport() the windows between the restored entries and it become gaps.
OpenBucket& openFor(Tier& t, uint32_t nowSec, int64_t idx)
{
    OpenBucket& b = *t.open;
    // The clock stepped back (time_sync.h: a better time source corrected
    // it) by up to one window: stay in the open window until the clock has
    // passed it again - no window twice, no day twice; its end (uptime)
    // moves with the clock. Steps further back close it (closeOpen() ends it
    // now, not in the future) and start over, but never in a window that
    // already has an entry.
    if (b.started && (idx == b.index || idx == b.index - 1)) {
        idx = b.index;
        const uint32_t e = (uint32_t)(windowEnd(t, idx) - clockOffset);
        if ((int32_t)(e - b.endSec) > 0) b.endSec = e;
    }
    if (b.started && idx != b.index) closeOpen(t, nowSec, idx);
    if (!b.started) {
        if (b.haveClosed && idx <= b.closedIndex && t.count) {   // back at or before the closed window
            const int64_t lastIdx = windowIndex(t, alignedOf(t.ring[(t.head + t.capacity - 1) % t.capacity].bucketEndSec));
            if (idx <= lastIdx) idx = lastIdx + 1;   // at most the next window: the newest entry ends now at the latest
        }
        b.started = true;
        b.index = idx;
        b.endSec = (uint32_t)(windowEnd(t, idx) - clockOffset);
        if (t.count) {
            const int64_t last = alignedOf(t.ring[(t.head + t.capacity - 1) % t.capacity].bucketEndSec);
            int64_t j = windowIndex(t, last) + 1;
            if (idx - j > (int64_t)t.capacity) j = idx - (int64_t)t.capacity;
            for (; j < idx; j++) {
                const int64_t e = windowEnd(t, j);
                if (e > last) appendGap(t, (uint32_t)(e - clockOffset));
            }
        }
    }
    return b;
}

// Adds the just-finished cycle (ended at nowSec) to the tier: to the bucket
// whose window contains that end time.
void pushTier(Tier& t, uint32_t nowSec, int64_t aligned, const float* sv, const bool* sok)
{
    OpenBucket& b = openFor(t, nowSec, windowIndex(t, aligned));
    mergeIntoBucket(b);
    mergeSeries(b, sv, sok);
}

// A finished hourly entry into its local day (the day in which it ends). The
// day closes as soon as its last hour is in. Hourly entries before the clock
// sync (uptime windows) are taken over by primeDaily() once it is synced.
void feedDaily(const HistorySample& s, const int16_t* sv)
{
    if (!dailyPrimed || !clockOffsetFromEpoch) return;
    const int64_t aligned = alignedOf(s.bucketEndSec);
    OpenBucket& b = openFor(daily, s.bucketEndSec, windowIndex(daily, aligned));
    mergeSample(b, s, sv);
    if (aligned >= windowEnd(daily, b.index)) closeOpen(daily, s.bucketEndSec, b.index + 1);
}

// The daily tier takes over the hourly entries it does not have yet - after
// the clock sync and after a restore: days the node was off around, the
// hours of the open day (so a restart does not lose the day so far), and
// with an empty daily tier everything the hourly tier holds. Caller holds
// historyMutex; clock synced.
void primeDaily()
{
    dailyPrimed = true;
    *daily.open = OpenBucket{};
    const bool have = daily.count > 0;
    const int64_t lastDay = have ? alignedOf(daily.ring[(daily.head + daily.capacity - 1) % daily.capacity].bucketEndSec) : 0;
    const size_t start = coarse.count < coarse.capacity ? 0 : coarse.head;
    int16_t sv[SERIES_MAX];
    for (size_t i = 0; i < coarse.count; i++) {
        const size_t at = (start + i) % coarse.capacity;
        const HistorySample& s = coarse.ring[at];
        if (have && alignedOf(s.bucketEndSec) <= lastDay) continue;
        for (int k = 0; k < SERIES_MAX; k++) sv[k] = coarse.ser[k] ? coarse.ser[k][at] : SERIES_GAP;
        feedDaily(s, sv);
    }
}

// The open hour takes over the restored 5-min buckets that lie in it (the
// cycles of the hour before the restart; this run's cycles are already in
// it), each weighted by its number of cycles, so the hour's means, extremes
// and rain come out as without the restart (except the cycles of the 5-min
// bucket open at the restart and the time the node was off). Caller holds
// historyMutex; clock synced, coarse.open started on the wall clock.
void primeHour()
{
    hourPrimePending = false;
    OpenBucket& b = *coarse.open;
    if (!b.started) return;
    const int64_t from = windowEnd(coarse, b.index - 1), to = windowEnd(coarse, b.index);
    const bool hadWind = b.windCount > 0;
    const uint16_t dir = b.windDirLast;   // the newest direction is this run's
    const size_t start = fine.count < fine.capacity ? 0 : fine.head;
    int16_t sv[SERIES_MAX];
    for (size_t i = 0; i < fine.count; i++) {
        const size_t at = (start + i) % fine.capacity;
        const HistorySample& s = fine.ring[at];
        const int64_t e = alignedOf(s.bucketEndSec);
        if (e <= from || e > to || e > hourPrimeUntil || !s.cycles) continue;
        for (int k = 0; k < SERIES_MAX; k++) sv[k] = fine.ser[k] ? fine.ser[k][at] : SERIES_GAP;
        mergeSample(b, s, sv, s.cycles);
    }
    if (hadWind) b.windDirLast = dir;
}

// One range of one tier as /api/history JSON (buildHistoryJson()). Ring
// entries whose age lies in [ageEnd, ageEnd + rangeSec) - `group` of them
// (aligned on the clock) merged into one point -, then, at ageEnd 0, the
// open bucket as one extra, newest ("age" 0) point whenever it has at least
// one merge, so the chart doesn't sit on "no data yet" for the bucket's whole
// window right after boot or a clear - a live preview naturally gets more
// representative as the bucket fills, same as any in-progress average. The
// open day (daily tier) is its finished hours plus the open hour.
void serializeRange(JsonDocument& doc, uint32_t nowSec, const Tier& tier, uint32_t ageEnd, uint32_t rangeSec, uint32_t group)
{
    const HistorySample* ring = tier.ring;
    const size_t capacity = tier.capacity, head = tier.head, count = tier.count;
    JsonArray ageSec      = doc["ageSec"].to<JsonArray>();
    JsonArray windSpeed   = doc["windSpeed"].to<JsonArray>();
    JsonArray windGust    = doc["windGust"].to<JsonArray>();
    JsonArray windDirDeg  = doc["windDirDeg"].to<JsonArray>();
    JsonArray rainRate    = doc["rainRate"].to<JsonArray>();
    JsonArray rainMm      = doc["rainMm"].to<JsonArray>();
    JsonArray temp        = doc["temp"].to<JsonArray>();
    JsonArray tempMin     = doc["tempMin"].to<JsonArray>();
    JsonArray tempMax     = doc["tempMax"].to<JsonArray>();
    JsonArray humidityPct = doc["humidityPct"].to<JsonArray>();
    JsonArray humMin      = doc["humMin"].to<JsonArray>();
    JsonArray humMax      = doc["humMax"].to<JsonArray>();
    JsonArray pressureQnh = doc["pressureQnh"].to<JsonArray>();
    JsonArray pressMin    = doc["pressMin"].to<JsonArray>();
    JsonArray pressMax    = doc["pressMax"].to<JsonArray>();
    JsonArray batteryVolt = doc["batteryVolt"].to<JsonArray>();
    JsonArray uvSensorVolts = doc["uvSensorVolts"].to<JsonArray>();
    JsonArray uvIndex = doc["uvIndex"].to<JsonArray>();
    JsonArray ds18b20_0   = doc["ds18b20_0"].to<JsonArray>();
    JsonArray ds18b20_1   = doc["ds18b20_1"].to<JsonArray>();
    JsonArray ds18b20_2   = doc["ds18b20_2"].to<JsonArray>();
    JsonArray ds18b20Arrays[3] = { ds18b20_0, ds18b20_1, ds18b20_2 };
    // custom series: {"slot", "name", "sensor", "field", "unit", "decimals", "agg", "perBucket", "values": [...]} per defined slot
    JsonArray serVals[SERIES_MAX];
    bool anyDefined = false;
    for (int i = 0; i < SERIES_MAX; i++) anyDefined |= serKey[i] && tier.ser[i];
    if (anyDefined) {
        JsonArray series = doc["series"].to<JsonArray>();
        for (int i = 0; i < SERIES_MAX; i++) {
            if (!serKey[i] || !tier.ser[i]) continue;
            const SeriesFieldInfo& fi = SERIES_FIELDS[serDef[i].field];
            JsonObject o = series.add<JsonObject>();
            o["slot"] = i + 1;
            o["name"] = (const char*)serDef[i].name;   // a char array would be taken as a literal of its full size (ArduinoJson 7)
            if (serDef[i].sensorInternalId[0]) o["sensor"] = (const char*)serDef[i].sensorInternalId;   // the logical sensor (dashboard sensor page)
            o["field"] = fi.key;
            char tu[8];
            snprintf(tu, sizeof(tu), "\xC2\xB0%s", temperatureUnitLabel(gCfg));
            if (fi.isTemp) o["unit"] = tu;
            else if (fi.unit[0]) o["unit"] = fi.unit;
            o["decimals"] = fi.decimals;
            o["agg"] = SERIES_AGG_KEYS[serAgg[i]];
            if (serAgg[i] == SA_SUM) o["perBucket"] = true;   // a sum per bucket (lightning), not a level
            serVals[i] = o["values"].to<JsonArray>();
        }
    }
    auto addSeries = [&](const int16_t* sv) {
        for (int i = 0; i < SERIES_MAX; i++) {
            if (serVals[i].isNull()) continue;
            const int16_t raw = sv ? sv[i] : SERIES_GAP;
            if (raw == SERIES_GAP) { serVals[i].add(nullptr); continue; }
            const uint8_t f = serDef[i].field;
            const float v = seriesDecode(f, raw);
            serVals[i].add(SERIES_FIELDS[f].isTemp ? temperatureForOutput(v, gCfg) : v);
        }
    };

    auto addSample = [&](const HistorySample& s, unsigned long ageSecVal, const int16_t* sv) {
        ageSec.add(ageSecVal);
        addSeries(sv);

        if (s.validMask & VALID_WIND) {
            windSpeed.add(windSpeedForOutput(s.windSpeedX100 / 100.0f, gCfg));
            windGust.add (windSpeedForOutput(s.windGustX100  / 100.0f, gCfg));
            windDirDeg.add(s.windDirDeg);
        } else {
            windSpeed.add(nullptr);
            windGust.add(nullptr);
            windDirDeg.add(nullptr);
        }
        if (s.validMask & VALID_RAIN) {
            rainRate.add(rainRateForOutput(s.rainRateX100 / 100.0f, gCfg));
            rainMm.add(rainForOutput(s.rainAmountX100 / 100.0f, gCfg));
        } else {
            rainRate.add(nullptr);
            rainMm.add(nullptr);
        }
        if (s.validMask & VALID_BME280) {
            temp.add(temperatureForOutput(s.tempCx10 / 10.0f, gCfg));
            tempMin.add(temperatureForOutput(s.tempMinCx10 / 10.0f, gCfg));
            tempMax.add(temperatureForOutput(s.tempMaxCx10 / 10.0f, gCfg));
        } else {
            temp.add(nullptr);
            tempMin.add(nullptr);
            tempMax.add(nullptr);
        }
        if (s.validMask & VALID_HUM) {
            humidityPct.add(s.humidityX2 / 2.0f);
            humMin.add(s.humMinX2 / 2.0f);
            humMax.add(s.humMaxX2 / 2.0f);
        } else {
            humidityPct.add(nullptr);
            humMin.add(nullptr);
            humMax.add(nullptr);
        }
        if (s.validMask & VALID_PRESS) {
            pressureQnh.add(pressureForOutput(s.pressureQnhX10 / 10.0f, gCfg));
            pressMin.add(pressureForOutput(s.pressMinX10 / 10.0f, gCfg));
            pressMax.add(pressureForOutput(s.pressMaxX10 / 10.0f, gCfg));
        } else {
            pressureQnh.add(nullptr);
            pressMin.add(nullptr);
            pressMax.add(nullptr);
        }
        if (s.validMask & VALID_LIPO) {
            batteryVolt.add(s.batteryVoltX100 / 100.0f);
        } else {
            batteryVolt.add(nullptr);
        }
        if (s.validMask & VALID_UV) {
            if (s.validMask & VALID_UVV) uvSensorVolts.add(s.uvVoltsX1000 / 1000.0f);
            else uvSensorVolts.add(nullptr);
            if (s.uvIndexX100 != 0xFFFF) uvIndex.add(s.uvIndexX100 / 100.0f);
            else uvIndex.add(nullptr);
        } else {
            uvSensorVolts.add(nullptr);
            uvIndex.add(nullptr);
        }
        for (int i = 0; i < 3; i++) {
            if (s.ds18b20X10[i] == DS18B20_INVALID_X10) ds18b20Arrays[i].add(nullptr);
            else ds18b20Arrays[i].add(temperatureForOutput(s.ds18b20X10[i] / 10.0f, gCfg));
        }
    };

    // Oldest-first: before the buffer first wraps, that's simply index 0;
    // once full, head already points at the oldest (next-to-overwrite) slot.
    size_t start = (count < capacity) ? 0 : head;
    int16_t sv[SERIES_MAX];
    // downsampling: the open point (group > 1)
    OpenBucket acc;
    bool accUsed = false;
    int64_t accKey = 0;
    uint32_t accAge = 0;
    auto flush = [&]() {
        if (!accUsed) return;
        HistorySample p;
        if (!sampleFromBucket(acc, 0, p)) p = gapSample(0);
        int16_t psv[SERIES_MAX];   // not sv: that holds the bucket about to be merged
        seriesFromBucket(acc, psv);
        addSample(p, accAge, psv);
        acc = OpenBucket{};
        accUsed = false;
    };
    const int64_t nowAligned = (int64_t)nowSec + clockOffset;
    const int64_t groupSec = (int64_t)tier.bucketSec * group;
    for (size_t i = 0; i < count; i++) {
        const size_t at = (start + i) % capacity;
        const HistorySample& s = ring[at];
        const uint32_t age = (uint32_t)(nowSec - s.bucketEndSec);   // modulo 2^32: restored ends may lie before the start
        if (age < ageEnd || age - ageEnd >= rangeSec) continue;
        for (int k = 0; k < SERIES_MAX; k++) sv[k] = tier.ser[k] ? tier.ser[k][at] : SERIES_GAP;
        if (group <= 1) { addSample(s, age, sv); continue; }
        const int64_t key = floorDiv(nowAligned - (int64_t)age - 1, groupSec);
        if (accUsed && key != accKey) flush();
        mergeSample(acc, s, sv);
        accUsed = true;
        accKey = key;
        accAge = age;
    }
    flush();

    if (ageEnd == 0) {
        OpenBucket o = *tier.open;
        if (&tier == &daily) {
            // + the open hour if it belongs to the open day (or to the day after the last finished one)
            const OpenBucket& h = *coarse.open;
            bool inDay = false;
            if (h.started && clockOffsetFromEpoch && dailyPrimed) {
                if (o.started) inDay = (int32_t)(o.endSec - h.endSec) >= 0;
                else inDay = !daily.count || (int32_t)(h.endSec - daily.ring[(daily.head + daily.capacity - 1) % daily.capacity].bucketEndSec) > 0;
            }
            HistorySample hs;
            if (inDay) {
                int16_t hsv[SERIES_MAX];
                seriesFromBucket(h, hsv);
                if (!sampleFromBucket(h, nowSec, hs)) hs = gapSample(nowSec);
                mergeSample(o, hs, hsv);
            }
        }
        HistorySample liveSample;
        const bool liveMain = sampleFromBucket(o, nowSec, liveSample);
        if (liveMain || anySeries(o)) {
            if (!liveMain) liveSample = gapSample(nowSec);
            seriesFromBucket(o, sv);
            addSample(liveSample, 0, sv);
        }
    }

    doc["windUnit"]     = windSpeedUnitLabel(gCfg);
    doc["rainRateUnit"] = rainRateUnitLabel(gCfg);
    doc["rainUnit"]     = rainUnitLabel(gCfg);
    doc["tempUnit"]     = temperatureUnitLabel(gCfg);
    doc["pressureUnit"] = pressureUnitLabel(gCfg);
    doc["uvSensorVoltsUnit"] = "V";
    doc["uvIndexUnit"] = "index";
}

} // namespace

void historyAllocate()
{
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    xSemaphoreGive(historyMutex);
}

void historyClear()
{
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    for (Tier* t : TIERS) { t->head = 0; t->count = 0; *t->open = OpenBucket{}; t->gen++; }
    dailyPrimed = false;
    hourPrimePending = false;
    xSemaphoreGive(historyMutex);
}

void pushHistorySample(const float* seriesV, const bool* seriesOk)
{
    uint32_t nowSec = uptimeSec();

    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    int64_t aligned = alignedNow(nowSec);
    pushTier(fine, nowSec, aligned, seriesV, seriesOk);
    pushTier(coarse, nowSec, aligned, seriesV, seriesOk);   // a finished hour goes on into its day
    if (hourPrimePending && clockOffsetFromEpoch) primeHour();
    if (!dailyPrimed && clockOffsetFromEpoch) primeDaily();
    xSemaphoreGive(historyMutex);
}

void historySeriesConfigure(const SeriesDef defs[SERIES_MAX], int limit)
{
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    for (int i = 0; i < SERIES_MAX; i++) {
        const SeriesDef d = defs[i];
        const uint32_t key = (i < limit && d.used()) ? seriesKey(d) : 0;
        if (key == serKey[i]) { serDef[i] = d; serAgg[i] = seriesAgg(d); continue; }   // same data: e.g. only re-saved
        for (Tier* t : TIERS) {
            if (key && !t->ser[i]) t->ser[i] = (int16_t*)bigAlloc(t->capacity * sizeof(int16_t));
            if (!key && t->ser[i]) { free(t->ser[i]); t->ser[i] = nullptr; }
            if (t->ser[i]) for (size_t k = 0; k < t->capacity; k++) t->ser[i][k] = SERIES_GAP;
            t->open->sAcc[i] = 0;
            t->open->sCount[i] = 0;
        }
        // no memory for the rings: the slot stays off
        serKey[i] = (key && fine.ser[i] && coarse.ser[i] && daily.ser[i]) ? key : 0;
        if (key && !serKey[i]) for (Tier* t : TIERS) { free(t->ser[i]); t->ser[i] = nullptr; }
        for (Tier* t : TIERS) t->gen++;
        serDef[i] = d;
        serAgg[i] = seriesAgg(d);
    }
    xSemaphoreGive(historyMutex);
}

const HistoryRangeDef HISTORY_RANGES[HISTORY_RANGE_COUNT] = {
    { "24h", 86400 }, { "7d", 7 * 86400 }, { "30d", 30 * 86400 }, { "1y", 365 * 86400 },
};
const char* const HISTORY_TIER_NAMES[HT_COUNT] = { "5m", "1h", "1d" };

uint32_t historyTierBucketSec(int tier)
{
    return tier == HT_FINE ? HISTORY_FINE_BUCKET_SEC : tier == HT_HOURLY ? HISTORY_HOURLY_BUCKET_SEC : HISTORY_DAILY_BUCKET_SEC;
}
size_t historyTierCapacity(int tier)
{
    return tier == HT_FINE ? HISTORY_FINE_CAPACITY : tier == HT_HOURLY ? HISTORY_HOURLY_CAPACITY : HISTORY_DAILY_CAPACITY;
}

bool historyParseQuery(const char* range, const char* end, const char* res, HistoryQuery& q)
{
    q = HistoryQuery{};
    for (const HistoryRangeDef& r : HISTORY_RANGES)
        if (range && !strcmp(range, r.name)) { q.range = r.name; q.rangeSec = r.sec; }
    if (end && *end) {
        char* rest = nullptr;
        const long long v = strtoll(end, &rest, 10);
        if (*rest || v <= 0) return false;
        q.endEpoch = (int64_t)v;
    }
    if (res && *res) {
        for (int i = 0; i < HT_COUNT; i++) if (!strcmp(res, HISTORY_TIER_NAMES[i])) q.res = (int8_t)i;
        if (q.res < 0) return false;
    }
    return true;
}

void historyPlan(uint32_t rangeSec, uint32_t depthSec, int res, int& tier, uint32_t& group)
{
    tier = HT_DAILY;
    if (res >= 0 && res < HT_COUNT) tier = res;
    else
        for (int i = 0; i < HT_COUNT; i++)
            if ((uint64_t)historyTierCapacity(i) * historyTierBucketSec(i) >= depthSec) { tier = i; break; }
    const uint32_t bucket = historyTierBucketSec(tier);
    const uint32_t nominal = (rangeSec + bucket - 1) / bucket;
    group = (uint32_t)((nominal + HISTORY_MAX_POINTS - 1) / HISTORY_MAX_POINTS);
    if (group < 1) group = 1;
}

size_t historyRangeMaxPoints(uint32_t rangeSec)
{
    int tier;
    uint32_t group;
    historyPlan(rangeSec, rangeSec, -1, tier, group);
    size_t n = rangeSec / historyTierBucketSec(tier);
    if (n > historyTierCapacity(tier)) n = historyTierCapacity(tier);
    return (n + group - 1) / group;
}

void buildHistoryJson(JsonDocument& doc, const HistoryQuery& q)
{
    doc.clear();
    doc["apiVersion"] = API_VERSION;
    const uint32_t nowSec = uptimeSec();
    // window end as an age; a past end only with the synced clock
    uint32_t ageEnd = 0;
    int64_t epoch;
    if (q.endEpoch && epochNow(epoch) && q.endEpoch < epoch) {
        const int64_t a = epoch - q.endEpoch;
        ageEnd = a > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)a;
    }
    int tier;
    uint32_t group;
    const uint64_t depth = (uint64_t)ageEnd + q.rangeSec;
    historyPlan(q.rangeSec, depth > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)depth, q.res, tier, group);
    doc["range"] = q.range;
    doc["res"] = HISTORY_TIER_NAMES[tier];
    doc["bucketSec"] = historyTierBucketSec(tier) * group;

    // Holds the lock for the whole read+JSON-build loop, not just an index
    // snapshot — simpler than copying samples out first, and the lock-out is
    // short (at most a few hundred iterations of scaling math) and
    // infrequent (one request at a time, one push per uplink cycle).
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    serializeRange(doc, nowSec, *TIERS[tier], ageEnd, q.rangeSec, group);
    xSemaphoreGive(historyMutex);
}

namespace {
// Fine-tier sample closest to ageSec ago, judged by the bucket's end. Only a
// bucket that is at least ageSec - 5 min and at most ageSec + 10 min old
// counts: its average then covers a time at least the nominal age back (a
// "1 h" trend needs about an hour of uptime, the 3-h tendency about 3 h),
// and a slightly older one bridges a missing bucket (gap).
constexpr long AGO_EARLY_SEC = 300, AGO_LATE_SEC = 600;
// Caller holds historyMutex.
const HistorySample* fineSampleAgo(unsigned long nowSec, uint32_t ageSec, uint8_t needMask)
{
    const HistorySample* best = nullptr;
    long bestDiff = 0;
    for (size_t i = 0; i < fine.count; i++) {
        const HistorySample& s = fine.ring[i];
        if (!(s.validMask & needMask)) continue;
        long diff = (long)(uint32_t)((uint32_t)nowSec - s.bucketEndSec) - (long)ageSec;
        if (diff < -AGO_EARLY_SEC || diff > AGO_LATE_SEC) continue;
        if (!best || labs(diff) < labs(bestDiff)) { bestDiff = diff; best = &s; }
    }
    return best;
}
} // namespace

bool historyPressureAgo(uint32_t ageSec, float& qnhHpa)
{
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    const HistorySample* s = fineSampleAgo(uptimeSec(), ageSec, VALID_PRESS);
    if (s) qnhHpa = s->pressureQnhX10 / 10.0f;
    xSemaphoreGive(historyMutex);
    return s != nullptr;
}

bool historyTempAgo(uint32_t ageSec, float& tempC)
{
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    const HistorySample* s = fineSampleAgo(uptimeSec(), ageSec, VALID_BME280);
    if (s) tempC = s->tempCx10 / 10.0f;
    xSemaphoreGive(historyMutex);
    return s != nullptr;
}

// A finished fine bucket covers the window (end - 5 min, end] and holds the
// rain of the cycles that ended in it. It counts only if that whole window
// lies within the requested one, so the sum never covers more than windowSec
// (it may miss up to one bucket, 5 min, at the old end). The open bucket -
// everything since the newest finished one - always counts.
float historyRainSum(uint32_t windowSec)
{
    uint32_t nowSec = uptimeSec();
    float mm = 0.0f;
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    for (size_t k = 0; k < fine.count; k++) {   // newest first
        const HistorySample& s = fine.ring[(fine.head + fine.capacity - 1 - k) % fine.capacity];
        if ((uint32_t)(nowSec - s.bucketEndSec + fine.bucketSec) > windowSec) break;
        if (s.validMask & VALID_RAIN) mm += s.rainAmountX100 / 100.0f;
    }
    if (fine.open->rainCount) mm += fine.open->rainAmountSum;
    xSemaphoreGive(historyMutex);
    return mm;
}

// ---- Persistence (history_store.cpp writes the blobs to NVS) ----
// Blob: 12-byte header "UWH" + version, u16 count, u16 bucket length in
// minutes (5, 60, 1440), u32 epoch at saving; then `count` HistorySamples
// (44 bytes), oldest first, whose bucketEndSec is the wall-clock (epoch) end
// of the window instead of the uptime; then the custom series: u8 n, then per
// series u32 key (seriesKey) + `count` int16 entries in the same order; a
// restore takes a series only into a slot with the same key. Version 4
// (4.0.0: 44-byte samples with humidity / pressure min / max and the cycle
// count, three tiers); blobs of older versions (32- / 36-byte samples) are
// not read - that history is dropped. Little endian (ESP32 and
// the host tests).
namespace {
constexpr uint8_t HISTORY_BLOB_VERSION = STORED_HISTORY_TIER_VERSION;   // storage_layout.h

// The epoch end of the window before E (a bucket end of this run, possibly
// still on uptime windows from before the clock sync): restored windows
// end at or before it, so they never overlap this run's first window.
int64_t boundaryBefore(const Tier& t, int64_t E)
{
    const int64_t idx = windowIndex(t, E);
    int64_t b = windowEnd(t, idx - 1);
    if (b > E - (int64_t)t.bucketSec + (int64_t)windowSlack(t)) b = windowEnd(t, idx - 2);
    return b;
}
} // namespace

uint32_t historyGeneration(HistoryTier tier)
{
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    const uint32_t g = TIERS[tier]->gen;
    xSemaphoreGive(historyMutex);
    return g;
}

size_t historyBlobMaxSize(HistoryTier tier)
{
    const size_t cap = historyTierCapacity(tier);
    return HISTORY_BLOB_HEADER + cap * sizeof(HistorySample) + 1 + SERIES_MAX * (4 + cap * sizeof(int16_t));
}

size_t historyExport(HistoryTier tier, uint8_t* out, size_t cap)
{
    const uint32_t nowSec = uptimeSec();
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    alignedNow(nowSec);
    const Tier& t = *TIERS[tier];
    size_t n = 0;
    int nSer = 0;
    for (int i = 0; i < SERIES_MAX; i++) nSer += serKey[i] && t.ser[i];
    const size_t need = HISTORY_BLOB_HEADER + t.count * sizeof(HistorySample) + 1 + nSer * (4 + t.count * sizeof(int16_t));
    if (clockOffsetFromEpoch && t.count && out && cap >= need) {
        const uint16_t count = (uint16_t)t.count, bucketMin = (uint16_t)(t.bucketSec / 60);
        const uint32_t savedEpoch = (uint32_t)((int64_t)nowSec + clockOffset);
        out[0] = 'U'; out[1] = 'W'; out[2] = 'H'; out[3] = HISTORY_BLOB_VERSION;
        memcpy(out + 4, &count, 2);
        memcpy(out + 6, &bucketMin, 2);
        memcpy(out + 8, &savedEpoch, 4);
        n = HISTORY_BLOB_HEADER;
        const size_t start = t.count < t.capacity ? 0 : t.head;
        for (size_t i = 0; i < t.count; i++) {
            HistorySample s = t.ring[(start + i) % t.capacity];
            s.bucketEndSec = (uint32_t)alignedOf(s.bucketEndSec);
            memcpy(out + n, &s, sizeof(s));
            n += sizeof(s);
        }
        out[n++] = (uint8_t)nSer;
        for (int k = 0; k < SERIES_MAX; k++) {
            if (!serKey[k] || !t.ser[k]) continue;
            memcpy(out + n, &serKey[k], 4);
            n += 4;
            for (size_t i = 0; i < t.count; i++) {
                memcpy(out + n, &t.ser[k][(start + i) % t.capacity], 2);
                n += 2;
            }
        }
    }
    xSemaphoreGive(historyMutex);
    return n;
}

StoredVersionAction historyBlobVersionAction(const uint8_t* in, size_t n)
{
    if (!in || n < 4 || in[0] != 'U' || in[1] != 'W' || in[2] != 'H') return STORED_TAKE;
    return storedVersionAction(in[3], HISTORY_BLOB_VERSION);
}

// Restored windows go in front of what was recorded since the start (the
// ring of this run), windows in between as gaps, so there is one entry per
// window; the newest `capacity` entries are kept.
int historyImport(HistoryTier tier, const uint8_t* in, size_t n)
{
    if (n < HISTORY_BLOB_HEADER || in[0] != 'U' || in[1] != 'W' || in[2] != 'H' || in[3] != HISTORY_BLOB_VERSION) return -1;
    uint16_t count, bucketMin;
    memcpy(&count, in + 4, 2);
    memcpy(&bucketMin, in + 6, 2);
    Tier& t = *TIERS[tier];
    const size_t samplesEnd = HISTORY_BLOB_HEADER + count * sizeof(HistorySample);
    if (bucketMin * 60UL != t.bucketSec || count > t.capacity) return -1;
    // the series section after the samples - its blob offset per slot (0 = not in the blob)
    size_t serAt[SERIES_MAX] = {};
    if (n < samplesEnd + 1) return -1;
    const uint8_t ns = in[samplesEnd];
    const size_t per = 4 + count * sizeof(int16_t);
    if (ns > SERIES_MAX || n != samplesEnd + 1 + ns * per) return -1;
    for (int j = 0; j < ns; j++) {
        uint32_t key;
        memcpy(&key, in + samplesEnd + 1 + j * per, 4);
        for (int k = 0; k < SERIES_MAX; k++)
            if (key && serKey[k] == key && !serAt[k]) serAt[k] = samplesEnd + 1 + j * per + 4;
    }

    const uint32_t nowSec = uptimeSec();
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    ensureRings();
    alignedNow(nowSec);
    if (!clockOffsetFromEpoch) { xSemaphoreGive(historyMutex); return -1; }
    // epoch end of the last window before this run's data
    int64_t boundary;
    if (t.count) {
        const size_t start = t.count < t.capacity ? 0 : t.head;
        boundary = boundaryBefore(t, alignedOf(t.ring[start].bucketEndSec));
    } else if (t.open->started) {
        boundary = boundaryBefore(t, alignedOf(t.open->endSec));   // endSec: uptime (index may be pre-sync)
    } else {
        boundary = windowEnd(t, windowIndex(t, (int64_t)nowSec + clockOffset) - 1);
    }

    // temporary, PSRAM (bigAlloc): up to 36 KB + 1.7 KB
    HistorySample* merged = (HistorySample*)bigAlloc(t.capacity * sizeof(HistorySample));
    // where each merged entry came from: >= 0 ring index of this run, -1 a
    // gap, <= -2 blob sample -2 - i (for the custom series)
    int16_t* origin = (int16_t*)bigAlloc(t.capacity * sizeof(int16_t));
    if (!merged || !origin) { free(merged); free(origin); xSemaphoreGive(historyMutex); return -1; }
    // newest first into a temporary ring: this run's entries, gaps, restored
    size_t m = 0;
    for (size_t k = 0; k < t.count && m < t.capacity; k++) {
        const size_t at = (t.head + t.capacity - 1 - k) % t.capacity;
        origin[m] = (int16_t)at;
        merged[m++] = t.ring[at];
    }
    int64_t lastRestored = INT64_MIN;
    for (int i = count - 1; i >= 0; i--) {
        HistorySample s;
        memcpy(&s, in + HISTORY_BLOB_HEADER + (size_t)i * sizeof(s), sizeof(s));
        if ((int64_t)s.bucketEndSec <= boundary) { lastRestored = s.bucketEndSec; break; }
    }
    int restoredCount = 0;
    if (lastRestored != INT64_MIN) {
        const int64_t bi = windowIndex(t, boundary), li = windowIndex(t, lastRestored);
        for (int64_t g = 0; g < bi - li && m < t.capacity; g++) {
            origin[m] = -1;
            merged[m++] = gapSample((uint32_t)(windowEnd(t, bi - g) - clockOffset));
        }
        for (int i = count - 1; i >= 0 && m < t.capacity; i--) {
            HistorySample s;
            memcpy(&s, in + HISTORY_BLOB_HEADER + (size_t)i * sizeof(s), sizeof(s));
            if ((int64_t)s.bucketEndSec > boundary) continue;
            s.bucketEndSec = (uint32_t)((int64_t)s.bucketEndSec - clockOffset);   // uptime (may wrap: ages are modulo 2^32)
            origin[m] = (int16_t)(-2 - i);
            merged[m++] = s;
            restoredCount++;
        }
    }
    if (restoredCount) {
        // the series first (their values of this run are read from the ring positions before they move)
        int16_t* tmp = (int16_t*)bigAlloc(t.capacity * sizeof(int16_t));
        for (int k = 0; k < SERIES_MAX; k++) {
            if (!t.ser[k]) continue;
            for (size_t i = 0; i < m; i++) {
                const int o = origin[m - 1 - i];
                int16_t v = SERIES_GAP;
                if (o >= 0) v = t.ser[k][o];
                else if (o <= -2 && serAt[k]) memcpy(&v, in + serAt[k] + (size_t)(-2 - o) * 2, 2);
                if (tmp) tmp[i] = v;
            }
            for (size_t i = 0; i < t.capacity; i++) t.ser[k][i] = (tmp && i < m) ? tmp[i] : SERIES_GAP;
        }
        free(tmp);
        for (size_t i = 0; i < m; i++) t.ring[i] = merged[m - 1 - i];   // oldest first
        t.count = m;
        t.head = m % t.capacity;
        t.gen++;
        // the daily tier takes the restored hours over at the next cycle (primeDaily()),
        // the open hour the restored 5-min buckets (primeHour())
        if (&t != &fine) dailyPrimed = false;
        else { hourPrimePending = true; hourPrimeUntil = boundary; }
    }
    free(merged);
    free(origin);
    xSemaphoreGive(historyMutex);
    return restoredCount;
}
