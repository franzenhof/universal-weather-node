#include "derived.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

float dewPointC(float tempC, float humidityPct)
{
    float rh = humidityPct < 1.0f ? 1.0f : (humidityPct > 100.0f ? 100.0f : humidityPct);
    const float a = 17.62f, b = 243.12f;
    float g = logf(rh / 100.0f) + a * tempC / (b + tempC);
    return b * g / (a - g);
}

float vpdKpa(float tempC, float humidityPct)
{
    float rh = humidityPct < 0.0f ? 0.0f : (humidityPct > 100.0f ? 100.0f : humidityPct);
    const float a = 17.62f, b = 243.12f;
    float es = 0.6112f * expf(a * tempC / (b + tempC));
    return es * (1.0f - rh / 100.0f);
}

bool windChillApplies(float tempC, float windMs)
{
    return tempC <= 10.0f && windMs * 3.6f > 4.8f;
}

float windChillC(float tempC, float windMs)
{
    if (!windChillApplies(tempC, windMs)) return tempC;
    float v016 = powf(windMs * 3.6f, 0.16f);
    float wc = 13.12f + 0.6215f * tempC - 11.37f * v016 + 0.3965f * tempC * v016;
    return wc < tempC ? wc : tempC;
}

bool heatIndexApplies(float tempC, float humidityPct)
{
    return tempC >= 26.7f && humidityPct >= 0.0f;
}

float heatIndexC(float tempC, float humidityPct)
{
    if (!heatIndexApplies(tempC, humidityPct)) return tempC;
    float t = tempC * 1.8f + 32.0f, rh = humidityPct;
    float hi = 0.5f * (t + 61.0f + (t - 68.0f) * 1.2f + rh * 0.094f);
    if ((hi + t) / 2.0f >= 80.0f) {
        hi = -42.379f + 2.04901523f * t + 10.14333127f * rh - 0.22475541f * t * rh
             - 0.00683783f * t * t - 0.05481717f * rh * rh + 0.00122874f * t * t * rh
             + 0.00085282f * t * rh * rh - 0.00000199f * t * t * rh * rh;
        if (rh < 13.0f && t >= 80.0f && t <= 112.0f)
            hi -= (13.0f - rh) / 4.0f * sqrtf((17.0f - fabsf(t - 95.0f)) / 17.0f);
        else if (rh > 85.0f && t >= 80.0f && t <= 87.0f)
            hi += (rh - 85.0f) / 10.0f * (87.0f - t) / 5.0f;
    }
    return (hi - 32.0f) / 1.8f;
}

float feelsLikeC(float tempC, float humidityPct, bool humValid, float windMs, bool windValid)
{
    if (windValid && windChillApplies(tempC, windMs)) return windChillC(tempC, windMs);
    if (humValid && heatIndexApplies(tempC, humidityPct)) return heatIndexC(tempC, humidityPct);
    return tempC;
}

float cloudBaseM(float tempC, float dewPoint)
{
    float spread = tempC - dewPoint;
    return spread > 0.0f ? 125.0f * spread : 0.0f;
}

const char* windDirText(int deg)
{
    static const char* const names[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                           "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    int d = ((deg % 360) + 360) % 360;
    return names[(int)((d + 11.25f) / 22.5f) % 16];
}

PressureTendency pressureTendency(float d)
{
    float a = fabsf(d);
    int k = a < 0.1f ? 0 : a < 1.6f ? 1 : a < 3.6f ? 2 : a < 6.0f ? 3 : 4;
    return (PressureTendency)(d < 0 ? -k : k);
}

const char* pressureTendencyText(PressureTendency t)
{
    switch (t) {
        case PT_FALLING_VERY_RAPIDLY: return "falling very rapidly";
        case PT_FALLING_QUICKLY:      return "falling quickly";
        case PT_FALLING:              return "falling";
        case PT_FALLING_SLOWLY:       return "falling slowly";
        case PT_RISING_SLOWLY:        return "rising slowly";
        case PT_RISING:               return "rising";
        case PT_RISING_QUICKLY:       return "rising quickly";
        case PT_RISING_VERY_RAPIDLY:  return "rising very rapidly";
        default:                      return "steady";
    }
}

const char* weatherTrendText(float d, float qnh)
{
    if (d <= -6.0f) return "storm warning";
    if (d <= -1.6f) return "worsening";
    if (d >= 1.6f)  return "improving";
    if (qnh >= 1020.0f) return "settled";
    if (qnh < 1000.0f)  return "unsettled";
    return "no change";
}

uint8_t weatherWarnings(float tempC, float humidityPct, bool humValid)
{
    uint8_t w = 0;
    if (tempC <= 0.0f) w |= WARN_FROST;
    if (humValid) {
        float td = dewPointC(tempC, humidityPct);
        if (tempC - td <= 1.0f && humidityPct >= 95.0f) w |= WARN_FOG;
        if (td >= 16.0f) w |= WARN_SULTRY;
    }
    return w;
}

namespace {
struct Ver { long n[3]; bool pre; char preTag[24]; long preNum; };

bool parseVersion(const char* s, Ver& v)
{
    memset(&v, 0, sizeof(v));
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        char* end;
        v.n[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2) { if (*s != '.') return false; s++; }
    }
    // "-beta.3" (a "-12-gabc" describe suffix starts with a digit; a bare
    // "-dirty" from git describe is not a pre-release either)
    if (*s == '-' && !isdigit((unsigned char)s[1]) && strcasecmp(s + 1, "dirty") != 0) {
        s++;
        v.pre = true;
        size_t k = 0;
        while (*s && *s != '.' && *s != '-' && k < sizeof(v.preTag) - 1) v.preTag[k++] = *s++;
        if (*s == '.') v.preNum = strtol(s + 1, nullptr, 10);
    }
    return true;
}
} // namespace

int compareVersions(const char* a, const char* b)
{
    Ver x, y;
    if (!parseVersion(a, x) || !parseVersion(b, y)) return 0;
    for (int i = 0; i < 3; i++) if (x.n[i] != y.n[i]) return x.n[i] < y.n[i] ? -1 : 1;
    if (x.pre != y.pre) return x.pre ? -1 : 1;
    if (!x.pre) return 0;
    int c = strcasecmp(x.preTag, y.preTag);   // "RC.1" > "beta.3" like "rc.1"
    if (c) return c < 0 ? -1 : 1;
    return x.preNum == y.preNum ? 0 : (x.preNum < y.preNum ? -1 : 1);
}
