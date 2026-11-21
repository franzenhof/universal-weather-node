#include "derived_now.h"
#include "globals.h"
#include "history.h"
#include "sources.h"

void derivedNow(DerivedNow& d, float windMs, int dirDeg)
{
    d = DerivedNow{};
    bool wind = srcActive(SG_WIND);
    if (gSrc.tempValid) {
        float t = gSrc.tempC;
        if (gSrc.humValid) {
            d.dewValid   = true;
            d.dewPointC  = dewPointC(t, gSrc.humidity);
            d.vpdValid   = true;
            d.vpdKpa     = vpdKpa(t, gSrc.humidity);
            d.cloudValid = true;
            d.cloudBaseM = cloudBaseM(t, d.dewPointC);
        }
        d.feelsValid = true;
        d.windChill  = wind && windChillApplies(t, windMs);
        d.heatIndex  = !d.windChill && gSrc.humValid && heatIndexApplies(t, gSrc.humidity);
        d.feelsLikeC = feelsLikeC(t, gSrc.humidity, gSrc.humValid, windMs, wind);
        d.warnValid  = true;
        d.warnings   = weatherWarnings(t, gSrc.humidity, gSrc.humValid);
        float t1h;
        if (historyTempAgo(3600, t1h)) { d.tempTrendValid = true; d.tempDelta1hC = t - t1h; }
    }
    if (wind) d.windDirText = windDirText(dirDeg);
    if (gSrc.pressValid) {
        float p3h;
        if (historyPressureAgo(3 * 3600, p3h)) {
            d.pressTrendValid = true;
            d.pressDelta3hHpa = gSrc.pressSeaHpa - p3h;
            d.tendency        = pressureTendency(d.pressDelta3hHpa);
            d.weatherTrend    = weatherTrendText(d.pressDelta3hHpa, gSrc.pressSeaHpa);
        }
    }
    if (srcActive(SG_RAIN)) {
        d.rainValid = true;
        d.rain1hMm  = historyRainSum(3600);
        d.rain24hMm = historyRainSum(24 * 3600);
    }
    d.sunValid = srcActive(SG_LIGHT);
    wxDailyStats(d.daily);
}
