#include "history.h"
#include "globals.h"
#include "units.h"
#include <Arduino.h>
#include <DallasTemperature.h>
#include <esp_timer.h>

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
struct HistorySample {
    uint32_t bucketEndSec;      // uptimeSec() when this bucket was finalized
    uint8_t  validMask;
    int16_t  windSpeedX100;     // average over the bucket
    int16_t  windGustX100;      // max over the bucket
    uint16_t windDirDeg;        // last valid reading in the bucket (not averaged — bearings wrap)
    int16_t  rainRateX100;      // average over the bucket
    uint16_t rainAmountX10;     // *sum* over the bucket (mm accumulated, not a rate)
    int16_t  tempCx10;          // average over the bucket
    uint8_t  humidityX2;        // average over the bucket
    uint16_t pressureQnhX10;    // average over the bucket
    uint16_t batteryVoltX100;   // average over the bucket
    int16_t  ds18b20X10[3];     // average over the bucket, per probe slot
};

constexpr uint8_t VALID_WIND   = 1 << 0;
constexpr uint8_t VALID_RAIN   = 1 << 1;
constexpr uint8_t VALID_BME280 = 1 << 2;
constexpr uint8_t VALID_LIPO   = 1 << 3;

// Defensive clamps before scaling to their fixed-width int fields, same
// rationale as clampLppAnalog() in lora_payload.cpp — cheap insurance
// against a silent wraparound rather than an actual expected case.
constexpr float RAIN_RATE_LIMIT_MMH = 327.67f;
constexpr float RAIN_AMOUNT_LIMIT_MM = 6553.5f;

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
    unsigned long startSec = 0;
    uint16_t windCount = 0, rainCount = 0, bmeCount = 0, lipoCount = 0;
    float    windSpeedSum = 0, windGustMax = 0;
    uint16_t windDirLast = 0;
    float    rainRateSum = 0, rainAmountSum = 0;
    float    tempSum = 0, humiditySum = 0, pressureSum = 0, batterySum = 0;
    float    ds18b20Sum[3] = {0, 0, 0};
    uint16_t ds18b20Count[3] = {0, 0, 0};
};

HistorySample fineRing[HISTORY_FINE_CAPACITY];
size_t fineHead = 0, fineCount = 0;
OpenBucket fineOpen;

HistorySample coarseRing[HISTORY_COARSE_CAPACITY];
size_t coarseHead = 0, coarseCount = 0;
OpenBucket coarseOpen;

// Guards both tiers' ring[]/head/count and their OpenBucket accumulators.
// pushHistorySample() (main loop task) and buildHistoryJson() (servable from
// the dedicated web-server task, see web_api.cpp) can genuinely run
// concurrently on the two ESP32-S3 cores — unlike a plain scalar (see the
// `volatile` note on Config's int fields), an in-progress head/count update
// read mid-write could hand back an out-of-bounds ring[] index, not just a
// stale value, so this needs a real mutex rather than just visibility
// guarantees.
SemaphoreHandle_t historyMutex = xSemaphoreCreateMutex();

void mergeIntoBucket(OpenBucket& b, unsigned long nowSec)
{
    if (!b.started) { b.startSec = nowSec; b.started = true; }

    if (gCfg.windEn) {
        b.windGustMax = (b.windCount == 0) ? windGustMs : max(b.windGustMax, windGustMs);
        b.windSpeedSum += windSpeedMs;
        b.windDirLast   = windDirection;
        b.windCount++;
    }
    if (gCfg.rainEn) {
        b.rainRateSum   += constrain(rainRateMmH, -RAIN_RATE_LIMIT_MMH, RAIN_RATE_LIMIT_MMH);
        b.rainAmountSum += constrain(rainMM, 0.0f, RAIN_AMOUNT_LIMIT_MM);
        b.rainCount++;
    }
    if (bme280Present) {
        b.tempSum     += bme280TempC;
        b.humiditySum += bme280Humidity;
        b.pressureSum += bme280PressSea;
        b.bmeCount++;
    }
    if (gCfg.lipoEn) {
        b.batterySum += batteryVolt;
        b.lipoCount++;
    }
    for (int i = 0; i < 3 && i < bus1Count; i++) {
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C) {
            b.ds18b20Sum[i] += bus1Temps[i];
            b.ds18b20Count[i]++;
        }
    }
}

