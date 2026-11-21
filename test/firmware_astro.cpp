// Astronomy (src/astro.cpp) against the U.S. Naval Observatory
// (aa.usno.navy.mil, "Rise/Set/Transit Times for Major Solar System
// Objects", values rounded to the minute): sun within 1 min, moon within
// 3 min (measured: sun <= 0.5, moon <= 1.1 min), illumination within 6 %
// (USNO gives one value per day).
#include "astro.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// UTC seconds of y-m-d 00:00 (proleptic Gregorian)
static int64_t utcDay(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400;
}

static int fails = 0;
// expected local "hh:mm" (or nullptr = no event that day)
static void check(const char* what, int64_t got, int64_t dayStart, const char* hhmm, int tolMin)
{
    if (!hhmm) {
        if (got) { printf("%s: expected none, got %+.1f h\n", what, (got - dayStart) / 3600.0); fails++; }
        return;
    }
    const int64_t exp = dayStart + (atoi(hhmm) * 60 + atoi(hhmm + 3)) * 60;
    const double diff = (got - exp) / 60.0;
    if (!got || std::fabs(diff) > tolMin) { printf("%s: expected %s, off by %.1f min (got %d)\n", what, hhmm, diff, got != 0); fails++; }
}

struct Ref {
    const char* name; double lat, lon; int y, m, d, tzH;
    const char *rise, *set, *dawn, *dusk, *noon, *mRise, *mSet, *mTransit;
    int illumPct; int8_t polar;
};

int main()
{
    const Ref refs[] = {
        // Berlin, summer solstice 2024 (CEST)
        { "Berlin 2024-06-21", 52.52, 13.405, 2024, 6, 21, 2, "04:43", "21:33", "03:53", "22:24", "13:08", "21:56", "03:21", "00:05", 99, 0 },
        // Berlin, winter solstice 2024 (CET)
        { "Berlin 2024-12-21", 52.52, 13.405, 2024, 12, 21, 1, "08:15", "15:54", "07:33", "16:36", "12:05", "22:59", "11:48", "04:56", 64, 0 },
        // Munich, 10 March 2025 (CET)
        { "Munich 2025-03-10", 48.137, 11.575, 2025, 3, 10, 1, "06:37", "18:12", "06:06", "18:43", "12:24", "13:56", "05:23", "22:00", 86, 0 },
        // Tromsø: polar day, moon below the horizon all day
        { "Tromso 2024-06-21", 69.65, 18.96, 2024, 6, 21, 2, nullptr, nullptr, nullptr, nullptr, "12:46", nullptr, nullptr, nullptr, 99, 1 },
        // Tromsø: polar night (civil twilight only)
        { "Tromso 2024-12-21", 69.65, 18.96, 2024, 12, 21, 1, nullptr, nullptr, "09:32", "13:53", nullptr, "22:04", "12:10", "04:33", 64, -1 },
    };
    for (const Ref& r : refs) {
        const int64_t start = utcDay(r.y, r.m, r.d) - r.tzH * 3600;
        AstroDay a;
        astroDay(r.lat, r.lon, start, start + 86400, a);
        char w[64];
        snprintf(w, sizeof(w), "%s sunrise", r.name);   check(w, a.sunrise, start, r.rise, 1);
        snprintf(w, sizeof(w), "%s sunset", r.name);    check(w, a.sunset, start, r.set, 1);
        snprintf(w, sizeof(w), "%s dawn", r.name);      check(w, a.civilDawn, start, r.dawn, 1);
        snprintf(w, sizeof(w), "%s dusk", r.name);      check(w, a.civilDusk, start, r.dusk, 1);
        if (r.noon) { snprintf(w, sizeof(w), "%s noon", r.name); check(w, a.solarNoon, start, r.noon, 1); }
        snprintf(w, sizeof(w), "%s moonrise", r.name);  check(w, a.moonrise, start, r.mRise, 3);
        snprintf(w, sizeof(w), "%s moonset", r.name);   check(w, a.moonset, start, r.mSet, 3);
        snprintf(w, sizeof(w), "%s transit", r.name);   check(w, a.moonTransit, start, r.mTransit, 3);
        if (a.polar != r.polar) { printf("%s polar %d\n", r.name, a.polar); fails++; }
        float illum; uint8_t phase;
        astroMoon(start + 12 * 3600, illum, phase);
        if (std::fabs(illum * 100.0f - r.illumPct) > 6.0f) { printf("%s illum %.1f\n", r.name, illum * 100.0f); fails++; }
    }
    // day length: Berlin solstice 16 h 50 min, polar day 24 h, polar night 0
    AstroDay a;
    int64_t s = utcDay(2024, 6, 21) - 7200;
    astroDay(52.52, 13.405, s, s + 86400, a);
    assert(std::abs(a.dayLengthSec - (16 * 3600 + 50 * 60)) <= 180);
    astroDay(69.65, 18.96, s, s + 86400, a);
    assert(a.dayLengthSec == 86400 && a.polar == 1 && !a.sunrise);
    s = utcDay(2024, 12, 21) - 3600;
    astroDay(69.65, 18.96, s, s + 86400, a);
    assert(a.dayLengthSec == 0 && a.polar == -1);
    // phases: full moon 2024-06-22 01:08 UTC, new moon 2024-07-05 22:57 UTC, first quarter 2024-07-13 22:49 UTC
    float illum; uint8_t phase;
    astroMoon(utcDay(2024, 6, 22) + 4080, illum, phase);  assert(phase == 4 && illum > 0.99f);
    astroMoon(utcDay(2024, 7, 5) + 82620, illum, phase);  assert(phase == 0 && illum < 0.01f);
    astroMoon(utcDay(2024, 7, 13) + 82140, illum, phase); assert(phase == 2 && std::fabs(illum - 0.5f) < 0.03f);
    assert(std::string(astroMoonPhaseKey(phase)) == "firstQuarter");
    // sun elevation at Berlin solar noon on the solstice: 90 - 52.52 + 23.44 = 60.9 deg
    assert(std::fabs(astroSunElevation(52.52, 13.405, utcDay(2024, 6, 21) + 11 * 3600 + 8 * 60) - 60.9) < 0.2);
    // clear sky: 0 below the horizon, ~1000 W/m2 at a high sun
    assert(astroClearSkyWm2(-1.0) == 0.0f);
    assert(astroClearSkyWm2(60.0) > 850.0f && astroClearSkyWm2(60.0) < 950.0f);
    assert(astroClearSkyWm2(10.0) < 200.0f);
    if (fails) { printf("%d astro mismatches\n", fails); return 1; }
    puts("astro ok");
    return 0;
}
