// Astronomy: device part (see astro.h) - the day's events, cached per local
// day and location, and the "astro" object of /api/data.
#include "astro.h"
#include "globals.h"
#include "time_sync.h"
#include "wx_upload.h"
#include <freertos/FreeRTOS.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {
AstroDay     day;
double       dayLat = NAN, dayLon = NAN;
bool         dayValid = false;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void iso(JsonObject o, const char* key, int64_t t)
{
    if (!t) return;
    time_t tt = (time_t)t;
    struct tm u;
    gmtime_r(&tt, &u);
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &u);
    o[key] = b;
}

int64_t localMidnight(struct tm lt, int addDays)
{
    lt.tm_mday += addDays;
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return (int64_t)mktime(&lt);
}
} // namespace

bool astroLocation(double& lat, double& lon)
{
    // wxUploadLock: the web task (settings) and the GNSS take-over write them
    wxUploadLock();
    char* e1 = nullptr;
    char* e2 = nullptr;
    lat = strtod(gCfg.latitude, &e1);
    lon = strtod(gCfg.longitude, &e2);
    const bool ok = gCfg.latitude[0] && gCfg.longitude[0] && e1 != gCfg.latitude && e2 != gCfg.longitude;
    wxUploadUnlock();
    return ok && fabs(lat) <= 90.0 && fabs(lon) <= 180.0;
}

void astroCycle()
{
    double lat, lon;
    struct tm lt;
    if (!astroLocation(lat, lon) || !localTimeNow(lt)) {
        portENTER_CRITICAL(&mux);
        dayValid = false;
        portEXIT_CRITICAL(&mux);
        return;
    }
    const int64_t start = localMidnight(lt, 0);
    if (dayValid && day.dayStart == start && lat == dayLat && lon == dayLon) return;
    AstroDay d;
    astroDay(lat, lon, start, localMidnight(lt, 1), d);   // ~150 position samples per body: a few ms, once a day
    portENTER_CRITICAL(&mux);
    day = d; dayLat = lat; dayLon = lon; dayValid = true;
    portEXIT_CRITICAL(&mux);
}

bool astroJson(JsonObject o)
{
    double lat, lon;
    int64_t now;
    AstroDay d;
    bool valid;
    portENTER_CRITICAL(&mux);   // the cached day and the location it was computed for
    valid = dayValid; d = day; lat = dayLat; lon = dayLon;
    portEXIT_CRITICAL(&mux);
    if (!valid || !epochNow(now)) return false;
    iso(o, "sunrise", d.sunrise);
    iso(o, "sunset", d.sunset);
    iso(o, "civilDawn", d.civilDawn);
    iso(o, "civilDusk", d.civilDusk);
    iso(o, "solarNoon", d.solarNoon);
    if (d.polar) o["polar"] = d.polar > 0 ? "day" : "night";
    o["dayLengthMin"] = (d.dayLengthSec + 30) / 60;
    const double el = astroSunElevation(lat, lon, now);
    o["sunElevation"] = roundf((float)el * 10.0f) / 10.0f;
    o["clearSkyWm2"]  = (long)lroundf(astroClearSkyWm2(el));
    iso(o, "moonrise", d.moonrise);
    iso(o, "moonset", d.moonset);
    iso(o, "moonTransit", d.moonTransit);
    float illum;
    uint8_t phase;
    astroMoon(now, illum, phase);
    o["moonIlluminationPct"] = (long)lroundf(illum * 100.0f);
    o["moonPhase"]           = astroMoonPhaseKey(phase);
    o["moonPhaseIndex"]      = phase;
    return true;
}

bool astroSunnyByRadiation(float wm2, bool& sunny)
{
    double lat, lon;
    int64_t now;
    bool valid;
    portENTER_CRITICAL(&mux);   // the location of the cached day (any task, no lock on gCfg)
    valid = dayValid; lat = dayLat; lon = dayLon;
    portEXIT_CRITICAL(&mux);
    if (!valid || !epochNow(now)) return false;
    const double el = astroSunElevation(lat, lon, now);
    sunny = el > 3.0 && wm2 >= 0.4f * astroClearSkyWm2(el);
    return true;
}
