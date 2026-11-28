#include "awekas.h"
#include "derived.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

struct Buf {
    Buf(char* o, size_t l) : out(o), len(l) {}
    char*  out;
    size_t len, n = 0;
    void add(const char* fmt, ...)
    {
        if (!len || n + 1 >= len) return;
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(out + n, len - n, fmt, ap);
        va_end(ap);
        if (k > 0) n += (size_t)k < len - n ? (size_t)k : len - n - 1;
    }
    void sep() { add(";"); }
};

} // namespace

size_t awekasVal(const AwekasData& d, char* out, size_t len)
{
    Buf b(out, len);
    if (len) out[0] = '\0';
    b.add("%s;%s;%s;%s;", d.user, d.md5Pass, d.date, d.time);        // 1-4
    // one field and its separator; an absent value stays empty
    auto num = [&b](bool present, const char* fmt, double v) {
        if (present) b.add(fmt, v);
        b.sep();
    };
    num(d.hasTemp,  "%.1f", d.tempC);                                 // 5
    num(d.hasHum,   "%.0f", d.humidity);                              // 6
    num(d.hasPress, "%.1f", d.pressureHpa);                           // 7
    num(d.hasRain,  "%.2f", d.dailyRainMm);                           // 8
    num(d.hasWind,  "%.1f", d.windKmh);                               // 9
    num(d.hasWind && d.hasDir, "%.0f", (double)d.dirDeg);             // 10
    b.add(";;;");                                                     // 11 condition, 12 warning text, 13 snow height
    b.add("en;");                                                     // 14 language
    num(d.hasTendency, "%.0f", (double)d.tendency);                   // 15
    num(d.hasWind,  "%.1f", d.gustKmh);                               // 16
    num(d.hasSolar, "%.0f", d.solarWm2);                              // 17
    num(d.hasUv,    "%.1f", d.uvIndex);                               // 18
    num(d.hasLux,   "%.0f", (double)lroundf(d.lux));                  // 19 (integer lux)
    num(d.hasSunshine, "%.2f", d.sunshineHours);                      // 20
    b.sep();                                                          // 21 soil temperature
    num(d.hasRain,  "%.1f", d.rainRateMmH);                           // 22
    b.add("%s;%s;%s", d.software, d.lon, d.lat);                      // 23-25
    return b.n;
}

int awekasTendency(float delta3hHpa)
{
    int k = (int)pressureTendency(delta3hHpa);   // -4..4
    int a = k < 0 ? -k : k;
    int m = a == 0 ? 0 : a <= 2 ? 1 : 2;
    return k < 0 ? -m : m;
}
