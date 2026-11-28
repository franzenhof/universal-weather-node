#include "oled_custom.h"
#include <stdio.h>
#include <string.h>

void oledCustomClean(const char* in, char* out, size_t len)
{
    if (!len) return;
    size_t n = 0;
    for (const unsigned char* s = (const unsigned char*)(in ? in : ""); *s; s++) {
        if (*s < 0x20 || *s == 0x7F) continue;   // newline, tab, ... (the stored form splits at '\n')
        // a UTF-8 character fits completely or not at all
        // (0 = a stray continuation byte 0x80-0xBF or a never-valid byte 0xF8-0xFF: dropped)
        size_t need = *s < 0x80 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 : (*s & 0xF8) == 0xF0 ? 4 : 0;
        if (!need) continue;
        if (need > 1) {
            size_t k = 1;
            while (k < need && (s[k] & 0xC0) == 0x80) k++;
            if (k < need) continue;   // broken sequence: drop the lead byte
        }
        if (n + need > len - 1) break;
        memcpy(out + n, s, need);
        n += need;
        s += need - 1;
    }
    out[n] = '\0';
}

size_t oledCustomPack(const OledCustom& c, char* out, size_t len)
{
    if (!len) return 0;
    int n = snprintf(out, len, "%u\n%s", (unsigned)c.layout, c.title);
    for (int i = 0; i < OLED_CUSTOM_LINES && n >= 0 && (size_t)n < len; i++)
        n += snprintf(out + n, len - n, "\n%s", c.line[i]);
    if (n < 0) { out[0] = '\0'; return 0; }
    return (size_t)n < len ? (size_t)n : len - 1;
}

void oledCustomUnpack(const char* s, OledCustom& c)
{
    c = OledCustom();
    if (!s || (s[0] != '0' && s[0] != '1') || s[1] != '\n') return;   // empty, or not ours
    c.layout = (uint8_t)(s[0] - '0');
    const char* p = s + 2;
    for (int f = 0; f <= OLED_CUSTOM_LINES; f++) {
        const char* end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        char field[OLED_CUSTOM_LINE * 2];
        const size_t k = n < sizeof(field) - 1 ? n : sizeof(field) - 1;
        memcpy(field, p, k);
        field[k] = '\0';
        if (f == 0) oledCustomClean(field, c.title, sizeof(c.title));
        else        oledCustomClean(field, c.line[f - 1], sizeof(c.line[f - 1]));
        if (!end) break;
        p = end + 1;
    }
}
