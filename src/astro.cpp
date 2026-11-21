#include "astro.h"
#include <math.h>

namespace {
constexpr double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;

double jd(int64_t t) { return (double)t / 86400.0 + 2440587.5; }
double norm360(double a) { a = fmod(a, 360.0); return a < 0 ? a + 360.0 : a; }
double norm180(double a) { a = norm360(a); return a > 180.0 ? a - 360.0 : a; }

struct Eq { double ra, dec; };   // degrees

double obliquity(double T, double omega)
{
    const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    return eps0 + 0.00256 * cos(omega * D2R);
}

// apparent ecliptic longitude of the sun (deg) - NOAA / Meeus ch. 25
double sunLongitude(double T, double& omega)
{
    const double L0 = norm360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const double M  = (357.52911 + T * (35999.05029 - 0.0001537 * T)) * D2R;
    const double C  = sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sin(2 * M) * (0.019993 - 0.000101 * T) +
                      sin(3 * M) * 0.000289;
    omega = 125.04 - 1934.136 * T;
    return L0 + C - 0.00569 - 0.00478 * sin(omega * D2R);
}

Eq toEquatorial(double lambda, double beta, double eps)
{
    const double l = lambda * D2R, b = beta * D2R, e = eps * D2R;
    Eq q;
    q.ra  = norm360(atan2(sin(l) * cos(e) - tan(b) * sin(e), cos(l)) * R2D);
    q.dec = asin(sin(b) * cos(e) + cos(b) * sin(e) * sin(l)) * R2D;
    return q;
}

Eq sunEq(int64_t t)
{
    const double T = (jd(t) - 2451545.0) / 36525.0;
    double omega;
    const double lambda = sunLongitude(T, omega);
    return toEquatorial(lambda, 0.0, obliquity(T, omega));
}

// geocentric moon: ecliptic longitude / latitude, horizontal parallax (deg)
void moonEcl(int64_t t, double& lambda, double& beta, double& par)
{
    const double T = (jd(t) - 2451545.0) / 36525.0;
    auto s = [](double a) { return sin(a * D2R); };
    auto c = [](double a) { return cos(a * D2R); };
    lambda = norm360(218.32 + 481267.881 * T + 6.29 * s(135.0 + 477198.87 * T) - 1.27 * s(259.3 - 413335.36 * T) +
                     0.66 * s(235.7 + 890534.22 * T) + 0.21 * s(269.9 + 954397.74 * T) - 0.19 * s(357.5 + 35999.05 * T) -
                     0.11 * s(186.5 + 966404.03 * T));
    beta = 5.13 * s(93.3 + 483202.02 * T) + 0.28 * s(228.2 + 960400.89 * T) - 0.28 * s(318.3 + 6003.15 * T) -
           0.17 * s(217.6 - 407332.21 * T);
    par = 0.9508 + 0.0518 * c(135.0 + 477198.87 * T) + 0.0095 * c(259.3 - 413335.36 * T) + 0.0078 * c(235.7 + 890534.22 * T) +
          0.0028 * c(269.9 + 954397.74 * T);
}

Eq moonEq(int64_t t, double& par)
{
    double lambda, beta;
    moonEcl(t, lambda, beta, par);
    const double T = (jd(t) - 2451545.0) / 36525.0;
    return toEquatorial(lambda, beta, obliquity(T, 125.04 - 1934.136 * T));
}

// hour angle (deg, -180..180) and altitude (deg) of q at t
void horizon(const Eq& q, double latDeg, double lonDeg, int64_t t, double& ha, double& alt)
{
    const double d = jd(t) - 2451545.0, T = d / 36525.0;
    const double gmst = 280.46061837 + 360.98564736629 * d + 0.000387933 * T * T;
    ha = norm180(gmst + lonDeg - q.ra);
    const double la = latDeg * D2R, de = q.dec * D2R;
    alt = asin(sin(la) * sin(de) + cos(la) * cos(de) * cos(ha * D2R)) * R2D;
}

// f(t) for the event search: what = 0 sun altitude, 1 sun hour angle,
// 2 moon altitude - h0(parallax), 3 moon hour angle
double fn(int what, double lat, double lon, int64_t t)
{
    double ha, alt, par = 0.0;
    const Eq q = what < 2 ? sunEq(t) : moonEq(t, par);
    horizon(q, lat, lon, t, ha, alt);
    if (what == 1 || what == 3) return ha;
    return what == 2 ? alt - (0.7275 * par - 0.5667) : alt;
}

// Crossings of fn - h0 in [a, b): rising (- to +) and falling; 0 = none.
// Hour angles: only the - to + crossing (transit), not the 180 deg wrap.
void findEvents(int what, double h0, double lat, double lon, int64_t a, int64_t b, int64_t& up, int64_t& down)
{
    constexpr int64_t STEP = 600;
    up = down = 0;
    const bool angle = what == 1 || what == 3;
    int64_t t0 = a;
    double f0 = fn(what, lat, lon, t0) - h0;
    while (t0 < b) {
        const int64_t t1 = t0 + STEP < b ? t0 + STEP : b;
        const double f1 = fn(what, lat, lon, t1) - h0;
        const bool cross = angle ? (f0 < 0 && f1 >= 0 && f1 - f0 < 90.0) : ((f0 < 0) != (f1 < 0));
        if (cross) {
            const bool rising = f1 >= 0;
            int64_t lo = t0, hi = t1;
            double flo = f0;
            while (hi - lo > 1) {
                const int64_t mid = lo + (hi - lo) / 2;
                const double fm = fn(what, lat, lon, mid) - h0;
                if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
            }
            int64_t& slot = rising ? up : down;
            if (!slot && hi < b) slot = hi;
        }
        t0 = t1; f0 = f1;
    }
}
} // namespace

