// Derived weather values (src/derived.cpp): dew point, wind chill, heat
// index, cloud base, compass text, pressure tendency, warnings, versions.
#include "derived.h"
#include <cassert>
#include <cmath>
#include <cstring>

static bool near(float a, float b, float eps) { return std::fabs(a - b) < eps; }

int main()
{
    // dew point (Magnus): 20 °C / 50 % -> 9.3 °C; 100 % -> the air temperature
    assert(near(dewPointC(20.0f, 50.0f), 9.3f, 0.1f));
    assert(near(dewPointC(5.0f, 100.0f), 5.0f, 0.01f));
    assert(near(dewPointC(-10.0f, 80.0f), -12.9f, 0.2f));

    // wind chill: -10 °C at 30 km/h -> -19.5 °C; not above 10 °C or below 4.8 km/h
    assert(near(windChillC(-10.0f, 30.0f / 3.6f), -19.5f, 0.2f));
    assert(windChillC(12.0f, 10.0f) == 12.0f && !windChillApplies(12.0f, 10.0f));
    assert(!windChillApplies(0.0f, 1.0f));

    // heat index: 32 °C / 70 % -> about 40.6 °C (NWS table: 90 °F/70 % = 105 °F)
    assert(near(heatIndexC(32.2f, 70.0f), 40.6f, 0.6f));
    assert(heatIndexC(20.0f, 90.0f) == 20.0f);

    // feels like picks wind chill, heat index or the temperature
    assert(near(feelsLikeC(-10.0f, 80.0f, true, 30.0f / 3.6f, true), -19.5f, 0.2f));
    assert(near(feelsLikeC(32.2f, 70.0f, true, 1.0f, true), 40.6f, 0.6f));
    assert(feelsLikeC(15.0f, 60.0f, true, 5.0f, true) == 15.0f);

    // cloud base: 125 m per K spread
    assert(near(cloudBaseM(20.0f, 12.0f), 1000.0f, 0.1f));
    assert(cloudBaseM(5.0f, 5.2f) == 0.0f);

    // compass text
    assert(!strcmp(windDirText(0), "N") && !strcmp(windDirText(359), "N"));
    assert(!strcmp(windDirText(22), "NNE") && !strcmp(windDirText(180), "S"));
    assert(!strcmp(windDirText(-90), "W") && !strcmp(windDirText(300), "WNW"));

    // pressure tendency classes and trend text
    assert(pressureTendency(0.05f) == PT_STEADY);
    assert(pressureTendency(-1.0f) == PT_FALLING_SLOWLY);
    assert(pressureTendency(2.0f) == PT_RISING);
    assert(pressureTendency(-4.0f) == PT_FALLING_QUICKLY);
    assert(pressureTendency(7.0f) == PT_RISING_VERY_RAPIDLY);
    assert(!strcmp(pressureTendencyText(PT_FALLING_SLOWLY), "falling slowly"));
    assert(!strcmp(weatherTrendText(-6.5f, 1000.0f), "storm warning"));
    assert(!strcmp(weatherTrendText(-2.0f, 1010.0f), "worsening"));
    assert(!strcmp(weatherTrendText(2.0f, 1010.0f), "improving"));
    assert(!strcmp(weatherTrendText(0.5f, 1025.0f), "settled"));
    assert(!strcmp(weatherTrendText(0.5f, 995.0f), "unsettled"));
    assert(!strcmp(weatherTrendText(0.5f, 1010.0f), "no change"));

    // warnings
    assert(weatherWarnings(-1.0f, 50.0f, true) == WARN_FROST);
    assert(weatherWarnings(8.0f, 98.0f, true) == WARN_FOG);
    assert(weatherWarnings(26.0f, 70.0f, true) == WARN_SULTRY);   // dew point ~20 °C
    assert(weatherWarnings(15.0f, 60.0f, false) == 0);

    // versions
    assert(compareVersions("2.0.0-beta.3", "2.0.0-beta.2") > 0);
    assert(compareVersions("2.0.0", "2.0.0-beta.9") > 0);
    assert(compareVersions("2.0.0-beta.10", "2.0.0-beta.9") > 0);
    assert(compareVersions("2.0.0-beta.2-3-gabc1234-dirty", "2.0.0-beta.2") == 0);
    assert(compareVersions("2.1.0-beta.1", "2.0.0") > 0);
    assert(compareVersions("v1.1.1", "1.1.1") == 0);
    assert(compareVersions("1.10.0", "1.9.9") > 0);
    assert(compareVersions("dev", "2.0.0") == 0);
    assert(compareVersions("2.0.0-dirty", "2.0.0") == 0);              // bare git-describe "dirty"
    assert(compareVersions("2.0.0-DIRTY", "2.0.0-beta.1") > 0);
    assert(compareVersions("2.0.0-RC.1", "2.0.0-beta.3") > 0);         // tags compare case-insensitively
    assert(compareVersions("2.0.0-Beta.3", "2.0.0-beta.3") == 0);
    assert(compareVersions("2.0.0-rc.1-dirty", "2.0.0-rc.1") == 0);
    return 0;
}
