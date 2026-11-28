#include "nmea.h"
#include "time_source.h"

#include <stdlib.h>
#include <string.h>

namespace {

constexpr int MAX_FIELDS = 20;

struct Fields {
    const char* p[MAX_FIELDS];
    uint8_t     n[MAX_FIELDS];
    int         count = 0;
    bool empty(int i) const { return i >= count || n[i] == 0; }
};

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// n digits at p as a number, -1 if one is not a digit
int digits(const char* p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isDigit(p[i])) return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

// A decimal number field ("12.5", "-3", "4807.038"); false if empty or malformed.
bool number(const Fields& f, int i, double& out)
{
    if (f.empty(i) || f.n[i] >= 20) return false;
    char tmp[20];
    memcpy(tmp, f.p[i], f.n[i]);
    tmp[f.n[i]] = 0;
    for (int k = 0; tmp[k]; k++)
        if (!isDigit(tmp[k]) && tmp[k] != '.' && !(k == 0 && (tmp[k] == '-' || tmp[k] == '+'))) return false;
    char* end = nullptr;
    out = strtod(tmp, &end);
    return end && *end == 0 && end != tmp;
}

// "hhmmss" or "hhmmss.s[s[s]]"
bool timeField(const Fields& f, int i, uint32_t& msOfDay)
{
    if (f.empty(i) || f.n[i] < 6) return false;
    const char* p = f.p[i];
    const int hh = digits(p, 2), mm = digits(p + 2, 2), ss = digits(p + 4, 2);
    if (hh < 0 || mm < 0 || ss < 0 || hh > 23 || mm > 59 || ss > 59) return false;
    uint32_t ms = 0;
    if (f.n[i] > 6) {
        if (p[6] != '.') return false;
        uint32_t scale = 100;
        for (int k = 7; k < f.n[i]; k++) {
            if (!isDigit(p[k])) return false;
            ms += (uint32_t)(p[k] - '0') * scale;
            scale /= 10;   // digits beyond the third: ignored
        }
    }
    msOfDay = ((uint32_t)hh * 3600 + (uint32_t)mm * 60 + (uint32_t)ss) * 1000 + ms;
    return true;
}

bool validDate(int y, int m, int d)
{
    static const uint8_t len[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (y < 1980 || y > 2199 || m < 1 || m > 12 || d < 1 || d > len[m - 1]) return false;
    if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return false;
    return true;
}

// "ddmm.mmmm" + N/S, "dddmm.mmmm" + E/W
bool coord(const Fields& f, int iv, int ih, double maxDeg, char pos, char neg, double& out)
{
    double v;
    if (!number(f, iv, v) || v < 0 || f.empty(ih) || f.n[ih] != 1) return false;
    const char h = f.p[ih][0];
    if (h != pos && h != neg) return false;
    const double deg = (double)(long)(v / 100.0);
    const double min = v - deg * 100.0;
    if (min >= 60.0) return false;
    out = deg + min / 60.0;
    if (out > maxDeg) return false;
    if (h == neg) out = -out;
    return true;
}

bool position(const Fields& f, int i, NmeaSentence& s)
{
    double la, lo;
    if (!coord(f, i, i + 1, 90.0, 'N', 'S', la) || !coord(f, i + 2, i + 3, 180.0, 'E', 'W', lo)) return false;
    s.lat = la;
    s.lon = lo;
    return true;
}

bool split(const char* body, size_t len, Fields& f)
{
    f.count = 0;
    size_t start = 0;
    for (size_t k = 0; k <= len; k++) {
        if (k == len || body[k] == ',') {
            if (f.count >= MAX_FIELDS) return false;
            f.p[f.count] = body + start;
            f.n[f.count] = (uint8_t)(k - start);
            f.count++;
            start = k + 1;
        }
    }
    return true;
}

} // namespace

int64_t nmeaUnixMs(int32_t ymd, uint32_t msOfDay)
{
    return (int64_t)ymdDayNumber(ymd) * 86400000LL + msOfDay;
}

NmeaType nmeaParse(const char* line, NmeaSentence& out)
{
    out = NmeaSentence();
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) len--;
    if (!len) return out.type = NMEA_NONE;
    // "$" body "*" hh
    if (line[0] != '$' || len < 4 + 5 || line[len - 3] != '*') return out.type = NMEA_BAD;
    const int h1 = hexVal(line[len - 2]), h2 = hexVal(line[len - 1]);
    if (h1 < 0 || h2 < 0) return out.type = NMEA_BAD;
    const char* body = line + 1;
    const size_t bodyLen = len - 4;
    uint8_t sum = 0;
    for (size_t k = 0; k < bodyLen; k++) {
        if (body[k] == '$' || body[k] == '*') return out.type = NMEA_BAD;
        sum ^= (uint8_t)body[k];
    }
    if (sum != (uint8_t)(h1 * 16 + h2)) return out.type = NMEA_BAD;

