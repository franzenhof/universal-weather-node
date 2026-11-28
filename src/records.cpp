#include "records.h"
#include "time_source.h"
#include "units.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t RECORDS_MAGIC = 0x52454331;   // "REC1"

uint32_t ymdOf(const struct tm& t) { return (uint32_t)((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday); }

void clearPeriod(Records& r, uint8_t p)
{
    for (int q = 0; q < RQ_COUNT; q++) { r.mn[q][p] = RecVal{}; r.mx[q][p] = RecVal{}; }
}

uint32_t fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void isoTime(char* out, size_t len, uint32_t t)
{
    time_t tt = (time_t)t;
    struct tm u;
    gmtime_r(&tt, &u);
    snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
}
} // namespace

bool recordsRoll(Records& r, const struct tm& local)
{
    const uint32_t ymd = ymdOf(local);
    if (ymd == r.ymd || ymdSteppedBack((int32_t)ymd, (int32_t)r.ymd)) return false;   // stepped back over midnight: stay
    if (r.ymd) {
        // after a gap of several days "yesterday" is the last day the node saw
        for (int q = 0; q < RQ_COUNT; q++) { r.mn[q][RP_YESTERDAY] = r.mn[q][RP_TODAY]; r.mx[q][RP_YESTERDAY] = r.mx[q][RP_TODAY]; }
        if (ymd / 100 != r.ymd / 100) { clearPeriod(r, RP_MONTH); r.rainDay[0] = RecVal{}; }
        if (ymd / 10000 != r.ymd / 10000) { clearPeriod(r, RP_YEAR); r.rainDay[1] = RecVal{}; }
    }
    clearPeriod(r, RP_TODAY);
    r.ymd = ymd;
    return true;
}

void recordsUpdate(Records& r, RecQty q, float v, uint32_t t)
{
    if (q >= RQ_COUNT || !isfinite(v) || !t) return;
    static const uint8_t periods[] = { RP_TODAY, RP_MONTH, RP_YEAR, RP_ALL };
    for (uint8_t p : periods) {
        RecVal& mx = r.mx[q][p];
        if (!mx.t || v > mx.v) mx = RecVal{ v, t };
        if (recHasMin(q)) {
            RecVal& mn = r.mn[q][p];
            if (!mn.t || v < mn.v) mn = RecVal{ v, t };
        }
    }
}

void recordsDailyRain(Records& r, float mm)
{
    const uint32_t t = r.ymd;   // the local date (yyyymmdd), not a time
    if (!isfinite(mm) || mm <= 0.0f || !t) return;
    for (RecVal& d : r.rainDay)
        if (!d.t || mm > d.v) d = RecVal{ mm, t };
}

void recordsReset(Records& r, bool allTime)
{
    const uint32_t ymd = r.ymd;
    if (allTime) { r = Records(); r.ymd = ymd; return; }
    for (uint8_t p : { (uint8_t)RP_TODAY, (uint8_t)RP_YESTERDAY, (uint8_t)RP_MONTH, (uint8_t)RP_YEAR }) clearPeriod(r, p);
    r.rainDay[0] = r.rainDay[1] = RecVal{};
}

void recordsEncode(const Records& r, uint8_t* out)
{
    const uint32_t magic = RECORDS_MAGIC;
    memcpy(out, &magic, 4);
    memcpy(out + 4, &r, sizeof(Records));
    const uint32_t h = fnv1a(out, 4 + sizeof(Records));
    memcpy(out + 4 + sizeof(Records), &h, 4);
}

bool recordsDecode(const uint8_t* in, size_t len, Records& out)
{
    if (len != RECORDS_BLOB_SIZE) return false;
    uint32_t magic, h;
    memcpy(&magic, in, 4);
    memcpy(&h, in + 4 + sizeof(Records), 4);
    if (magic != RECORDS_MAGIC || h != fnv1a(in, 4 + sizeof(Records))) return false;
    memcpy(&out, in + 4, sizeof(Records));
    return true;
}

bool recordsBlobIsNewer(const uint8_t* in, size_t len)
{
    if (!in || len < 4) return false;
    uint32_t magic;
    memcpy(&magic, in, 4);
    // "REC" + the version digit in the lowest byte (little-endian in flash)
    return (magic & 0xFFFFFF00u) == (RECORDS_MAGIC & 0xFFFFFF00u) && (magic & 0xFFu) > (RECORDS_MAGIC & 0xFFu);
}

void recordsJson(JsonDocument& doc, const Records& r, const Config& c, bool clockValid)
{
    doc.clear();
    doc["apiVersion"] = 1;
    doc["valid"] = clockValid && r.ymd != 0;   // false: clock not synced yet / nothing recorded
    doc["tempUnit"]     = temperatureUnitLabel(c);
    doc["pressureUnit"] = pressureUnitLabel(c);
    doc["windUnit"]     = windSpeedUnitLabel(c);
    doc["rainUnit"]     = rainUnitLabel(c);
    doc["rainRateUnit"] = rainRateUnitLabel(c);
    static const char* const periods[RP_COUNT] = { "today", "yesterday", "month", "year", "allTime" };
    static const char* const minKeys[RQ_COUNT] = { "tempMin", "humidityMin", "pressureMin", nullptr, nullptr };
    static const char* const maxKeys[RQ_COUNT] = { "tempMax", "humidityMax", "pressureMax", "gustMax", "rainRateMax" };
    auto conv = [&c](uint8_t q, float v) -> float {
        switch (q) {
            case RQ_TEMP:     return roundf(temperatureForOutput(v, c) * 10.0f) / 10.0f;
            case RQ_HUM:      return roundf(v);
            case RQ_PRESS:    return c.unitSystem == 1 ? roundf(pressureForOutput(v, c) * 100.0f) / 100.0f : roundf(v * 10.0f) / 10.0f;
            case RQ_GUST:     return roundf(windSpeedForOutput(v, c) * 10.0f) / 10.0f;
            default:          return roundf(rainRateForOutput(v, c) * 100.0f) / 100.0f;
        }
    };
    char ts[24];
    for (int p = 0; p < RP_COUNT; p++) {
        JsonObject o = doc[periods[p]].to<JsonObject>();
        for (int q = 0; q < RQ_COUNT; q++) {
            if (minKeys[q] && r.mn[q][p].t) {
                JsonObject e = o[minKeys[q]].to<JsonObject>();
                e["value"] = conv((uint8_t)q, r.mn[q][p].v);
                isoTime(ts, sizeof(ts), r.mn[q][p].t); e["time"] = ts;
            }
            if (r.mx[q][p].t) {
                JsonObject e = o[maxKeys[q]].to<JsonObject>();
                e["value"] = conv((uint8_t)q, r.mx[q][p].v);
                isoTime(ts, sizeof(ts), r.mx[q][p].t); e["time"] = ts;
            }
        }
        const int rd = p == RP_MONTH ? 0 : p == RP_YEAR ? 1 : p == RP_ALL ? 2 : -1;
        if (rd >= 0 && r.rainDay[rd].t) {
            JsonObject e = o["rainDayMax"].to<JsonObject>();
            e["value"] = c.unitSystem == 1 ? roundf(rainForOutput(r.rainDay[rd].v, c) * 100.0f) / 100.0f
                                           : roundf(r.rainDay[rd].v * 10.0f) / 10.0f;
            const uint32_t d = r.rainDay[rd].t;   // yyyymmdd (local)
            snprintf(ts, sizeof(ts), "%04lu-%02lu-%02lu", (unsigned long)(d / 10000), (unsigned long)(d / 100 % 100), (unsigned long)(d % 100));
            e["date"] = ts;
        }
    }
}
