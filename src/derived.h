#pragma once
// Derived weather values - pure functions (no Arduino/global state), so the
// host tests can check them directly. All inputs/outputs in base units
// (°C, %, m/s, hPa, m); unit conversion happens at the output (units.h).
#include <stdint.h>

// Dew point (Magnus formula, a = 17.62, b = 243.12 °C; WMO, valid -45..60 °C).
float dewPointC(float tempC, float humidityPct);

// Vapour pressure deficit (kPa): saturation vapour pressure (Magnus, the
// same a/b as dewPointC, es = 0.6112 kPa * exp(a*T / (b+T))) times
// (1 - RH/100). Humidity is clamped to 0..100 %: 100 % -> 0, 0 % -> es.
float vpdKpa(float tempC, float humidityPct);

// Wind chill (JAG/TI, used by DWD, MeteoSwiss, NWS): only defined for
// tempC <= 10 °C and wind > 4.8 km/h - otherwise returns tempC.
bool  windChillApplies(float tempC, float windMs);
float windChillC(float tempC, float windMs);

// Heat index (NWS: Steadman simple formula, Rothfusz regression with its
// adjustments from 80 °F / 26.7 °C) - below that it returns tempC.
bool  heatIndexApplies(float tempC, float humidityPct);
float heatIndexC(float tempC, float humidityPct);

// Feels-like temperature: wind chill when it applies, else heat index when
// it applies, else the air temperature.
float feelsLikeC(float tempC, float humidityPct, bool humValid, float windMs, bool windValid);

// Cumulus cloud base above the station (Espy: 125 m per K of temperature -
// dew point spread). Negative spreads (rounding) give 0.
float cloudBaseM(float tempC, float dewPointC);

// 16-point compass text ("N", "NNE", ... "NNW").
const char* windDirText(int deg);

// Barometric tendency over 3 h (Met Office classes, hPa/3 h):
// |d| < 0.1 steady, < 1.6 slowly, < 3.6 (normal), < 6.0 quickly, else very rapidly.
enum PressureTendency : int8_t {
    PT_FALLING_VERY_RAPIDLY = -4, PT_FALLING_QUICKLY, PT_FALLING, PT_FALLING_SLOWLY,
    PT_STEADY = 0,
    PT_RISING_SLOWLY, PT_RISING, PT_RISING_QUICKLY, PT_RISING_VERY_RAPIDLY
};
PressureTendency pressureTendency(float delta3hHpa);
const char* pressureTendencyText(PressureTendency t);   // "falling slowly", "steady", ...

// Simple local trend from the 3-h tendency and the current QNH (no forecast
// model - a barometer's rule of thumb): "storm warning" (falling >= 6 hPa/3 h),
// "worsening" (falling >= 1.6), "improving" (rising >= 1.6), else "settled"
// (QNH >= 1020), "unsettled" (QNH < 1000) or "no change".
const char* weatherTrendText(float delta3hHpa, float qnhHpa);

// Warnings (bit mask): frost (T <= 0 °C), fog/dew (spread <= 1 K and
// RH >= 95 %), sultry (dew point >= 16 °C).
constexpr uint8_t WARN_FROST  = 1 << 0;
constexpr uint8_t WARN_FOG    = 1 << 1;
constexpr uint8_t WARN_SULTRY = 1 << 2;
uint8_t weatherWarnings(float tempC, float humidityPct, bool humValid);

// Sunshine (WMO: direct irradiance >= 120 W/m2) estimated from an
// illuminance sensor: >= SUNSHINE_LUX_THRESHOLD counts as sunshine.
// An approximation - a lux sensor sees global, not direct radiation.
constexpr float SUNSHINE_LUX_THRESHOLD = 15000.0f;

// Version strings like "2.0.0", "2.0.0-beta.3", optionally with a leading
// "v" or a "-N-gHASH[-dirty]" / bare "-dirty" git-describe suffix (ignored).
// Returns <0, 0, >0 like strcmp; a pre-release sorts before its release
// (2.0.0-beta.3 < 2.0.0); pre-release tags compare case-insensitively.
// Unparsable strings compare as equal (0).
int compareVersions(const char* a, const char* b);
