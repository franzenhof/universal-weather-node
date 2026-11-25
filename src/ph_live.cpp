#include "ph_live.h"
#include "globals.h"
#include "sources.h"
#include "sensors.h"
#include "rf_sensors.h"
#include "board_io.h"
#include "time_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void phLiveInit(PhLive& l, const WindStats* w10)
{
    l = PhLive();
    if (w10) {
        l.windValid = w10->valid && srcActive(SG_WIND);
        l.windMs = w10->meanMs; l.gustMs = w10->gustMs; l.dirDeg = w10->dirValid ? w10->dirDeg : windDirection;
    } else {
        l.windValid = srcActive(SG_WIND) && windCycleValid;
        l.windMs = windSpeedMs; l.gustMs = windGustMs; l.dirDeg = windDirection;
    }
    derivedNow(l.d, l.windMs, l.dirDeg);
    int64_t e;
    if (epochNow(e)) {
        time_t t = (time_t)e;
        struct tm u, lt;
        gmtime_r(&t, &u);
        localtime_r(&t, &lt);
        strftime(l.timeUtc, sizeof(l.timeUtc), "%Y-%m-%d %H:%M:%S", &u);
        strftime(l.timeLocal, sizeof(l.timeLocal), "%Y-%m-%d %H:%M:%S", &lt);
        snprintf(l.epoch, sizeof(l.epoch), "%lld", (long long)e);
    }
}

namespace {
bool num(PhValue& o, PhKind k, float v, int dec = -1) { o.kind = k; o.v = v; o.decimals = (int8_t)dec; return true; }
bool text(PhValue& o, const char* s) { if (!s || !*s) return false; o.kind = PK_TEXT; o.text = s; return true; }

bool rfLookup(int i, const char* f, PhValue& o)
{
    if (i < 0 || !gRfSensors[i].used) return false;
    const RfSensorReading& r = gRfSensors[i];
    if (!strcmp(f, "batt")) return num(o, PK_PLAIN, r.batteryOk ? 1 : 0);
    if (!strcmp(f, "temp")) return r.hasTemp && srcRfValueFresh(i, "temp") && num(o, PK_TEMP, r.tempC);
    if (!strcmp(f, "hum"))  return r.hasHumidity && srcRfValueFresh(i, "humidity") && num(o, PK_PLAIN, r.humidity);
    const bool w = r.hasWind && srcRfValueFresh(i, "wind");
    if (!strcmp(f, "wind")) return w && num(o, PK_SPEED, r.windAvgMs);
    if (!strcmp(f, "gust")) return w && num(o, PK_SPEED, r.windGustMs);
    if (!strcmp(f, "dir"))  return w && num(o, PK_PLAIN, r.windDirDeg);
    if (!strcmp(f, "rain")) return r.hasRain && srcRfValueFresh(i, "rain") && num(o, PK_RAIN, r.rainTotalMm);
    if (!strcmp(f, "uv"))   return r.hasUv && srcRfValueFresh(i, "uv") && num(o, PK_PLAIN, r.uvIndex, 1);
    if (!strcmp(f, "lux"))  return r.hasLight && srcRfValueFresh(i, "light") && num(o, PK_PLAIN, r.lightLux);
    return false;
}
} // namespace

bool phLiveLookup(const char* n, PhValue& o, void* ctx)
{
    const PhLive& l = *static_cast<const PhLive*>(ctx);
    const DerivedNow& d = l.d;
    if (!strncmp(n, "rf", 2)) {   // no other name starts with "rf"
        const char* field = nullptr;
        const int i = rfIndexOfPlaceholder(n, &field);
        return i >= 0 && rfLookup(i, field, o);
    }
    if (!strcmp(n, "temp"))      return gSrc.tempValid && num(o, PK_TEMP, gSrc.tempC);
    if (!strcmp(n, "hum"))       return gSrc.tempValid && gSrc.humValid && num(o, PK_PLAIN, gSrc.humidity);
    if (!strcmp(n, "dew"))       return d.dewValid && num(o, PK_TEMP, d.dewPointC);
    if (!strcmp(n, "feels"))     return d.feelsValid && num(o, PK_TEMP, d.feelsLikeC);
    if (!strcmp(n, "qnh"))       return gSrc.pressValid && num(o, PK_PRESS, gSrc.pressSeaHpa);
    if (!strcmp(n, "press"))     return gSrc.pressValid && num(o, PK_PRESS, gSrc.pressAbsHpa);
    if (!strcmp(n, "ptend3h"))   return d.pressTrendValid && num(o, PK_PRESS, d.pressDelta3hHpa);
    if (!strcmp(n, "wind"))      return l.windValid && num(o, PK_SPEED, l.windMs);
    if (!strcmp(n, "gust"))      return l.windValid && num(o, PK_SPEED, l.gustMs);
    if (!strcmp(n, "dir"))       return l.windValid && num(o, PK_PLAIN, l.dirDeg);
    const bool rain = srcActive(SG_RAIN);
    if (!strcmp(n, "rain_day"))  return rain && d.daily.valid && num(o, PK_RAIN, d.daily.rainTodayMm);
    if (!strcmp(n, "rain_1h"))   return rain && d.rainValid && num(o, PK_RAIN, d.rain1hMm);
    if (!strcmp(n, "rain_24h"))  return rain && d.rainValid && num(o, PK_RAIN, d.rain24hMm);
    if (!strcmp(n, "rain_rate")) return rain && num(o, PK_RATE, rainRateMmH);
    if (!strcmp(n, "uv")) {
        if (gSrc.uvRadio) return num(o, PK_PLAIN, gSrc.uvIndexRadio, 1);
        return gSrc.uvBoard && gIo.uvIndexCalibrated && isfinite(gIo.uvIndex) && num(o, PK_PLAIN, gIo.uvIndex, 1);
    }
    if (!strcmp(n, "lux"))       return gSrc.lightValid && num(o, PK_PLAIN, gSrc.lightLux);
    if (!strcmp(n, "solar")) {
        const int li = srcResolve(SG_LIGHT);
        return li >= 0 && gRfSensors[li].hasSolar && num(o, PK_PLAIN, gRfSensors[li].solarWm2);
    }
    if (!strcmp(n, "time_utc"))   return text(o, l.timeUtc);
    if (!strcmp(n, "time_local")) return text(o, l.timeLocal);
    if (!strcmp(n, "epoch"))      return text(o, l.epoch);
    if (!strcmp(n, "station"))    return text(o, gCfg.deviceName);
    if (!strcmp(n, "lat"))        return text(o, gCfg.latitude);
    if (!strcmp(n, "lon"))        return text(o, gCfg.longitude);
    return false;
}

const char* const PH_LIVE_NAMES[] = {
    "temp", "hum", "dew", "feels", "qnh", "press", "ptend3h", "wind", "gust", "dir",
    "rain_day", "rain_1h", "rain_24h", "rain_rate", "uv", "lux", "solar",
    "time_local", "time_utc", "epoch", "station", "lat", "lon",
};
const int PH_LIVE_NAME_COUNT = sizeof(PH_LIVE_NAMES) / sizeof(PH_LIVE_NAMES[0]);
const char* const PH_RF_FIELDS[] = { "temp", "hum", "wind", "gust", "dir", "rain", "uv", "lux", "batt" };
const int PH_RF_FIELD_COUNT = sizeof(PH_RF_FIELDS) / sizeof(PH_RF_FIELDS[0]);
