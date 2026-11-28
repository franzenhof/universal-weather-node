#include "wx_formats.h"
#include "derived.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
struct Buf {
    char* p; size_t len, n; bool ok;
    Buf(char* out, size_t l) : p(out), len(l), n(0), ok(l > 0) { if (l) out[0] = '\0'; }
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
void Buf::add(const char* fmt, ...)
{
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(p + n, len - n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= len - n) { ok = false; p[n] = '\0'; return; }
    n += (size_t)k;
}

void urlPart(Buf& b, const char* s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~')
            b.add("%c", c);
        else
            b.add("%%%c%c", hex[c >> 4], hex[c & 15]);
    }
}

constexpr float MS_TO_MPH = 2.236936f;
float f(float c) { return c * 1.8f + 32.0f; }
} // namespace

bool wowUrl(const WxObs& o, const char* siteId, const char* pin, const char* dateUtc, const char* software,
            char* out, size_t len)
{
    Buf b(out, len);
    b.add("http://wow.metoffice.gov.uk/automaticreading?siteid="); urlPart(b, siteId);
    b.add("&siteAuthenticationKey="); urlPart(b, pin);
    b.add("&dateutc=");               urlPart(b, dateUtc);
    b.add("&softwaretype=");          urlPart(b, software);
    if (o.hasTemp)  b.add("&tempf=%.1f", f(o.tempC));
    if (o.hasHum)   b.add("&humidity=%.0f", o.humidity);
    if (o.hasTemp && o.hasHum) b.add("&dewptf=%.1f", f(dewPointC(o.tempC, o.humidity)));
    if (o.hasPress) b.add("&baromin=%.2f", o.qnhHpa / 33.8639f);
    if (o.hasWind)  b.add("&windspeedmph=%.1f&windgustmph=%.1f", o.windMs * MS_TO_MPH, o.gustMs * MS_TO_MPH);
    if (o.hasWind && o.hasDir) b.add("&winddir=%d", o.dirDeg);
    if (o.hasRain)  b.add("&dailyrainin=%.2f", o.rainDayMm / 25.4f);
    return b.ok;
}

bool cwopPacket(const WxObs& o, const char* call, double lat, double lon, int day, int hour, int minute,
                const char* software, char* out, size_t len)
{
    Buf b(out, len);
    const double alat = fabs(lat), alon = fabs(lon);
    int latD = (int)alat, lonD = (int)alon;
    double latM = (alat - latD) * 60.0, lonM = (alon - lonD) * 60.0;
    if (latM >= 59.995) { latD++; latM = 0; }   // 59.999' would print as 60.00
    if (lonM >= 59.995) { lonD++; lonM = 0; }
    b.add("%s>APRS,TCPIP*:@%02d%02d%02dz%02d%05.2f%c/%03d%05.2f%c_", call, day, hour, minute,
          latD, latM, lat < 0 ? 'S' : 'N', lonD, lonM, lon < 0 ? 'W' : 'E');
    auto clamp = [](long v, long lo, long hi) { return v < lo ? lo : v > hi ? hi : v; };
    if (o.hasWind) {
        if (o.hasDir) b.add("%03d", o.dirDeg % 360);
        else          b.add("...");
        b.add("/%03ld", clamp(lroundf(o.windMs * MS_TO_MPH), 0, 999));
        b.add("g%03ld", clamp(lroundf(o.gustMs * MS_TO_MPH), 0, 999));
    } else {
        b.add(".../...g...");
    }
    if (o.hasTemp) {
        const long t = clamp(lroundf(f(o.tempC)), -99, 999);
        b.add("t%03ld", t);   // -5 -> "t-05" (3 characters incl. the sign)
    } else {
        b.add("t...");
    }
    if (o.hasRain) {
        b.add("r%03ld", clamp(lroundf(o.rain1hMm / 0.254f), 0, 999));
        b.add("p%03ld", clamp(lroundf(o.rain24hMm / 0.254f), 0, 999));
        b.add("P%03ld", clamp(lroundf(o.rainDayMm / 0.254f), 0, 999));
    }
    if (o.hasHum) b.add("h%02ld", clamp(lroundf(o.humidity), 1, 100) % 100);
    if (o.hasPress) b.add("b%05ld", clamp(lroundf(o.qnhHpa * 10.0f), 0, 99999));
    b.add("%s", software ? software : "");
    return b.ok;
}

bool cwopLogin(const char* call, int passcode, const char* software, const char* version, char* out, size_t len)
{
    const int k = snprintf(out, len, "user %s pass %d vers %s %s", call, passcode, software, version);
    return k > 0 && (size_t)k < len;
}