    Fields f;
    if (!split(body, bodyLen, f) || f.n[0] != 5) return out.type = NMEA_OTHER;   // proprietary ($PCAS..) or unknown
    const char* id = f.p[0];
    if (id[0] == 'P') return out.type = NMEA_OTHER;
    const char* t = id + 2;

    if (!strncmp(t, "RMC", 3)) {
        // 1 time, 2 status, 3-6 position, 7 speed, 8 course, 9 date, 10-11 magnetic variation, 12 mode
        out.type = NMEA_RMC;
        out.timeOk = timeField(f, 1, out.msOfDay);
        out.active = !f.empty(2) && f.p[2][0] == 'A';
        out.posOk = position(f, 3, out);
        if (!f.empty(9) && f.n[9] == 6) {
            const int d = digits(f.p[9], 2), m = digits(f.p[9] + 2, 2), yy = digits(f.p[9] + 4, 2);
            const int y = yy < 0 ? -1 : yy < 80 ? 2000 + yy : 1900 + yy;
            if (d >= 0 && m >= 0 && validDate(y, m, d)) { out.dateOk = true; out.ymd = y * 10000 + m * 100 + d; }
        }
        if (!f.empty(12)) out.mode = f.p[12][0];
        if (out.mode == 'N') out.active = false;   // "no fix" despite an 'A' (seen on some clones)
        return out.type;
    }
    if (!strncmp(t, "GGA", 3)) {
        // 1 time, 2-5 position, 6 quality, 7 satellites, 8 HDOP, 9 altitude, 10 'M', 11 geoid separation
        out.type = NMEA_GGA;
        out.timeOk = timeField(f, 1, out.msOfDay);
        out.posOk = position(f, 2, out);
        double v;
        if (number(f, 6, v) && v >= 0 && v <= 9) out.quality = (uint8_t)v;
        if (number(f, 7, v) && v >= 0 && v <= 99) out.sats = (uint8_t)v;
        if (number(f, 8, v) && v > 0 && v < 100) out.hdop = (float)v;
        if (number(f, 9, v) && v > -500 && v < 10000 && (f.empty(10) || f.p[10][0] == 'M')) { out.altOk = true; out.altM = (float)v; }
        if (!out.quality) out.posOk = out.altOk = false;
        return out.type;
    }
    if (!strncmp(t, "ZDA", 3)) {
        // 1 time, 2 day, 3 month, 4 year, 5-6 local zone
        out.type = NMEA_ZDA;
        out.timeOk = timeField(f, 1, out.msOfDay);
        double d, m, y;
        // in range before the cast: a huge number cast to int is undefined
        if (number(f, 2, d) && number(f, 3, m) && number(f, 4, y) && d >= 1 && d < 32 && m >= 1 && m < 13 &&
            y >= 1980 && y < 2200 && validDate((int)y, (int)m, (int)d)) {
            out.dateOk = true;
            out.ymd = (int32_t)y * 10000 + (int32_t)m * 100 + (int32_t)d;
        }
        return out.type;
    }
    return out.type = NMEA_OTHER;
}

NmeaType NmeaReader::feed(char c, NmeaSentence& out)
{
    if (c == '$') {   // a new sentence starts: whatever came before was cut off
        const bool cut = len > 0;
        len = 0;
        overflow = false;
        buf[len++] = c;
        if (cut) { out = NmeaSentence(); return out.type = NMEA_BAD; }
        return NMEA_NONE;
    }
    if (c == '\n' || c == '\r') {
        if (!len) return NMEA_NONE;
        const bool over = overflow;
        buf[len] = 0;
        len = 0;
        overflow = false;
        if (over) { out = NmeaSentence(); return out.type = NMEA_BAD; }
        return nmeaParse(buf, out);
    }
    if (!len) return NMEA_NONE;   // noise before the first '$'
    if (len >= MAX_LINE) { overflow = true; return NMEA_NONE; }
    buf[len++] = c;
    return NMEA_NONE;
}
