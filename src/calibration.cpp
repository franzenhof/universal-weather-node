#include "calibration.h"
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

float calTemp(float c)            { return c + gCfg.calTempDc / 10.0f; }
float calStationPressure(float h) { return h + gCfg.calPressDhpa / 10.0f; }
float calRain(float mm)           { return mm * gCfg.calRainPermil / 1000.0f; }

float calHumidity(float pct)
{
    float h = pct + gCfg.calHumDpct / 10.0f;
    return h < 0.0f ? 0.0f : h > 100.0f ? 100.0f : h;
}

WindStats calWind(WindStats w)
{
    const float f = gCfg.calWindPermil / 1000.0f;
    w.meanMs *= f;
    w.gustMs *= f;
    return w;
}

// Stored form per entry: "ID:t10:h10[:rain:wind:uv:lux[:flags]]" (gains in
// thousandths, flags bit 0 = WN20 preset). Entries written by older firmware
// ("ID:t10:h10") read with gains 1000.
bool rfCalGet(const char* list, uint32_t id, RfCal& out)
{
    out = RfCal();
    for (const char* p = list; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        long f[9] = { 0, 0, 1000, 1000, 1000, 1000, 0, 0, 0 };
        p = end;
        for (int k = 0; k < 9 && *p == ':'; k++) { f[k] = strtol(p + 1, &end, 10); p = end; }
        if ((uint32_t)v == id) {
            out.t10 = (int)f[0]; out.h10 = (int)f[1];
            out.rain = (uint16_t)f[2]; out.wind = (uint16_t)f[3]; out.uv = (uint16_t)f[4]; out.lux = (uint16_t)f[5];
            out.wn20 = (f[6] & 1) != 0;
            out.wh40 = (f[6] & 2) != 0;
            out.ad0 = (uint16_t)f[7]; out.ad100 = (uint16_t)f[8];
            return true;
        }
        while (*p && *p != ',') p++;
    }
    return false;
}

bool rfCalLookup(const char* list, uint32_t id, int& t10, int& h10)
{
    RfCal c;
    const bool found = rfCalGet(list, id, c);
    t10 = c.t10; h10 = c.h10;
    return found;
}

bool rfCalParseText(const char* in, char* out, size_t len)
{
    char buf[RF_CAL_LIST_LEN] = "";
    size_t n = 0;
    int count = 0;
    for (const char* p = in; p && *p;) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char* end = nullptr;
        unsigned long id = strtoul(p, &end, 16);
        if (end == p || end - p > 8 || *end != ':') return false;
        p = end + 1;
        // temp, hum, then optional gains rain, wind, uv, lux and "wn20"
        float val[6] = { 0, 0, 1, 1, 1, 1 };
        bool wn20 = false, wh40 = false;
        long ad0 = 0, ad100 = 0;
        int nv = 0;
        for (;;) {
            while (*p == ' ') p++;
            if (nv >= 1 && (!strncmp(p, "wn20", 4) || !strncmp(p, "wh40", 4) || !strncmp(p, "ad=", 3))) {
                // keywords: presets and the soil moisture calibration "ad=<AD at 0 %>-<AD at 100 %>"
                if (!strncmp(p, "wn20", 4)) { wn20 = true; p += 4; }
                else if (!strncmp(p, "wh40", 4)) { wh40 = true; p += 4; }
                else {
                    ad0 = strtol(p + 3, &end, 10);
                    if (end == p + 3 || *end != '-') return false;
                    p = end + 1;
                    ad100 = strtol(p, &end, 10);
                    if (end == p || ad0 < 0 || ad100 < 0 || ad0 > 1023 || ad100 > 1023 || ad0 == ad100) return false;
                    p = end;
                }
            } else if (nv < 6) {
                float x = strtof(p, &end);
                if (end == p) { if (nv == 0) return false; break; }
                val[nv++] = x;
                p = end;
            } else {
                break;
            }
            while (*p == ' ') p++;
            if (*p != ':') break;
            p++;
        }
        if (wn20 && wh40) return false;   // one rain preset
        while (*p == ' ') p++;
        if (*p && *p != ',' && *p != ';' && *p != '\n' && *p != '\r') return false;
        if (!(val[0] >= -10.0f && val[0] <= 10.0f && val[1] >= -20.0f && val[1] <= 20.0f)) return false;
        for (int k = 2; k < 6; k++) if (!(val[k] >= 0.5f && val[k] <= 2.0f)) return false;
        const int t10 = (int)lroundf(val[0] * 10.0f), h10 = (int)lroundf(val[1] * 10.0f);
        long g[4];
        bool gains = wn20 || wh40 || ad0 != ad100;
        for (int k = 0; k < 4; k++) { g[k] = lroundf(val[k + 2] * 1000.0f); if (g[k] != 1000) gains = true; }
        if (!t10 && !h10 && !gains) continue;   // nothing to correct
        if (++count > RF_CAL_MAX) return false;
        const int flags = (wn20 ? 1 : 0) | (wh40 ? 2 : 0);
        int k = gains ? (ad0 != ad100
                         ? snprintf(buf + n, sizeof(buf) - n, "%s%lX:%d:%d:%ld:%ld:%ld:%ld:%d:%ld:%ld", n ? "," : "", id, t10, h10,
                                    g[0], g[1], g[2], g[3], flags, ad0, ad100)
                         : snprintf(buf + n, sizeof(buf) - n, "%s%lX:%d:%d:%ld:%ld:%ld:%ld:%d", n ? "," : "", id, t10, h10,
                                    g[0], g[1], g[2], g[3], flags))
                      : snprintf(buf + n, sizeof(buf) - n, "%s%lX:%d:%d", n ? "," : "", id, t10, h10);
        if (k < 0 || (size_t)k >= sizeof(buf) - n) return false;
        n += (size_t)k;
    }
    if (n >= len) return false;
    memcpy(out, buf, n + 1);
    return true;
}

