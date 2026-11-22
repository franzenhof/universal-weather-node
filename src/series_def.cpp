#include "series_def.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//                                 key        unit      step   dec agg      temp   board
const SeriesFieldInfo SERIES_FIELDS[SF_COUNT] = {
    /* SF_TEMP     */ { "temp",     "\xC2\xB0" "C", 0.1f,  1, SA_MEAN, true,  false },
    /* SF_HUM      */ { "hum",      "%",        1.0f,  0, SA_MEAN, false, false },
    /* SF_MOIST    */ { "moist",    "%",        1.0f,  0, SA_MEAN, false, false },
    /* SF_LEAF     */ { "leaf",     "%",        1.0f,  0, SA_MEAN, false, false },
    /* SF_STRIKES  */ { "strikes",  "",         1.0f,  0, SA_SUM,  false, false },
    /* SF_DIST     */ { "dist",     "km",       1.0f,  0, SA_MIN,  false, false },
    /* SF_PM25     */ { "pm25",     "\xC2\xB5g/m\xC2\xB3", 1.0f, 0, SA_MEAN, false, false },
    /* SF_PM10     */ { "pm10",     "\xC2\xB5g/m\xC2\xB3", 1.0f, 0, SA_MEAN, false, false },
    /* SF_CO2      */ { "co2",      "ppm",      1.0f,  0, SA_MEAN, false, false },
    /* SF_EC       */ { "ec",       "\xC2\xB5S/cm", 1.0f, 0, SA_MEAN, false, false },
    /* SF_PRESS    */ { "press",    "hPa",      0.1f,  1, SA_MEAN, false, false },
    /* SF_LUX      */ { "lux",      "lx",       10.0f, 0, SA_MEAN, false, false },
    /* SF_UV       */ { "uv",       "",         0.1f,  1, SA_MEAN, false, false },
    /* SF_BATT     */ { "batt",     "V",        0.01f, 2, SA_MEAN, false, false },
    /* SF_SUPERCAP */ { "supercap", "V",        0.01f, 2, SA_MEAN, false, false },
    /* SF_DS1      */ { "ds1",      "\xC2\xB0" "C", 0.1f,  1, SA_MEAN, true,  true  },
    /* SF_DS2      */ { "ds2",      "\xC2\xB0" "C", 0.1f,  1, SA_MEAN, true,  true  },
    /* SF_DS3      */ { "ds3",      "\xC2\xB0" "C", 0.1f,  1, SA_MEAN, true,  true  },
};

int seriesFieldByKey(const char* key)
{
    for (int i = 0; key && i < SF_COUNT; i++)
        if (!strcmp(SERIES_FIELDS[i].key, key)) return i;
    return -1;
}

void seriesCleanName(const char* in, char* out, size_t len)
{
    if (!len) return;
    const unsigned char* s = (const unsigned char*)(in ? in : "");
    while (*s == ' ') s++;
    size_t n = 0;
    for (; *s; s++) {
        if (*s < 0x20 || *s == 0x7F || *s == '|') continue;
        size_t need = *s < 0x80 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 : (*s & 0xF8) == 0xF0 ? 4 : 1;
        if (need > 1) {
            size_t k = 1;
            while (k < need && (s[k] & 0xC0) == 0x80) k++;
            if (k < need) continue;
        }
        if (n + need > len - 1) break;
        memmove(out + n, s, need);   // in == out allowed
        n += need;
        s += need - 1;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

size_t seriesPack(const SeriesDef defs[SERIES_MAX], char* out, size_t len)
{
    if (!len) return 0;
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < SERIES_MAX; i++) {
        const SeriesDef& d = defs[i];
        int w = d.used() ? snprintf(out + n, len - n, "%s%s|%08lX|%s", i ? "\n" : "", d.name, (unsigned long)d.rfId,
                                    SERIES_FIELDS[d.field].key)
                         : snprintf(out + n, len - n, "%s", i ? "\n" : "");
        if (w < 0 || n + (size_t)w >= len) { out[n] = '\0'; return n; }
        n += (size_t)w;
    }
    return n;
}

void seriesUnpack(const char* s, SeriesDef defs[SERIES_MAX])
{
    for (int i = 0; i < SERIES_MAX; i++) defs[i] = SeriesDef();
    if (!s) return;
    const char* p = s;
    for (int i = 0; i < SERIES_MAX && p; i++) {
        const char* end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        char line[96];
        const size_t k = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, p, k);
        line[k] = '\0';
        char* a = strchr(line, '|');
        char* b = a ? strchr(a + 1, '|') : nullptr;
        if (a && b) {
            *a = '\0';
            *b = '\0';
            char* e = nullptr;
            const unsigned long id = strtoul(a + 1, &e, 16);
            const int f = seriesFieldByKey(b + 1);
            SeriesDef d;
            seriesCleanName(line, d.name, sizeof(d.name));
            d.rfId = (uint32_t)id;
            d.field = f < 0 ? SF_COUNT : (uint8_t)f;
            if (e && *e == '\0' && d.used()) defs[i] = d;
        }
        p = end ? end + 1 : nullptr;
    }
}

uint32_t seriesKey(const SeriesDef& d)
{
    // FNV-1a over name, ID and value
    uint32_t h = 2166136261u;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    };
    mix(d.name, strlen(d.name));
    mix(&d.rfId, sizeof(d.rfId));
    mix(&d.field, 1);
    return h ? h : 1;
}

int16_t seriesEncode(uint8_t field, float v)
{
    if (field >= SF_COUNT || !isfinite(v)) return SERIES_GAP;
    const float x = roundf(v / SERIES_FIELDS[field].step);
    if (x > 32767.0f) return 32767;
    if (x < -32767.0f) return -32767;
    return (int16_t)x;
}

float seriesDecode(uint8_t field, int16_t raw)
{
    return field < SF_COUNT ? raw * SERIES_FIELDS[field].step : 0.0f;
}