void astroDay(double lat, double lon, int64_t dayStart, int64_t dayEnd, AstroDay& o)
{
    o = AstroDay();
    o.dayStart = dayStart; o.dayEnd = dayEnd;
    int64_t unused;
    findEvents(0, -0.833, lat, lon, dayStart, dayEnd, o.sunrise, o.sunset);   // refraction + semi-diameter
    findEvents(0, -6.0,   lat, lon, dayStart, dayEnd, o.civilDawn, o.civilDusk);
    findEvents(1, 0.0,    lat, lon, dayStart, dayEnd, o.solarNoon, unused);
    findEvents(2, 0.0,    lat, lon, dayStart, dayEnd, o.moonrise, o.moonset);
    findEvents(3, 0.0,    lat, lon, dayStart, dayEnd, o.moonTransit, unused);
    const int64_t len = dayEnd - dayStart;
    if (!o.sunrise && !o.sunset) {
        const int64_t noon = o.solarNoon ? o.solarNoon : dayStart + len / 2;
        o.polar = fn(0, lat, lon, noon) > -0.833 ? 1 : -1;
        o.dayLengthSec = o.polar > 0 ? (int32_t)len : 0;
    } else if (o.sunrise && o.sunset && o.sunset > o.sunrise) {
        o.dayLengthSec = (int32_t)(o.sunset - o.sunrise);
    } else {   // only one of them, or set before rise (polar transition days)
        o.dayLengthSec = (int32_t)((o.sunset ? o.sunset - dayStart : 0) + (o.sunrise ? dayEnd - o.sunrise : 0));
    }
}

double astroSunElevation(double lat, double lon, int64_t t) { return fn(0, lat, lon, t); }

float astroClearSkyWm2(double el)
{
    if (el <= 0.0) return 0.0f;
    const double s = sin(el * D2R);
    return (float)(1098.0 * s * exp(-0.057 / s));
}

void astroMoon(int64_t t, float& illum, uint8_t& phase)
{
    double lambda, beta, par;
    moonEcl(t, lambda, beta, par);
    const double T = (jd(t) - 2451545.0) / 36525.0;
    double omega;
    const double sunLon = sunLongitude(T, omega);
    const double elong = norm360(lambda - sunLon);   // 0 new, 180 full
    const double cosPsi = cos(beta * D2R) * cos(elong * D2R);
    illum = (float)((1.0 - cosPsi) / 2.0);
    phase = (uint8_t)((int)floor(elong / 45.0 + 0.5) % 8);
}

const char* astroMoonPhaseKey(uint8_t p)
{
    static const char* const k[8] = { "new", "waxingCrescent", "firstQuarter", "waxingGibbous",
                                      "full", "waningGibbous", "lastQuarter", "waningCrescent" };
    return k[p & 7];
}