// Converts an OpenBucket's running sums into a HistorySample without
// touching the bucket itself — shared by finalizeBucket() (which then
// appends the result and resets the bucket) and serializeTier() (which uses
// it to preview the *current*, not-yet-finalized bucket as a live point, see
// below). Returns false for an empty bucket (e.g. the node was off/
// unreachable for a whole window) rather than producing a misleadingly
// "valid" all-zero sample.
bool sampleFromBucket(const OpenBucket& b, unsigned long endSec, HistorySample& out)
{
    bool anyDs18b20 = b.ds18b20Count[0] || b.ds18b20Count[1] || b.ds18b20Count[2];
    if (!(b.windCount || b.rainCount || b.bmeCount || b.lipoCount || anyDs18b20)) return false;

    out = HistorySample{};
    out.bucketEndSec = endSec;
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
        out.rainAmountX10  = (uint16_t)roundf(b.rainAmountSum * 10.0f);
    }
    if (b.bmeCount) {
        out.validMask      |= VALID_BME280;
        out.tempCx10        = (int16_t)roundf(b.tempSum / b.bmeCount * 10.0f);
        out.humidityX2      = (uint8_t)roundf(b.humiditySum / b.bmeCount * 2.0f);
        out.pressureQnhX10  = (uint16_t)roundf(b.pressureSum / b.bmeCount * 10.0f);
    }
    if (b.lipoCount) {
        out.validMask       |= VALID_LIPO;
        out.batteryVoltX100  = (uint16_t)roundf(b.batterySum / b.lipoCount * 100.0f);
    }
    return true;
}

// Appends the OpenBucket (if non-empty) to the given tier's ring, then
// resets it for the next window.
void finalizeBucket(OpenBucket& b, HistorySample* ring, size_t capacity,
                     size_t& head, size_t& count, unsigned long endSec)
{
    HistorySample s;
    if (sampleFromBucket(b, endSec, s)) {
        ring[head] = s;
        head = (head + 1) % capacity;
        if (count < capacity) count++;
    }
    b = OpenBucket{};
}

