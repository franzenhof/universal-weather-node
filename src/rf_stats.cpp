#include "rf_stats.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <math.h>
#include <string.h>

void rfStatRoll(RfStat& s, uint32_t t, uint32_t ymd)
{
    if (ymd && ymd != s.ymd) {   // new local day (or the clock just became known)
        if (s.ymd) { s.rainTodayMm = 0.0f; s.strikesToday = 0; }
        s.ymd = ymd;
    }
    const uint32_t b = t / 600;
    if (b != s.bucket) {
        const uint32_t steps = b > s.bucket ? b - s.bucket : 6;   // time went back (clock set): start over
        for (uint32_t k = 1; k <= steps && k <= 6; k++) {
            s.rainB[(s.bucket + k) % 6] = 0;
            s.strikeB[(s.bucket + k) % 6] = 0;
        }
        if (b < s.bucket) { memset(s.rainB, 0, sizeof(s.rainB)); memset(s.strikeB, 0, sizeof(s.strikeB)); }
        s.bucket = b;
    }
}

void rfStatRain(RfStat& s, float mm, uint32_t t, uint32_t ymd)
{
    rfStatRoll(s, t, ymd);
    if (!(mm > 0.0f) || !isfinite(mm)) return;
    s.rainTodayMm += mm;
    uint16_t& b = s.rainB[s.bucket % 6];
    const long add = lroundf(mm * 100.0f);
    b = (uint16_t)(b + add > 65535 ? 65535 : b + add);
}

int rfStatLightning(RfStat& s, uint8_t count, uint32_t t, uint32_t ymd)
{
    rfStatRoll(s, t, ymd);
    if (!s.countValid) { s.countValid = true; s.lastCount = count; return 0; }
    const uint8_t d = (uint8_t)(count - s.lastCount);   // 8-bit wrap
    s.lastCount = count;
    if (d == 0 || d > 50) return 0;                     // nothing new, or a counter reset (battery change)
    s.strikesToday = (uint16_t)(s.strikesToday + d);
    uint8_t& b = s.strikeB[s.bucket % 6];
    b = (uint8_t)(b + d > 255 ? 255 : b + d);
    if (ymd) s.lastStrike = t;
    return d;
}

float rfStatRain1h(RfStat& s, uint32_t t)
{
    rfStatRoll(s, t, 0);
    uint32_t sum = 0;
    for (uint16_t v : s.rainB) sum += v;
    return sum / 100.0f;
}

float rfStatRecentRate(RfStat& s, uint32_t t)
{
    rfStatRoll(s, t, 0);
    const uint32_t mm100 = (uint32_t)s.rainB[s.bucket % 6] + s.rainB[(s.bucket + 5) % 6];
    const float hours = (600 + t % 600) / 3600.0f;
    return mm100 / 100.0f / hours;
}

int rfStatStrikes1h(RfStat& s, uint32_t t)
{
    rfStatRoll(s, t, 0);
    int sum = 0;
    for (uint8_t v : s.strikeB) sum += v;
    return sum;
}

namespace {
RfStat       stats[RF_STATS_MAX];
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
}

void rfStatsLock()   { portENTER_CRITICAL(&statsMux); }
void rfStatsUnlock() { portEXIT_CRITICAL(&statsMux); }

RfStat* rfStatFor(uint32_t id, bool create)
{
    if (!id) return nullptr;
    for (RfStat& s : stats) if (s.id == id) return &s;
    if (!create) return nullptr;
    for (RfStat& s : stats) if (!s.id) { s = RfStat(); s.id = id; return &s; }
    return nullptr;   // full: no statistics for this sensor
}

void rfStatsClear()
{
    rfStatsLock();
    for (RfStat& s : stats) s = RfStat();
    rfStatsUnlock();
}

bool rfStatSnapshot(uint32_t id, RfStat& out, uint32_t& t)
{
    if (!id) return false;
    uint32_t ymd;
    rfStatsClock(t, ymd);
    bool found = false;
    rfStatsLock();
    for (const RfStat& s : stats) if (s.id == id) { out = s; found = true; break; }
    rfStatsUnlock();
    if (found) rfStatRoll(out, t, ymd);
    return found;
}

void rfStatsPrune(bool (*used)(uint32_t id))
{
    for (RfStat& s : stats) if (s.id && !used(s.id)) s = RfStat();
}

__attribute__((weak)) bool rfStatsClock(uint32_t& t, uint32_t& ymd)
{
    t = millis() / 1000;
    ymd = 0;
    return false;
}
