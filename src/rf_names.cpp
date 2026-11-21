#include "rf_names.h"
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rfNameClean(char* s)
{
    size_t n = 0;
    for (const char* p = s; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F || c == '|') continue;
        s[n++] = (char)c;
    }
    s[n] = '\0';
    while (n && s[n - 1] == ' ') s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] == ' ') lead++;
    if (lead) { memmove(s, s + lead, strlen(s + lead) + 1); n -= lead; }
    if (n > RF_NAME_MAX) {   // cut on a character boundary (no continuation byte at the cut)
        n = RF_NAME_MAX;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s[n] = '\0';
        while (n && s[n - 1] == ' ') s[--n] = '\0';
    }
}

bool rfNameLookup(const char* list, uint32_t id, char* out, size_t len)
{
    if (len) out[0] = '\0';
    for (const char* p = list; p && *p;) {
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        const char* sep = strchr(p, '|');
        const char* stop = sep ? sep : p + strlen(p);
        if (end != p && *end == ':' && (uint32_t)v == id) {
            size_t n = (size_t)(stop - end - 1);
            if (n >= len) n = len ? len - 1 : 0;
            if (len) { memcpy(out, end + 1, n); out[n] = '\0'; }
            return true;
        }
        p = sep ? sep + 1 : stop;
    }
    return false;
}

bool rfNameSet(char* list, size_t len, uint32_t id, const char* name)
{
    char clean[RF_NAME_MAX * 2 + 1];
    snprintf(clean, sizeof(clean), "%s", name ? name : "");
    rfNameClean(clean);
    // rebuild without id, then append it
    char* out = (char*)malloc(len);
    if (!out) return false;
    size_t n = 0;
    out[0] = '\0';
    bool ok = true;
    for (const char* p = list; *p;) {
        const char* sep = strchr(p, '|');
        const char* stop = sep ? sep : p + strlen(p);
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        if (end != p && *end == ':' && end < stop && (uint32_t)v != id && stop > end + 1) {
            const size_t k = (size_t)(stop - p);
            if (n + k + 2 > len) { ok = false; break; }
            if (n) out[n++] = '|';
            memcpy(out + n, p, k);
            n += k;
            out[n] = '\0';
        }
        p = sep ? sep + 1 : stop;
    }
    if (ok && clean[0]) {
        const int k = snprintf(out + n, len - n, "%s%lX:%s", n ? "|" : "", (unsigned long)id, clean);
        if (k < 0 || (size_t)k >= len - n) ok = false;
    }
    if (ok) memcpy(list, out, strlen(out) + 1);
    free(out);
    return ok;
}

const char* rfNameOf(uint32_t id)
{
    static char buf[RF_NAME_MAX + 1];
    rfNameLookup(gCfg.rfNames, id, buf, sizeof(buf));
    return buf;
}
