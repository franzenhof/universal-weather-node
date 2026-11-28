#pragma once
// Astronomy on the node, from latitude / longitude and the clock only (no
// internet): sunrise / sunset, civil twilight, solar noon, day length, the
// current sun elevation, a clear-sky solar maximum, moonrise / moonset /
// transit, moon illumination and phase.
//
// Own implementation of published formulas (MIT): the sun after the NOAA
// solar calculator equations (Meeus), the moon with the low-precision series
// of the Astronomical Almanac (~0.3 deg, rise / set within a few minutes).
// Events are found by sampling the altitude in 10-minute steps over the local
// day and refining each horizon crossing by bisection - this handles polar
// day / night and days without moonrise or moonset.
// Pure part host-tested (test/firmware_astro.cpp); astro_store.cpp keeps the
// day's result and adds the "astro" object to /api/data.
#include <stdint.h>
#include <ArduinoJson.h>

struct AstroDay {
    int64_t dayStart = 0, dayEnd = 0;   // the local day [start, end), UTC seconds
    int64_t sunrise = 0, sunset = 0;    // 0 = none that day
    int64_t civilDawn = 0, civilDusk = 0;
    int64_t solarNoon = 0;
    int8_t  polar = 0;                  // 1 = sun up all day, -1 = down all day, 0 = normal
    int32_t dayLengthSec = 0;           // time the sun is up that day
    int64_t moonrise = 0, moonset = 0, moonTransit = 0;   // 0 = none that day
};

// The events of the local day [dayStart, dayEnd) (UTC seconds of the local
// midnights - 23 / 25 h on DST change days).
void astroDay(double latDeg, double lonDeg, int64_t dayStart, int64_t dayEnd, AstroDay& out);
// Sun elevation (deg, geometric, no refraction) at UTC time t
double astroSunElevation(double latDeg, double lonDeg, int64_t t);
// Clear-sky global horizontal irradiance (W/m2, Haurwitz model) at a sun elevation
float astroClearSkyWm2(double elevationDeg);
// Moon at t: illuminated fraction (0..1), phase 0..7 (new, waxing crescent,
// first quarter, waxing gibbous, full, waning gibbous, last quarter, waning crescent)
void astroMoon(int64_t t, float& illum, uint8_t& phase);
const char* astroMoonPhaseKey(uint8_t phase);   // "new", "waxingCrescent", ...

// ---- device part (astro_store.cpp) ----
// false = latitude / longitude not set. Takes wxUploadLock (not while holding it).
bool astroLocation(double& lat, double& lon);
void astroCycle();                              // main loop: recompute at the day change / location change
// "astro" object for /api/data (any task, from the cached day and its
// location); false (nothing added) without location or clock
bool astroJson(JsonObject o);
// Sunshine by radiation (Config::sunMethod 1, WMO-like): sun above 3 deg and
// wm2 >= 0.4 x clear-sky maximum now, at the cached day's location. false =
// no location / clock (sunny unset).
bool astroSunnyByRadiation(float wm2, bool& sunny);