void rfCalFormatText(const char* list, char* out, size_t len)
{
    size_t n = 0;
    out[0] = '\0';
    for (const char* p = list; *p && n < len;) {
        while (*p == ',') p++;
        if (!*p) break;
        char* end = nullptr;
        unsigned long id = strtoul(p, &end, 16);
        if (end == p) break;
        RfCal c;
        rfCalGet(p, (uint32_t)id, c);
        int k = snprintf(out + n, len - n, "%s%08lX:%+.1f:%+.1f", n ? ", " : "", id, c.t10 / 10.0, c.h10 / 10.0);
        if (k >= 0 && (size_t)k < len - n &&
            (c.rain != 1000 || c.wind != 1000 || c.uv != 1000 || c.lux != 1000 || c.wn20 || c.wh40 || c.ad0 != c.ad100)) {
            n += (size_t)k;
            k = snprintf(out + n, len - n, ":%.3f:%.3f:%.3f:%.3f%s", c.rain / 1000.0, c.wind / 1000.0, c.uv / 1000.0,
                         c.lux / 1000.0, c.wn20 ? ":wn20" : c.wh40 ? ":wh40" : "");
            if (k >= 0 && (size_t)k < len - n && c.ad0 != c.ad100) {
                n += (size_t)k;
                k = snprintf(out + n, len - n, ":ad=%u-%u", c.ad0, c.ad100);
            }
        }
        if (k < 0 || (size_t)k >= len - n) { out[n] = '\0'; break; }
        n += (size_t)k;
        while (*p && *p != ',') p++;
    }
}

void rfCalOffsets(uint32_t id, float& dT, float& dH)
{
    int t10, h10;
    rfCalLookup(gCfg.rfCal, id, t10, h10);
    dT = t10 / 10.0f;
    dH = h10 / 10.0f;
}

float rfWn20Gain(float rateMmH) { return rateMmH <= 5.0f ? 0.908f : 0.773f; }

float rfRainGain(const RfCal& c, float rateMmH)
{
    if (c.wn20) return rfWn20Gain(rateMmH);   // a preset replaces the plain rain gain
    if (c.wh40) return 1.016f;                // 0.1016 mm (0.004 in) tips reported as 0.1 mm
    return c.rain / 1000.0f;
}

bool rfCalMoisture(const RfCal& c, uint16_t ad, uint8_t& pct)
{
    if (c.ad0 == c.ad100) return false;
    const long p = ((long)ad - c.ad0) * 100 / ((long)c.ad100 - c.ad0);
    pct = (uint8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
    return true;
}
