// AWEKAS upload string (src/awekas.cpp): field order, empty fields, the
// new solar / UV / brightness / sunshine / tendency fields.
#include "awekas.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

static std::vector<std::string> fields(const char* s)
{
    std::vector<std::string> f(1);
    for (; *s; s++) {
        if (*s == ';') f.emplace_back();
        else f.back() += *s;
    }
    return f;
}

int main()
{
    char v[400];
    AwekasData d;
    d.user = "station1"; d.md5Pass = "5f4dcc3b5aa765d61d8327deb882cf99"; d.date = "15.06.2026"; d.time = "14:05";
    d.hasTemp = true; d.tempC = 18.44f; d.hasHum = true; d.humidity = 63.4f; d.hasPress = true; d.pressureHpa = 1013.27f;
    d.hasRain = true; d.dailyRainMm = 2.4f; d.rainRateMmH = 0.6f;
    d.hasWind = true; d.windKmh = 12.34f; d.gustKmh = 25.0f; d.dirDeg = 235;
    d.software = "weather-node-2.0.0"; d.lon = "13.04"; d.lat = "47.80";
    // the fields the firmware sent before (no tendency/solar/UV/lux/sunshine): byte-identical
    awekasVal(d, v, sizeof(v));
    assert(std::string(v) == "station1;5f4dcc3b5aa765d61d8327deb882cf99;15.06.2026;14:05;18.4;63;1013.3;2.40;12.3;235;"
                             ";;;en;;25.0;;;;;;0.6;weather-node-2.0.0;13.04;47.80");
    std::vector<std::string> f = fields(v);
    assert(f.size() == 25);   // positions 1..25
    // new fields at their positions (1-based: 15 tendency, 17 solar, 18 UV, 19 lux, 20 sunshine)
    d.hasTendency = true; d.tendency = awekasTendency(-2.1f);
    d.hasSolar = true; d.solarWm2 = 512.4f;
    d.hasUv = true; d.uvIndex = 4.25f;
    d.hasLux = true; d.lux = 30123.6f;
    d.hasSunshine = true; d.sunshineHours = 5.85f;
    awekasVal(d, v, sizeof(v));
    f = fields(v);
    assert(f.size() == 25);
    assert(f[14] == "-1" && f[15] == "25.0" && f[16] == "512" && f[17] == "4.2" && f[18] == "30124" && f[19] == "5.85");
    assert(f[20].empty() && f[21] == "0.6" && f[13] == "en" && f[10].empty() && f[11].empty() && f[12].empty());
    // absent values stay empty, the field count stays
    AwekasData e; e.user = "u"; e.md5Pass = "p"; e.date = "d"; e.time = "t";
    awekasVal(e, v, sizeof(v));
    f = fields(v);
    assert(f.size() == 25 && f[4].empty() && f[6].empty() && f[14].empty() && f[16].empty() && f[19].empty() && f[24].empty());
    // tendency classes (hPa / 3 h): steady, slowly/normal = 1, quickly/very rapidly = 2
    assert(awekasTendency(0.05f) == 0 && awekasTendency(-0.05f) == 0);
    assert(awekasTendency(0.5f) == 1 && awekasTendency(3.0f) == 1 && awekasTendency(-1.0f) == -1);
    assert(awekasTendency(4.0f) == 2 && awekasTendency(-7.0f) == -2);
    // truncation never overruns
    char small[20];
    assert(awekasVal(d, small, sizeof(small)) == sizeof(small) - 1 && strlen(small) == sizeof(small) - 1);
    return 0;
}