// Shared by both tiers in buildHistoryJson() — capacity/ring/head/count
// differ, so this is a plain helper rather than a template
// (nothing here depends on the capacity being a compile-time constant).
// `openBucket` is the tier's current, not-yet-finalized bucket: appended as
// one extra, newest ("age" 0) point after the finalized ones whenever it has
// at least one merge, so the chart doesn't sit on "no data yet" for the
// bucket's whole window (5 min/1h) right after boot or a clear — a live
// preview naturally gets more representative as the bucket fills, same as
// any in-progress average.
void serializeTier(JsonDocument& doc, unsigned long nowSec, const HistorySample* ring,
                    size_t capacity, size_t head, size_t count, const OpenBucket& openBucket)
{
    JsonArray ageSec      = doc["ageSec"].to<JsonArray>();
    JsonArray windSpeed   = doc["windSpeed"].to<JsonArray>();
    JsonArray windGust    = doc["windGust"].to<JsonArray>();
    JsonArray windDirDeg  = doc["windDirDeg"].to<JsonArray>();
    JsonArray rainRate    = doc["rainRate"].to<JsonArray>();
    JsonArray rainMm      = doc["rainMm"].to<JsonArray>();
    JsonArray temp        = doc["temp"].to<JsonArray>();
    JsonArray humidityPct = doc["humidityPct"].to<JsonArray>();
    JsonArray pressureQnh = doc["pressureQnh"].to<JsonArray>();
    JsonArray batteryVolt = doc["batteryVolt"].to<JsonArray>();
    JsonArray ds18b20_0   = doc["ds18b20_0"].to<JsonArray>();
    JsonArray ds18b20_1   = doc["ds18b20_1"].to<JsonArray>();
    JsonArray ds18b20_2   = doc["ds18b20_2"].to<JsonArray>();
    JsonArray ds18b20Arrays[3] = { ds18b20_0, ds18b20_1, ds18b20_2 };

    auto addSample = [&](const HistorySample& s, unsigned long ageSecVal) {
        ageSec.add(ageSecVal);

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
            rainMm.add(rainForOutput(s.rainAmountX10 / 10.0f, gCfg));
        } else {
            rainRate.add(nullptr);
            rainMm.add(nullptr);
        }
        if (s.validMask & VALID_BME280) {
            temp.add(temperatureForOutput(s.tempCx10 / 10.0f, gCfg));
            humidityPct.add(s.humidityX2 / 2.0f);
            pressureQnh.add(pressureForOutput(s.pressureQnhX10 / 10.0f, gCfg));
        } else {
            temp.add(nullptr);
            humidityPct.add(nullptr);
            pressureQnh.add(nullptr);
        }
        if (s.validMask & VALID_LIPO) {
            batteryVolt.add(s.batteryVoltX100 / 100.0f);
        } else {
            batteryVolt.add(nullptr);
        }
        for (int i = 0; i < 3; i++) {
            if (s.ds18b20X10[i] == DS18B20_INVALID_X10) ds18b20Arrays[i].add(nullptr);
            else ds18b20Arrays[i].add(temperatureForOutput(s.ds18b20X10[i] / 10.0f, gCfg));
        }
    };

    // Oldest-first: before the buffer first wraps, that's simply index 0;
    // once full, head already points at the oldest (next-to-overwrite) slot.
    size_t start = (count < capacity) ? 0 : head;
    for (size_t i = 0; i < count; i++) {
        const HistorySample& s = ring[(start + i) % capacity];
        addSample(s, nowSec - s.bucketEndSec);
    }

    HistorySample liveSample;
    if (sampleFromBucket(openBucket, nowSec, liveSample)) {
        addSample(liveSample, 0);
    }

    doc["windUnit"]     = windSpeedUnitLabel(gCfg);
    doc["rainRateUnit"] = rainRateUnitLabel(gCfg);
    doc["rainUnit"]     = rainUnitLabel(gCfg);
    doc["tempUnit"]     = temperatureUnitLabel(gCfg);
    doc["pressureUnit"] = pressureUnitLabel(gCfg);
}

} // namespace

void pushHistorySample()
{
    unsigned long nowSec = uptimeSec();

    xSemaphoreTake(historyMutex, portMAX_DELAY);

    mergeIntoBucket(fineOpen, nowSec);
    mergeIntoBucket(coarseOpen, nowSec);

    if (nowSec - fineOpen.startSec >= HISTORY_FINE_BUCKET_SEC) {
        finalizeBucket(fineOpen, fineRing, HISTORY_FINE_CAPACITY, fineHead, fineCount, nowSec);
    }
    if (nowSec - coarseOpen.startSec >= HISTORY_COARSE_BUCKET_SEC) {
        finalizeBucket(coarseOpen, coarseRing, HISTORY_COARSE_CAPACITY, coarseHead, coarseCount, nowSec);
    }

    xSemaphoreGive(historyMutex);
}

void buildHistoryJson(JsonDocument& doc, bool coarse)
{
    doc.clear();
    unsigned long nowSec = uptimeSec();

    // Holds the lock for the whole read+JSON-build loop, not just an index
    // snapshot — simpler than copying samples out first, and the lock-out is
    // short (at most a few hundred iterations of scaling math) and
    // infrequent (one request at a time, one push per uplink cycle).
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    if (coarse) serializeTier(doc, nowSec, coarseRing, HISTORY_COARSE_CAPACITY, coarseHead, coarseCount, coarseOpen);
    else        serializeTier(doc, nowSec, fineRing,   HISTORY_FINE_CAPACITY,   fineHead,   fineCount,   fineOpen);
    xSemaphoreGive(historyMutex);
}
